// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/perception.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "proto/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

TEST(PerceptionTest, CompassDirectionUsesWowAxes) {
  // +x is north, +y is west.
  EXPECT_EQ(CompassDirection(10, 0), pb::N);
  EXPECT_EQ(CompassDirection(-10, 0), pb::S);
  EXPECT_EQ(CompassDirection(0, -10), pb::E);
  EXPECT_EQ(CompassDirection(0, 10), pb::W);
  EXPECT_EQ(CompassDirection(10, -10), pb::NE);
  EXPECT_EQ(CompassDirection(-10, -10), pb::SE);
  EXPECT_EQ(CompassDirection(-10, 10), pb::SW);
  EXPECT_EQ(CompassDirection(10, 10), pb::NW);
  // Just west of north still reads as north.
  EXPECT_EQ(CompassDirection(10, 3), pb::N);
  EXPECT_EQ(CompassDirection(1, 1), pb::HERE);
}

TEST(PerceptionTest, HeightHasSlack) {
  EXPECT_EQ(HeightOf(2), pb::Relative::LEVEL);
  EXPECT_EQ(HeightOf(-2), pb::Relative::LEVEL);
  EXPECT_EQ(HeightOf(8), pb::Relative::ABOVE);
  EXPECT_EQ(HeightOf(-8), pb::Relative::BELOW);
}

TEST(PerceptionTest, LevelColorMatchesTheClient) {
  EXPECT_EQ(LevelColor(5, 15), pb::Unit::SKULL_LEVEL);
  EXPECT_EQ(LevelColor(5, 10), pb::Unit::RED);
  EXPECT_EQ(LevelColor(5, 8), pb::Unit::ORANGE);
  EXPECT_EQ(LevelColor(5, 5), pb::Unit::YELLOW);
  EXPECT_EQ(LevelColor(5, 3), pb::Unit::YELLOW);
  EXPECT_EQ(LevelColor(5, 2), pb::Unit::GREEN);
  // Grey level for 20 is 13: a level-13 mob gives no XP.
  EXPECT_EQ(LevelColor(20, 14), pb::Unit::GREEN);
  EXPECT_EQ(LevelColor(20, 13), pb::Unit::GRAY);
  EXPECT_EQ(LevelColor(80, 71), pb::Unit::GRAY);
}

TEST(PerceptionTest, ClassNames) {
  EXPECT_STREQ(ClassName(1), "Warrior");
  EXPECT_STREQ(ClassName(6), "Death Knight");
  EXPECT_STREQ(ClassName(11), "Druid");
  EXPECT_STREQ(ClassName(10), "");  // Unused id.
  EXPECT_STREQ(ClassName(0), "");
  EXPECT_STREQ(ClassName(200), "");
}

TEST(PerceptionTest, InvisibleModelsAreHidden) {
  // 11686 is the Invisible Stalker. 169 is the Infernal: visible, though
  // many trigger creatures list it as their other (GM-only) model.
  EXPECT_TRUE(IsInvisibleModel(11686));
  EXPECT_TRUE(IsInvisibleModel(6908));   // First in the table.
  EXPECT_TRUE(IsInvisibleModel(23258));  // Last in the table.
  EXPECT_FALSE(IsInvisibleModel(169));
  EXPECT_FALSE(IsInvisibleModel(0));
  EXPECT_FALSE(IsInvisibleModel(11685));  // Just before an invisible one.
  EXPECT_FALSE(IsInvisibleModel(4000000));
}

TEST(PerceptionTest, TriggersAndInvisibleModelsAreHiddenFromPlayers) {
  EXPECT_TRUE(HiddenFromPlayers(/*is_trigger=*/true, /*display_id=*/169));
  EXPECT_TRUE(HiddenFromPlayers(false, 11686));
  EXPECT_TRUE(HiddenFromPlayers(true, 11686));
  EXPECT_FALSE(HiddenFromPlayers(false, 169));
}

TEST(PerceptionTest, SizeBuckets) {
  EXPECT_EQ(SizeFromBoundingRadius(0.1f), pb::Unit::SMALL);
  EXPECT_EQ(SizeFromBoundingRadius(0.35f), pb::Unit::MEDIUM);
  EXPECT_EQ(SizeFromBoundingRadius(2.0f), pb::Unit::LARGE);
  EXPECT_EQ(SizeFromBoundingRadius(8.0f), pb::Unit::HUGE);
}

class LosCacheTest : public testing::Test {
 protected:
  LosCache cache_{/*near_yd=*/40, base::Seconds(1), base::Seconds(5)};
  const base::TimeTicks t0_ = base::TimeTicks::Now();
};

TEST_F(LosCacheTest, GetReturnsTheLastKnownResultAtAnyAge) {
  cache_.Put(/*guid=*/1, /*in_los=*/true, /*dist_yd=*/10, t0_);
  cache_.Put(/*guid=*/2, /*in_los=*/false, /*dist_yd=*/80, t0_);
  EXPECT_EQ(cache_.Get(1), true);
  EXPECT_EQ(cache_.Get(2), false);
  EXPECT_EQ(cache_.Get(3), std::nullopt);
}

