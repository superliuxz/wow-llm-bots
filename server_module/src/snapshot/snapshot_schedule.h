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

#ifndef MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_SCHEDULE_H_
#define MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_SCHEDULE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/time/time.h"

namespace llm_brain {

class SnapshotSchedule {
 public:
  // The facts that make a snapshot due early.
  struct Flags {
    bool in_combat = false;
    bool alive = true;
    // Bitmask of active loss-of-control states (stun, fear, charm, silence,
    // root, pacify, disarm).
    uint8_t control_mask = 0;
    // Number of attackers currently engaging the bot.
    uint32_t attacker_count = 0;
    // True while the bot's active target option (Attack / Loot) has not yet
    // completed; flipping to false triggers an immediate snapshot so step
    // completion is seen without waiting out the regular interval.
    bool active_target_pending = false;
    // True when a new cast started or a new aura appeared on the bot, its
    // target, or an attacker since the last snapshot. Triggers an immediate
    // snapshot subject to kMinReactiveGap (50 ms).
    bool reactive_event = false;
    // True when, in combat, a hostile unit nearby started a cast or channel
    // or a hostile spell area appeared since the last snapshot (a mechanic
    // that may need a step aside). Triggers an immediate snapshot subject to
    // kMinHazardGap (100 ms).
    bool hazard_event = false;

    bool operator==(const Flags&) const = default;
  };

  // `bot_count` bots, numbered 0..bot_count-1 (their order in the config). A
  // `combat_interval` that is zero or not shorter than `interval` means no
  // faster cadence in combat. Intervals shorter than kMinInterval are raised
  // to it (zero would build a snapshot every world tick).
  static constexpr base::TimeDelta kMinInterval = base::Milliseconds(100);
  // Minimum gap between reactive (cast/aura) immediate snapshots per bot.
  static constexpr base::TimeDelta kMinReactiveGap = base::Milliseconds(50);
  // Minimum gap between hazard (nearby hostile cast / area) immediate
  // snapshots per bot.
  static constexpr base::TimeDelta kMinHazardGap = base::Milliseconds(100);
  SnapshotSchedule(size_t bot_count,
                   base::TimeDelta interval,
                   base::TimeDelta combat_interval,
                   base::TimeTicks start);

  // Is a snapshot of `bot` due at `now`: its time has come, its flags are not
  // what they were at its last snapshot, or it is in combat and hasn't been
  // sent one since the schedule (re)started.
  bool IsDue(size_t bot, base::TimeTicks now, Flags flags) const;

  // A snapshot of `bot` was sent. Plans the next one.
  void Sent(size_t bot, base::TimeTicks now, Flags flags);

  // Arms or clears `active_target_pending` on `bot`'s last recorded flags when
  // a target option starts, so starting the option does not emit a redundant
  // snapshot while completing it (flipping true -> false) triggers one at once.
  void SetActiveTargetPending(size_t bot, bool pending);

  // Spreads the bots over the interval again from `now`. For a harness that
  // connects after startup: every bot is overdue by then, and without this
  // they would all be built on its first tick.
  void Restart(base::TimeTicks now);

  // When `bot`'s next regular snapshot is due.
  base::TimeTicks next_due(size_t bot) const { return slots_[bot].next_due; }

 private:
  struct Slot {
    base::TimeTicks next_due;
    base::TimeTicks last_sent;
    Flags last_flags;
    bool sent_once = false;
  };

  const base::TimeDelta interval_;
  const base::TimeDelta combat_interval_;
  base::TimeTicks start_;
  std::vector<Slot> slots_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_SCHEDULE_H_
