// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "scheduler.h"

#include <map>
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
  std::map<std::string, base::OnceClosure> dones_;
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

}  // namespace
}  // namespace harness
