// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// When each bot's snapshot is built. No AzerothCore dependency, unit-tested.
//
// Every bot gets one snapshot per interval, but not all on the same tick: bot i
// of N is first due i/N of an interval after the start, so the work is spread
// over the whole interval and a snapshot is never more than one interval old
// for one bot while the others wait their turn. A bot in combat is due more
// often (combat changes in a fraction of a second), and a bot whose combat or
// life state changed since its last snapshot is due at once, so the brain
// doesn't learn about a fight up to a whole interval late.
//
// These are rates keyed on facts, not game judgments: what the brain does about
// a fight is still the brain's.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_SCHEDULE_H_
#define MOD_LLM_BRAIN_SNAPSHOT_SCHEDULE_H_

#include <cstddef>
#include <vector>

#include "base/time/time.h"

namespace llm_brain {

class SnapshotSchedule {
 public:
  // The facts that make a snapshot due early.
  struct Flags {
    bool in_combat = false;
    bool alive = true;

    bool operator==(const Flags&) const = default;
  };

  // `bot_count` bots, numbered 0..bot_count-1 (their order in the config). A
  // `combat_interval` that is zero or not shorter than `interval` means no
  // faster cadence in combat.
  SnapshotSchedule(size_t bot_count,
                   base::TimeDelta interval,
                   base::TimeDelta combat_interval,
                   base::TimeTicks start);

  // Is a snapshot of `bot` due at `now`: its time has come, or its flags are
  // not what they were at its last snapshot.
  bool IsDue(size_t bot, base::TimeTicks now, Flags flags) const;

  // A snapshot of `bot` was sent. Plans the next one.
  void Sent(size_t bot, base::TimeTicks now, Flags flags);

  // When `bot`'s next regular snapshot is due.
  base::TimeTicks next_due(size_t bot) const { return slots_[bot].next_due; }

 private:
  struct Slot {
    base::TimeTicks next_due;
    Flags last_flags;
    bool sent_once = false;
  };

  const base::TimeDelta interval_;
  const base::TimeDelta combat_interval_;
  std::vector<Slot> slots_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_SCHEDULE_H_
