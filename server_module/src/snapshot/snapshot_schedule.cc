// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/snapshot_schedule.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "base/time/time.h"

namespace llm_brain {

SnapshotSchedule::SnapshotSchedule(size_t bot_count,
                                   base::TimeDelta interval,
                                   base::TimeDelta combat_interval,
                                   base::TimeTicks start)
    : interval_(std::max(interval, kMinInterval)),
      combat_interval_(combat_interval.is_positive() &&
                               combat_interval < interval_
                           ? std::max(combat_interval, kMinInterval)
                           : interval_),
      slots_(bot_count) {
  Restart(start);
}

void SnapshotSchedule::Restart(base::TimeTicks now) {
  start_ = now;
  const int64_t count = static_cast<int64_t>(slots_.size());
  for (size_t i = 0; i < slots_.size(); ++i) {
    slots_[i] = Slot{};
    slots_[i].next_due = now + interval_ * static_cast<int64_t>(i) / count;
  }
}

bool SnapshotSchedule::IsDue(size_t bot,
                             base::TimeTicks now,
                             Flags flags) const {
  const Slot& slot = slots_[bot];
  const bool core_changed =
      slot.sent_once &&
      (flags.in_combat != slot.last_flags.in_combat ||
       flags.alive != slot.last_flags.alive ||
       flags.control_mask != slot.last_flags.control_mask ||
       flags.attacker_count != slot.last_flags.attacker_count ||
       flags.active_target_pending != slot.last_flags.active_target_pending);
  const bool reactive_due = slot.sent_once && flags.reactive_event &&
                            (now - slot.last_sent >= kMinReactiveGap);
  const bool hazard_due = slot.sent_once && flags.hazard_event &&
                          (now - slot.last_sent >= kMinHazardGap);
  // A bot already fighting when the schedule (re)starts goes first: its place
  // in the interval can wait, the fight can't.
  return now >= slot.next_due || core_changed || reactive_due || hazard_due ||
         (!slot.sent_once && flags.in_combat);
}

void SnapshotSchedule::Sent(size_t bot, base::TimeTicks now, Flags flags) {
  Slot& slot = slots_[bot];
  const base::TimeDelta period = flags.in_combat ? combat_interval_ : interval_;
  if (now >= slot.next_due && period.is_positive()) {
    // The regular slot is used up. Keep the bot's place in the interval: skip
    // the slots missed during a stall rather than sending a burst to catch up.
    do {
      slot.next_due = slot.next_due + period;
    } while (slot.next_due <= now);
  }
  if (flags.in_combat && period.is_positive()) {
    const base::TimeTicks cap = now + period;
    if (slot.next_due > cap) {
      const int64_t count = static_cast<int64_t>(slots_.size());
      if (count <= 1) {
        slot.next_due = cap;
      } else {
        // Align to this bot's staggered combat phase (`start_ + k*period +
        // period*bot/N`) inside `(now, now + period]` so a simultaneous raid
        // pull does not phase-lock every bot onto the same world tick.
        const base::TimeDelta phase =
            period * static_cast<int64_t>(bot) / count;
        const int64_t p_us = period.InMicroseconds();
        const int64_t rel_us = (now - start_ - phase).InMicroseconds();
        const int64_t next_k =
            rel_us >= 0 ? (rel_us / p_us + 1) : -((-rel_us - 1) / p_us);
        slot.next_due = start_ + period * next_k + phase;
      }
    }
  }
  flags.reactive_event = false;
  flags.hazard_event = false;
  slot.last_flags = flags;
  slot.last_sent = now;
  slot.sent_once = true;
}

void SnapshotSchedule::SetActiveTargetPending(size_t bot, bool pending) {
  if (bot < slots_.size()) {
    slots_[bot].last_flags.active_target_pending = pending;
  }
}

}  // namespace llm_brain
