// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "scheduler.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

namespace harness {

Scheduler::Scheduler(size_t max_in_flight)
    // Zero would never start anything.
    : max_in_flight_(std::max<size_t>(max_in_flight, 1)),
      slot_busy_(max_in_flight_, false) {}

Scheduler::~Scheduler() = default;

void Scheduler::Submit(const std::string& bot, Urgency urgency, SlotWork work) {
  for (Waiting& waiting : queue_) {
    if (waiting.bot == bot) {
      // The newer wake-up replaces the stale one but keeps its place in line
      // and the stronger urgency.
      waiting.work = std::move(work);
      if (urgency == Urgency::kUrgent) {
        waiting.urgency = Urgency::kUrgent;
      }
      StartWaiting();
      return;
    }
  }
  queue_.push_back({bot, urgency, next_order_++, std::move(work)});
  StartWaiting();
}

void Scheduler::Submit(const std::string& bot, Urgency urgency, Work work) {
  Submit(
      bot, urgency,
      [work = std::move(work)](int /*slot*/, base::OnceClosure done) mutable {
        std::move(work).Run(std::move(done));
      });
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
    // The next to start: urgent first, then the longest waiting, skipping bots
    // that already have a question in progress.
    size_t best = queue_.size();
    for (size_t i = 0; i < queue_.size(); ++i) {
      if (in_flight_.count(queue_[i].bot) > 0) {
        continue;
      }
      if (best == queue_.size() || queue_[i].urgency < queue_[best].urgency ||
          (queue_[i].urgency == queue_[best].urgency &&
           queue_[i].order < queue_[best].order)) {
        best = i;
      }
    }
    if (best == queue_.size()) {
      break;
    }
    Waiting next = std::move(queue_[best]);
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(best));
    const int slot = PickSlot(next.bot);
    slot_busy_[static_cast<size_t>(slot)] = true;
    in_flight_[next.bot] = slot;
    last_slot_[next.bot] = slot;
    // The work may finish at once and its done callback may destroy this
    // scheduler: never touch a member after Run() unless it is still alive.
    base::WeakPtr<Scheduler> self = weak_factory_.GetWeakPtr();
    std::move(next.work).Run(
        slot, base::BindOnce(&Scheduler::OnDone, self, next.bot));
    if (!self) {
      return;
    }
  }
  starting_ = false;
}

void Scheduler::OnDone(const std::string& bot) {
  if (const auto it = in_flight_.find(bot); it != in_flight_.end()) {
    slot_busy_[static_cast<size_t>(it->second)] = false;
    in_flight_.erase(it);
  }
  StartWaiting();
}

}  // namespace harness
