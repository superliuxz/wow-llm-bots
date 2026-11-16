// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot_schedule.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "base/time/time.h"

namespace llm_brain {

SnapshotSchedule::SnapshotSchedule(size_t bot_count,
                                   base::TimeDelta interval,
                                   base::TimeDelta combat_interval,
                                   base::TimeTicks start)
    : interval_(interval),
      combat_interval_(combat_interval.is_positive() &&
                               combat_interval < interval
                           ? combat_interval
                           : interval),
      slots_(bot_count) {
  for (size_t i = 0; i < bot_count; ++i) {
    slots_[i].next_due = start + interval * static_cast<int64_t>(i) /
                                     static_cast<int64_t>(bot_count);
  }
}

bool SnapshotSchedule::IsDue(size_t bot,
                             base::TimeTicks now,
                             Flags flags) const {
  const Slot& slot = slots_[bot];
  return now >= slot.next_due || (slot.sent_once && flags != slot.last_flags);
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
    slot.next_due = std::min(slot.next_due, now + period);
  }
  slot.last_flags = flags;
  slot.sent_once = true;
}

}  // namespace llm_brain