TEST_F(LosCacheTest, NearResultsGoStaleSoonerThanFar) {
  cache_.Put(1, true, 10, t0_);
  cache_.Put(2, false, 80, t0_);
  const std::vector<LosCache::Candidate> candidates = {{1, 10}, {2, 80}};
  EXPECT_TRUE(
      cache_.PickChecks(candidates, t0_ + base::Milliseconds(500), 10).empty());
  EXPECT_EQ(cache_.PickChecks(candidates, t0_ + base::Seconds(2), 10),
            (std::vector<uint64_t>{1}));
  EXPECT_EQ(cache_.PickChecks(candidates, t0_ + base::Seconds(6), 10),
            (std::vector<uint64_t>{1, 2}));
}

TEST_F(LosCacheTest, PicksNeverCheckedFirstThenOldestWithinBudget) {
  cache_.Put(1, true, 10, t0_);                     // Fresh: skipped.
  cache_.Put(2, true, 20, t0_ - base::Seconds(3));  // Stale.
  cache_.Put(6, true, 2, t0_ - base::Seconds(2));   // Stale, checked later.
  const std::vector<LosCache::Candidate> candidates = {
      {1, 10}, {2, 20}, {3, 90}, {4, 5}, {5, 60}, {6, 2}};
  // Never checked (4, 5, 3 by distance), then stale (2 before 6: older).
  EXPECT_EQ(cache_.PickChecks(candidates, t0_, 4),
            (std::vector<uint64_t>{4, 5, 3, 2}));
  EXPECT_EQ(cache_.PickChecks(candidates, t0_, 10),
            (std::vector<uint64_t>{4, 5, 3, 2, 6}));
  EXPECT_TRUE(cache_.PickChecks(candidates, t0_, 0).empty());
}

TEST_F(LosCacheTest, ACrowdNextToTheBotCannotStarveFarObjects) {
  // 40 near objects that go stale every second, 10 far ones, 32 casts per
  // snapshot (one snapshot a second): every far object gets checked within a
  // few snapshots.
  std::vector<LosCache::Candidate> candidates;
  for (uint64_t guid = 1; guid <= 50; ++guid) {
    candidates.push_back({guid, guid <= 40 ? 1.0f : 80.0f});
  }
  for (int second = 0; second < 4; ++second) {
    const base::TimeTicks now = t0_ + base::Seconds(second);
    for (uint64_t guid : cache_.PickChecks(candidates, now, 32)) {
      cache_.Put(guid, true, guid <= 40 ? 1.0f : 80.0f, now);
    }
  }
  for (uint64_t guid = 41; guid <= 50; ++guid) {
    EXPECT_TRUE(cache_.Get(guid).has_value()) << guid;
  }
}

TEST_F(LosCacheTest, PruneForgetsOldResults) {
  cache_.Put(1, true, 10, t0_);
  cache_.Put(2, true, 10, t0_ + base::Seconds(20));
  cache_.Prune(t0_ + base::Seconds(31), base::Seconds(30));
  EXPECT_EQ(cache_.size(), 1u);
}

TEST_F(LosCacheTest, MovingMakesResultsDueButKeepsThem) {
  cache_.SetViewer(/*map_id=*/1, 0, 0, 0, /*moved_yd=*/3);
  cache_.Put(/*guid=*/1, /*in_los=*/false, /*dist_yd=*/80, t0_);
  // A step or two: the result holds, nothing to re-check.
  cache_.SetViewer(1, 2, 0, 0, 3);
  EXPECT_EQ(cache_.Get(1), false);
  EXPECT_TRUE(cache_.PickChecks({{1, 80}}, t0_, 32).empty());
  // Round the pillar: due for a re-check, but still known until then.
  cache_.SetViewer(1, 6, 0, 0, 3);
  EXPECT_EQ(cache_.Get(1), false);
  EXPECT_EQ(cache_.PickChecks({{1, 80}}, t0_, 32), std::vector<uint64_t>{1});
  // Re-checked from here: fresh again.
  cache_.Put(1, true, 80, t0_);
  EXPECT_TRUE(cache_.PickChecks({{1, 80}}, t0_, 32).empty());
}

TEST_F(LosCacheTest, AnotherMapForgetsEverything) {
  cache_.SetViewer(/*map_id=*/1, 0, 0, 0, /*moved_yd=*/3);
  cache_.Put(/*guid=*/1, /*in_los=*/true, /*dist_yd=*/10, t0_);
  cache_.SetViewer(/*map_id=*/2, 0, 0, 0, 3);
  EXPECT_EQ(cache_.Get(1), std::nullopt);
}

}  // namespace
}  // namespace llm_brain
