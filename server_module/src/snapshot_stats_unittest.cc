// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot_stats.h"
#include <cstddef>

#include "base/time/time.h"
#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(SnapshotStatsTest, EmptySummary) {
  SnapshotStats stats;
  EXPECT_EQ(stats.Summary(), "n=0");
}

TEST(SnapshotStatsTest, SummarizesQuantilesAndAverages) {
  SnapshotStats stats;
  for (int i = 1; i <= 100; ++i) {
    stats.Add({base::Microseconds(i * 10), /*bytes=*/1000,
               /*units=*/static_cast<size_t>(i % 10), /*objects=*/2,
               /*hidden=*/static_cast<size_t>(i % 3), /*raycasts=*/32});
  }
  EXPECT_EQ(stats.count(), 100u);
  EXPECT_EQ(stats.Summary(),
            "n=100 build p50=510us p99=990us max=1000us total=50ms "
            "bytes avg=1000 max=1000 units avg=4 max=9 objects avg=2 max=2 "
            "hidden avg=1 max=2 raycasts avg=32 max=32");
  stats.Reset();
  EXPECT_EQ(stats.Summary(), "n=0");
}

}  // namespace
}  // namespace llm_brain
