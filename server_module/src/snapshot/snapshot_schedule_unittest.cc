// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/snapshot_schedule.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "base/time/time.h"
#include "gtest/gtest.h"

namespace llm_brain {
namespace {

using Flags = SnapshotSchedule::Flags;

const base::TimeTicks kStart = base::TimeTicks() + base::Seconds(100);

base::TimeTicks At(int64_t ms) {
  return kStart + base::Milliseconds(ms);
}

SnapshotSchedule Schedule(size_t bots,
                          int64_t interval_ms = 1000,
                          int64_t combat_ms = 250) {
  return SnapshotSchedule(bots, base::Milliseconds(interval_ms),
                          base::Milliseconds(combat_ms), kStart);
}

TEST(SnapshotScheduleTest, BotsAreSpreadOverTheInterval) {
  const SnapshotSchedule schedule = Schedule(4);
  for (size_t bot = 0; bot < 4; ++bot) {
    const int64_t first_due = static_cast<int64_t>(bot) * 250;
    EXPECT_TRUE(schedule.IsDue(bot, At(first_due), Flags())) << bot;
    if (first_due > 0) {
      EXPECT_FALSE(schedule.IsDue(bot, At(first_due - 1), Flags())) << bot;
    }
  }
  // At any moment only the bots whose turn it is are due.
  int due_at_start = 0;
  for (size_t bot = 0; bot < 4; ++bot) {
    due_at_start += schedule.IsDue(bot, At(0), Flags());
  }
  EXPECT_EQ(due_at_start, 1);
}

TEST(SnapshotScheduleTest, FiftyBotsGetADistinctMomentEach) {
  SnapshotSchedule schedule = Schedule(50);
  // Step through two intervals in 1 ms steps: no tick has more than one bot.
  size_t busiest = 0;
  size_t sent = 0;
  for (int64_t ms = 0; ms < 2000; ++ms) {
    size_t due = 0;
    for (size_t bot = 0; bot < 50; ++bot) {
      if (schedule.IsDue(bot, At(ms), Flags())) {
        ++due;
        schedule.Sent(bot, At(ms), Flags());
        ++sent;
      }
    }
    busiest = std::max(busiest, due);
  }
  EXPECT_EQ(busiest, 1u);
  EXPECT_EQ(sent, 100u);  // Each bot twice.
}

TEST(SnapshotScheduleTest, EachBotKeepsItsRhythm) {
  SnapshotSchedule schedule = Schedule(4);
  schedule.Sent(1, At(250), Flags());
  EXPECT_FALSE(schedule.IsDue(1, At(1249), Flags()));
  EXPECT_TRUE(schedule.IsDue(1, At(1250), Flags()));
  // A little late is not made up for by a quick next one.
  schedule.Sent(1, At(1270), Flags());
  EXPECT_FALSE(schedule.IsDue(1, At(2249), Flags()));
  EXPECT_TRUE(schedule.IsDue(1, At(2250), Flags()));
}

TEST(SnapshotScheduleTest, AStallDoesNotBecomeABurstAndKeepsTheStagger) {
  SnapshotSchedule schedule = Schedule(4);
  // The world thread stalls for over five seconds: every bot is overdue.
  for (size_t bot = 0; bot < 4; ++bot) {
    EXPECT_TRUE(schedule.IsDue(bot, At(5300), Flags()));
    schedule.Sent(bot, At(5300), Flags());
  }
  // Each sends once, and its next slot is the next point of its own grid again
  // (0, 250, 500 and 750 ms into the second), not a burst to catch up.
  EXPECT_EQ(schedule.next_due(0), At(6000));
  EXPECT_EQ(schedule.next_due(1), At(6250));
  EXPECT_EQ(schedule.next_due(2), At(5500));
  EXPECT_EQ(schedule.next_due(3), At(5750));
  for (size_t bot = 0; bot < 4; ++bot) {
    EXPECT_FALSE(schedule.IsDue(bot, At(5301), Flags()));
  }
}

TEST(SnapshotScheduleTest, InCombatABotIsDueMoreOften) {
  SnapshotSchedule schedule = Schedule(1);
  const Flags fighting{.in_combat = true};
  schedule.Sent(0, At(0), fighting);
  EXPECT_FALSE(schedule.IsDue(0, At(249), fighting));
  EXPECT_TRUE(schedule.IsDue(0, At(250), fighting));
  schedule.Sent(0, At(250), fighting);
  EXPECT_TRUE(schedule.IsDue(0, At(500), fighting));
}

TEST(SnapshotScheduleTest, ACombatIntervalThatIsNotShorterIsIgnored) {
  SnapshotSchedule schedule = Schedule(1, 1000, 0);
  const Flags fighting{.in_combat = true};
  schedule.Sent(0, At(0), fighting);
  EXPECT_FALSE(schedule.IsDue(0, At(999), fighting));
  SnapshotSchedule longer = Schedule(1, 1000, 2000);
  longer.Sent(0, At(0), fighting);
  EXPECT_FALSE(longer.IsDue(0, At(999), fighting));
}

TEST(SnapshotScheduleTest, EnteringCombatMakesTheNextSnapshotDueAtOnce) {
  SnapshotSchedule schedule = Schedule(1);
  schedule.Sent(0, At(0), Flags());
  EXPECT_FALSE(schedule.IsDue(0, At(100), Flags()));
  EXPECT_TRUE(schedule.IsDue(0, At(100), Flags{.in_combat = true}));
  // After that early snapshot the fast rhythm starts from it, not from the
  // regular slot a second away.
  schedule.Sent(0, At(100), Flags{.in_combat = true});
  EXPECT_FALSE(schedule.IsDue(0, At(349), Flags{.in_combat = true}));
  EXPECT_TRUE(schedule.IsDue(0, At(350), Flags{.in_combat = true}));
}

TEST(SnapshotScheduleTest, DyingMakesTheNextSnapshotDueAtOnce) {
  SnapshotSchedule schedule = Schedule(1);
  schedule.Sent(0, At(0), Flags());
  EXPECT_TRUE(schedule.IsDue(0, At(10), Flags{.alive = false}));
  schedule.Sent(0, At(10), Flags{.alive = false});
  EXPECT_FALSE(schedule.IsDue(0, At(500), Flags{.alive = false}));
}

TEST(SnapshotScheduleTest, AnEarlySnapshotDoesNotSkipTheRegularOne) {
  SnapshotSchedule schedule = Schedule(1);
  schedule.Sent(0, At(0), Flags());  // Next regular: 1000.
  schedule.Sent(0, At(400), Flags{.alive = false});
  EXPECT_EQ(schedule.next_due(0), At(1000));
}

TEST(SnapshotScheduleTest, LeavingCombatIsAlsoAChange) {
  SnapshotSchedule schedule = Schedule(1);
  schedule.Sent(0, At(0), Flags{.in_combat = true});
  EXPECT_TRUE(schedule.IsDue(0, At(50), Flags()));
}

TEST(SnapshotScheduleTest, ABotNeverSentWaitsForItsTurnUnlessFighting) {
  const SnapshotSchedule schedule = Schedule(2);
  EXPECT_FALSE(schedule.IsDue(1, At(499), Flags()));
  EXPECT_TRUE(schedule.IsDue(1, At(500), Flags()));
  // Already in a fight when the schedule starts: at once.
  EXPECT_TRUE(schedule.IsDue(1, At(0), Flags{.in_combat = true}));
}

TEST(SnapshotScheduleTest, NoBotsIsFine) {
  const SnapshotSchedule schedule = Schedule(0);
  (void)schedule;
}

TEST(SnapshotScheduleTest, AZeroIntervalIsRaisedToTheMinimum) {
  SnapshotSchedule schedule = Schedule(1, /*interval_ms=*/0, /*combat_ms=*/0);
  schedule.Sent(0, At(0), Flags());
  // Not due on the very next world tick, as a zero interval would be.
  EXPECT_FALSE(schedule.IsDue(0, At(50), Flags()));
  EXPECT_TRUE(schedule.IsDue(0, At(100), Flags()));
}

TEST(SnapshotScheduleTest, RestartSpreadsOverdueBotsAgain) {
  SnapshotSchedule schedule = Schedule(4);
  // The harness connects 10 s after startup: every bot is overdue.
  const base::TimeTicks connected = At(10000);
  int overdue = 0;
  for (size_t bot = 0; bot < 4; ++bot) {
    overdue += schedule.IsDue(bot, connected, Flags());
  }
  EXPECT_EQ(overdue, 4);
  schedule.Restart(connected);
  int due = 0;
  for (size_t bot = 0; bot < 4; ++bot) {
    due += schedule.IsDue(bot, connected, Flags());
  }
  EXPECT_EQ(due, 1);  // One at a time again, not a burst of four.
  EXPECT_TRUE(schedule.IsDue(3, At(10750), Flags()));
  // But a bot already fighting doesn't wait for its place.
  EXPECT_TRUE(schedule.IsDue(3, connected, Flags{/*in_combat=*/true, true}));
}

TEST(SnapshotScheduleTest, LossOfControlMakesTheNextSnapshotDueAtOnce) {
  SnapshotSchedule schedule = Schedule(1);
  schedule.Sent(0, At(0), Flags{.in_combat = true});
  EXPECT_FALSE(schedule.IsDue(0, At(20), Flags{.in_combat = true}));
  const Flags stunned{.in_combat = true, .control_mask = 0x01};
  EXPECT_TRUE(schedule.IsDue(0, At(20), stunned));
  schedule.Sent(0, At(20), stunned);
  EXPECT_FALSE(schedule.IsDue(0, At(40), stunned));
  // Regaining control also notifies the harness immediately.
  EXPECT_TRUE(schedule.IsDue(0, At(40), Flags{.in_combat = true}));
}

TEST(SnapshotScheduleTest, AttackerCountChangeMakesTheNextSnapshotDueAtOnce) {
  SnapshotSchedule schedule = Schedule(1);
  const Flags one_mob{.in_combat = true, .attacker_count = 1};
  schedule.Sent(0, At(0), one_mob);
  EXPECT_FALSE(schedule.IsDue(0, At(30), one_mob));
  const Flags add_joined{.in_combat = true, .attacker_count = 2};
  EXPECT_TRUE(schedule.IsDue(0, At(30), add_joined));
  schedule.Sent(0, At(30), add_joined);
  EXPECT_FALSE(schedule.IsDue(0, At(60), add_joined));
  // One of the two attackers dying mid-fight is also immediate.
  EXPECT_TRUE(schedule.IsDue(0, At(60), one_mob));
}

TEST(SnapshotScheduleTest, ActiveTargetCompletionMakesSnapshotDueAtOnce) {
  SnapshotSchedule schedule = Schedule(1);
  schedule.Sent(0, At(0), Flags{.in_combat = true});
  // Starting an option arms `active_target_pending` without forcing an extra
  // snapshot while the target is still alive / unlooted.
  schedule.SetActiveTargetPending(0, true);
  const Flags pending{.in_combat = true, .active_target_pending = true};
  EXPECT_FALSE(schedule.IsDue(0, At(50), pending));
  // As soon as the target dies or is looted (`active_target_pending` -> false),
  // a snapshot is due immediately without waiting out the 250 ms combat tick.
  EXPECT_TRUE(schedule.IsDue(0, At(55), Flags{.in_combat = true}));
  schedule.Sent(0, At(55), Flags{.in_combat = true});
  EXPECT_FALSE(schedule.IsDue(0, At(100), Flags{.in_combat = true}));
}

}  // namespace
}  // namespace llm_brain
