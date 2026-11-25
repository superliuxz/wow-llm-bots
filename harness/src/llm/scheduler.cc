// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "llm/scheduler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"

namespace harness {

Scheduler::Scheduler(size_t max_in_flight, base::TimeDelta aging_step)
    // Zero would never start anything.
    : max_in_flight_(std::max<size_t>(max_in_flight, 1)),
      aging_step_(aging_step),
      slot_busy_(max_in_flight_, false) {}

Scheduler::~Scheduler() = default;

void Scheduler::SetNowForTesting(
    base::RepeatingCallback<base::TimeTicks()> now_fn) {
  now_fn_ = std::move(now_fn);
}

base::TimeTicks Scheduler::Now() const {
  return now_fn_ ? now_fn_.Run() : base::TimeTicks::Now();
}

int Scheduler::EffectivePriority(const Waiting& w, base::TimeTicks now) const {
  const int base_prio = static_cast<int>(w.urgency);
  if (base_prio == 0 || !aging_step_.is_positive() || w.enqueued_at.is_null()) {
    return base_prio;
  }
  const base::TimeDelta waited = now - w.enqueued_at;
  if (!waited.is_positive()) {
    return base_prio;
  }
  const int64_t steps = waited.InMicroseconds() / aging_step_.InMicroseconds();
  // Background reflection (P4) never jumps ahead of P1/P0 gameplay wakes;
  // interactive/routine wakes (P1..P3) age up to P1 so they cannot starve
  // under continuous load (A-M7), but never to P0: a fresh combat wake always
  // goes first, ahead of old "plan done" entries.
  const int floor_prio = w.urgency == Urgency::kP4Reflect
                             ? static_cast<int>(Urgency::kP2Routine)
                             : static_cast<int>(Urgency::kP1Danger);
  return std::max(floor_prio,
                  base_prio - static_cast<int>(std::min<int64_t>(steps, 10)));
}

void Scheduler::SubmitCancelable(const std::string& bot,
                                 Urgency urgency,
                                 CancelableSlotWork work) {
  bool replaced = false;
  for (Waiting& waiting : queue_) {
    if (waiting.bot == bot) {
      // The newer wake-up replaces the stale one, keeping its place in line and
      // the stronger (numerically lower) urgency tier.
      waiting.work = std::move(work);
      waiting.urgency = std::min(waiting.urgency, urgency);
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    queue_.push_back({bot, urgency, next_order_++, Now(), std::move(work)});
  }

  // Preempt an obsolete lower-priority in-flight call for the same bot when a
  // P0 combat wake arrives (docs/design.md §6).
  if (urgency == Urgency::kP0Combat) {
    if (const auto it = in_flight_.find(bot);
        it != in_flight_.end() && it->second.urgency > Urgency::kP0Combat &&
        it->second.cancel) {
      base::OnceClosure cancel = std::move(it->second.cancel);
      base::WeakPtr<Scheduler> self = weak_factory_.GetWeakPtr();
      std::move(cancel).Run();
      if (!self) {
        return;
      }
    }
  }
  StartWaiting();
}

void Scheduler::Submit(const std::string& bot, Urgency urgency, SlotWork work) {
  SubmitCancelable(
      bot, urgency,
      [work = std::move(work)](int slot, base::OnceClosure done) mutable {
        std::move(work).Run(slot, std::move(done));
        return base::OnceClosure{};
      });
}

void Scheduler::Submit(const std::string& bot, Urgency urgency, Work work) {
  Submit(
      bot, urgency,
      [work = std::move(work)](int /*slot*/, base::OnceClosure done) mutable {
        std::move(work).Run(std::move(done));
      });
}

void Scheduler::CancelAllInFlight() {
  std::vector<base::OnceClosure> cancels;
  cancels.reserve(in_flight_.size());
  for (auto&& [bot, state] : in_flight_) {
    if (state.cancel) {
      cancels.push_back(std::move(state.cancel));
    }
  }
  base::WeakPtr<Scheduler> self = weak_factory_.GetWeakPtr();
  for (base::OnceClosure& cancel : cancels) {
    std::move(cancel).Run();
    if (!self) {
      return;
    }
  }
}

int Scheduler::PickSlot(const std::string& bot) {
  if (const auto it = last_slot_.find(bot);
      it != last_slot_.end() && !slot_busy_[static_cast<size_t>(it->second)]) {
    return it->second;
  }
  for (size_t i = 0; i < max_in_flight_; ++i) {
    const size_t s = (next_slot_ + i) % max_in_flight_;
    if (!slot_busy_[s]) {
      next_slot_ = (s + 1) % max_in_flight_;
      return static_cast<int>(s);
    }
  }
  return 0;
}

void Scheduler::StartWaiting() {
  if (starting_) {
    return;  // The loop below picks up whatever changed.
  }
  starting_ = true;
  while (in_flight_.size() < max_in_flight_) {
    // The next to start: lowest effective priority (with aging so lower tiers
    // cannot starve under load), then the longest waiting, skipping bots that
    // already have a question in progress.
    const base::TimeTicks now = Now();
    size_t best = queue_.size();
    int best_prio = 0;
    for (size_t i = 0; i < queue_.size(); ++i) {
      if (in_flight_.count(queue_[i].bot) > 0) {
        continue;
      }
      const int prio = EffectivePriority(queue_[i], now);
      if (best == queue_.size() || prio < best_prio ||
          (prio == best_prio && queue_[i].order < queue_[best].order)) {
        best = i;
        best_prio = prio;
      }
    }
    if (best == queue_.size()) {
      break;
    }
    Waiting next = std::move(queue_[best]);
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(best));
    const int slot = PickSlot(next.bot);
    const uint64_t run_id = ++next_run_id_;
    slot_busy_[static_cast<size_t>(slot)] = true;
    in_flight_[next.bot] = InFlight{run_id, slot, next.urgency, {}};
    last_slot_[next.bot] = slot;
    // The work may finish at once and its done callback may destroy this
    // scheduler: never touch a member after Run() unless it is still alive.
    base::WeakPtr<Scheduler> self = weak_factory_.GetWeakPtr();
    const std::string started_bot = next.bot;
    base::OnceClosure cancel = std::move(next.work).Run(
        slot, base::BindOnce(&Scheduler::OnDone, self, started_bot, run_id));
    if (!self) {
      return;
    }
    if (cancel) {
      if (const auto it = in_flight_.find(started_bot);
          it != in_flight_.end() && it->second.run_id == run_id) {
        it->second.cancel = std::move(cancel);
      }
    }
  }
  starting_ = false;
}

void Scheduler::OnDone(const std::string& bot, uint64_t run_id) {
  if (const auto it = in_flight_.find(bot);
      it != in_flight_.end() && it->second.run_id == run_id) {
    slot_busy_[static_cast<size_t>(it->second.slot)] = false;
    in_flight_.erase(it);
  }
  StartWaiting();
}

}  // namespace harness
