// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "scheduler.h"

#include <flat_map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "gtest/gtest.h"

namespace harness {
namespace {

// Records which bots were started and keeps their `done` closures, so a test
// finishes them when it wants.
class Recorder {
 public:
  Scheduler::Work Work(const std::string& bot) {
    return [this, bot](base::OnceClosure done) {
      started_.push_back(bot);
      dones_[bot] = std::move(done);
    };
  }
  // A work that finishes at once, inside Run().
  Scheduler::Work Instant(const std::string& bot) {
    return [this, bot](base::OnceClosure done) {
      started_.push_back(bot);
      std::move(done).Run();
    };
  }
  void Finish(const std::string& bot) { std::move(dones_[bot]).Run(); }
  const std::vector<std::string>& started() const { return started_; }

 private:
  std::vector<std::string> started_;
  std::flat_map<std::string, base::OnceClosure> dones_;
};

using Names = std::vector<std::string>;

TEST(SchedulerTest, StartsUpToTheLimitAndQueuesTheRest) {
  Scheduler scheduler(2);
  Recorder r;
  scheduler.Submit("a", Urgency::kNormal, r.Work("a"));
  scheduler.Submit("b", Urgency::kNormal, r.Work("b"));
  scheduler.Submit("c", Urgency::kNormal, r.Work("c"));
  EXPECT_EQ(r.started(), (Names{"a", "b"}));
  EXPECT_EQ(scheduler.in_flight(), 2u);
  EXPECT_EQ(scheduler.queued(), 1u);
}

TEST(SchedulerTest, WhenOneFinishesTheNextStarts) {
  Scheduler scheduler(1);
  Recorder r;
  scheduler.Submit("a", Urgency::kNormal, r.Work("a"));
  scheduler.Submit("b", Urgency::kNormal, r.Work("b"));
  r.Finish("a");
  EXPECT_EQ(r.started(), (Names{"a", "b"}));
  EXPECT_EQ(scheduler.in_flight(), 1u);
  r.Finish("b");
  EXPECT_EQ(scheduler.in_flight(), 0u);
  EXPECT_EQ(scheduler.queued(), 0u);
}

TEST(SchedulerTest, UrgentGoesFirstThenTheLongestWaiting) {
  Scheduler scheduler(1);
  Recorder r;
  scheduler.Submit("busy", Urgency::kNormal, r.Work("busy"));
  scheduler.Submit("n1", Urgency::kNormal, r.Work("n1"));
  scheduler.Submit("n2", Urgency::kNormal, r.Work("n2"));
  scheduler.Submit("u1", Urgency::kUrgent, r.Work("u1"));
  scheduler.Submit("u2", Urgency::kUrgent, r.Work("u2"));
  for (const char* bot : {"busy", "u1", "u2", "n1"}) {
    r.Finish(bot);
  }
  EXPECT_EQ(r.started(), (Names{"busy", "u1", "u2", "n1", "n2"}));
}

TEST(SchedulerTest, ANewerWakeUpReplacesAWaitingOneAndKeepsItsPlace) {
  Scheduler scheduler(1);
  Recorder r;
  int stale_runs = 0;
  scheduler.Submit("busy", Urgency::kNormal, r.Work("busy"));
  scheduler.Submit("a", Urgency::kNormal,
                   [&](base::OnceClosure) { ++stale_runs; });
  scheduler.Submit("b", Urgency::kNormal, r.Work("b"));
  scheduler.Submit("a", Urgency::kNormal, r.Work("a"));
  EXPECT_EQ(scheduler.queued(), 2u);
  r.Finish("busy");
  r.Finish("a");
  EXPECT_EQ(r.started(), (Names{"busy", "a", "b"}));  // a kept its place.
  EXPECT_EQ(stale_runs, 0);
}

TEST(SchedulerTest, AReplacementCanMakeTheWaitingOneUrgent) {
  Scheduler scheduler(1);
  Recorder r;
  scheduler.Submit("busy", Urgency::kNormal, r.Work("busy"));
  scheduler.Submit("a", Urgency::kNormal, r.Work("a"));
  scheduler.Submit("b", Urgency::kNormal, r.Work("b"));
  scheduler.Submit("b", Urgency::kUrgent, r.Work("b"));
  r.Finish("busy");
  EXPECT_EQ(r.started(), (Names{"busy", "b"}));
}

TEST(SchedulerTest, ABotIsNeverAskedTwiceAtOnce) {
  Scheduler scheduler(4);
  Recorder r;
  scheduler.Submit("a", Urgency::kNormal, r.Work("a"));
  scheduler.Submit("a", Urgency::kUrgent, r.Work("a"));
  EXPECT_EQ(r.started(), (Names{"a"}));  // The second waits, with room free.
  EXPECT_EQ(scheduler.in_flight(), 1u);
  EXPECT_EQ(scheduler.queued(), 1u);
  r.Finish("a");
  EXPECT_EQ(r.started(), (Names{"a", "a"}));
}

TEST(SchedulerTest, WorkThatFinishesAtOnceDoesNotBlockTheQueue) {
  Scheduler scheduler(1);
  Recorder r;
  scheduler.Submit("a", Urgency::kNormal, r.Instant("a"));
  scheduler.Submit("b", Urgency::kNormal, r.Instant("b"));
  scheduler.Submit("c", Urgency::kNormal, r.Instant("c"));
  EXPECT_EQ(r.started(), (Names{"a", "b", "c"}));
  EXPECT_EQ(scheduler.in_flight(), 0u);
}

TEST(SchedulerTest, AnAnswerThatArrivesAfterTheSchedulerIsGoneIsHarmless) {
  Recorder r;
  {
    Scheduler scheduler(1);
    scheduler.Submit("a", Urgency::kNormal, r.Work("a"));
  }
  r.Finish("a");  // Must not touch the destroyed scheduler.
  SUCCEED();
}

TEST(SchedulerTest, WorkThatDestroysTheSchedulerIsSafe) {
  // The owner may drop the scheduler from a done callback run inside Submit.
  auto scheduler = std::make_unique<Scheduler>(2);
  Scheduler::Work destroy = [&](base::OnceClosure done) {
    scheduler.reset();
    std::move(done).Run();
  };
  scheduler->Submit("a", Urgency::kNormal, std::move(destroy));
  EXPECT_EQ(scheduler, nullptr);  // And no use-after-free (ASan).
}

TEST(SchedulerTest, ALimitOfZeroStillRunsOneAtATime) {
  Scheduler scheduler(0);
  Recorder r;
  scheduler.Submit("a", Urgency::kNormal, r.Work("a"));
  EXPECT_EQ(r.started(), (Names{"a"}));
}

TEST(SchedulerTest, SpreadsBotsAcrossSlotsAndReusesIdlePreferredSlot) {
  Scheduler scheduler(3);
  std::vector<int> slots;
  auto instant_slot = [&](int slot, base::OnceClosure done) {
    slots.push_back(slot);
    std::move(done).Run();
  };
  scheduler.Submit("a", Urgency::kNormal, instant_slot);
  scheduler.Submit("b", Urgency::kNormal, instant_slot);
  scheduler.Submit("c", Urgency::kNormal, instant_slot);
  // New bots spread across 0, 1, 2 even when arriving one at a time, then each
  // reuses its own slot when idle.
  scheduler.Submit("b", Urgency::kNormal, instant_slot);
  scheduler.Submit("a", Urgency::kNormal, instant_slot);
  EXPECT_EQ(slots, (std::vector<int>{0, 1, 2, 1, 0}));
}

TEST(SchedulerTest, NeverWaitsForABusyPreferredSlot) {
  Scheduler scheduler(2);
  std::flat_map<std::string, base::OnceClosure> dones;
  std::flat_map<std::string, int> assigned;
  auto capture = [&](const std::string& bot) -> Scheduler::SlotWork {
    return [&, bot](int slot, base::OnceClosure done) {
      assigned[bot] = slot;
      dones[bot] = std::move(done);
    };
  };
  // "a" takes slot 0 and finishes.
  scheduler.Submit("a", Urgency::kNormal, capture("a"));
  EXPECT_EQ(assigned["a"], 0);
  std::move(dones["a"]).Run();

  // "b" takes slot 1, "c" takes slot 0 (occupying "a"'s preferred slot).
  scheduler.Submit("b", Urgency::kNormal, capture("b"));
  scheduler.Submit("c", Urgency::kNormal, capture("c"));
  EXPECT_EQ(assigned["b"], 1);
  EXPECT_EQ(assigned["c"], 0);

  // "b" finishes freeing slot 1 while slot 0 is still busy with "c": "a" takes
  // slot 1 immediately instead of waiting for slot 0.
  std::move(dones["b"]).Run();
  scheduler.Submit("a", Urgency::kNormal, capture("a2"));
  EXPECT_EQ(assigned["a2"], 1);
  EXPECT_EQ(scheduler.in_flight(), 2u);
  EXPECT_EQ(scheduler.queued(), 0u);
}

// A cancelable work that keeps its `done` and counts how often it is cancelled.
Scheduler::CancelableSlotWork CountingWork(
    std::flat_map<std::string, base::OnceClosure>* dones,
    std::flat_map<std::string, int>* cancels,
    const std::string& key) {
  return [dones, cancels, key](int /*slot*/, base::OnceClosure done) {
    (*dones)[key] = std::move(done);
    return base::OnceClosure([cancels, key] { ++(*cancels)[key]; });
  };
}

TEST(SchedulerTest, ACombatWakePreemptsALowerPriorityCallForTheSameBot) {
  Scheduler scheduler(2);
  std::flat_map<std::string, base::OnceClosure> dones;
  std::flat_map<std::string, int> cancels;
  scheduler.SubmitCancelable("a", Urgency::kP2Routine,
                             CountingWork(&dones, &cancels, "a1"));
  scheduler.SubmitCancelable("b", Urgency::kP2Routine,
                             CountingWork(&dones, &cancels, "b1"));

  // A P1 wake waits behind the call in flight, without cancelling it.
  scheduler.SubmitCancelable("a", Urgency::kP1Danger,
                             CountingWork(&dones, &cancels, "a2"));
  EXPECT_EQ(cancels["a1"], 0);
  EXPECT_EQ(scheduler.queued(), 1u);

  // A P0 wake cancels the obsolete call once; the other bot is untouched.
  scheduler.SubmitCancelable("a", Urgency::kP0Combat,
                             CountingWork(&dones, &cancels, "a3"));
  EXPECT_EQ(cancels["a1"], 1);
  EXPECT_EQ(cancels["b1"], 0);
  EXPECT_EQ(scheduler.queued(), 1u);  // a2 was replaced by a3.

  // A second P0 wake does not cancel the same call again.
  scheduler.SubmitCancelable("a", Urgency::kP0Combat,
                             CountingWork(&dones, &cancels, "a4"));
  EXPECT_EQ(cancels["a1"], 1);

  // The cancelled call reports done; the newest wake starts in its place.
  std::move(dones["a1"]).Run();
  EXPECT_TRUE(dones.contains("a4"));
  EXPECT_FALSE(dones.contains("a3"));
  EXPECT_EQ(scheduler.in_flight(), 2u);
  EXPECT_EQ(scheduler.queued(), 0u);
}

TEST(SchedulerTest, ACombatWakeNeverPreemptsACombatCall) {
  Scheduler scheduler(1);
  std::flat_map<std::string, base::OnceClosure> dones;
  std::flat_map<std::string, int> cancels;
  scheduler.SubmitCancelable("a", Urgency::kP0Combat,
                             CountingWork(&dones, &cancels, "a1"));
  scheduler.SubmitCancelable("a", Urgency::kP0Combat,
                             CountingWork(&dones, &cancels, "a2"));
  EXPECT_EQ(cancels["a1"], 0);
  EXPECT_EQ(scheduler.queued(), 1u);
}

TEST(SchedulerTest, CancelAllInFlightCancelsRunningCallsOnly) {
  Scheduler scheduler(2);
  std::flat_map<std::string, base::OnceClosure> dones;
  std::flat_map<std::string, int> cancels;
  scheduler.SubmitCancelable("a", Urgency::kP2Routine,
                             CountingWork(&dones, &cancels, "a"));
  scheduler.SubmitCancelable("b", Urgency::kP3Chat,
                             CountingWork(&dones, &cancels, "b"));
  scheduler.SubmitCancelable("c", Urgency::kP2Routine,
                             CountingWork(&dones, &cancels, "c"));
  scheduler.CancelAllInFlight();
  EXPECT_EQ(cancels["a"], 1);
  EXPECT_EQ(cancels["b"], 1);
  EXPECT_EQ(cancels["c"], 0);     // Still waiting: never started.
  scheduler.CancelAllInFlight();  // Each cancel runs at most once.
  EXPECT_EQ(cancels["a"], 1);
}

TEST(SchedulerTest, TheCancelOfAFinishedRunIsNeverKeptForTheNextRun) {
  Scheduler scheduler(1);
  int stale_cancels = 0;
  // Finishes inside Run(), then hands back a cancel for a call that is over.
  scheduler.SubmitCancelable(
      "a", Urgency::kP2Routine, [&](int /*slot*/, base::OnceClosure done) {
        std::move(done).Run();
        return base::OnceClosure([&] { ++stale_cancels; });
      });
  EXPECT_EQ(scheduler.in_flight(), 0u);

  // The next run of the same bot has no cancel of its own; a P0 wake must not
  // run the stale one.
  Recorder r;
  scheduler.Submit("a", Urgency::kP2Routine, r.Work("a"));
  scheduler.Submit("a", Urgency::kP0Combat, r.Work("a"));
  EXPECT_EQ(stale_cancels, 0);
}

}  // namespace
}  // namespace harness
