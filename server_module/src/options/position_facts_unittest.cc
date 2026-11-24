// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/position_facts.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

pb::OptionFacts::Spot Spot(const char* id, float x, float y) {
  pb::OptionFacts::Spot spot;
  spot.id = id;
  spot.pos = {x, y, 0};
  return spot;
}

pb::OptionFacts::Place PlaceAt(const char* id, float x, float y) {
  pb::OptionFacts::Place place;
  place.id = id;
  place.name = id;
  place.pos = {x, y, 0};
  return place;
}

pb::Objective Objective(uint32_t quest_id, bool complete, float dist_yd) {
  pb::Objective obj;
  obj.quest.emplace().quest_id = quest_id;
  obj.quest->quest = "Quest " + std::to_string(quest_id);
  obj.quest->complete = complete;
  obj.where.emplace().name = "Area " + std::to_string(quest_id);
  obj.where->rel.dist_yd = dist_yd;
  obj.where->rel.position = {dist_yd, 0, 0};
  return obj;
}

TEST(PositionFactsTest, RefreshMeasuresFromTheNewPositionAndKeepsIds) {
  PositionFacts facts;
  facts.spots = {Spot("spot:a", 100, 0), Spot("spot:b", 0, -300)};
  facts.places = {PlaceAt("place:town", -900, 0)};

  RefreshFrom({0, 0, 0}, &facts);
  ASSERT_EQ(facts.spots.size(), 2u);
  EXPECT_EQ(facts.spots[0].dir, pb::N);  // +x is north.
  EXPECT_FLOAT_EQ(facts.spots[0].dist_yd, 100);
  EXPECT_EQ(facts.spots[1].dir, pb::E);  // -y is east.
  EXPECT_EQ(facts.places[0].dir, pb::S);

  // Walking 90 yd north: spot a is now 10 yd away (too near to offer), the
  // others keep their ids with new distances.
  RefreshFrom({90, 0, 0}, &facts);
  ASSERT_EQ(facts.spots.size(), 1u);
  EXPECT_EQ(facts.spots[0].id, "spot:b");
  EXPECT_FLOAT_EQ(facts.places[0].dist_yd, 990);
}

TEST(PositionFactsTest, RefreshSortsByDistanceAndDeduplicatesCompassDirection) {
  PositionFacts facts;
  // From (0, 0): spot:far is N (100, 0), spot:near is NE (40, -40).
  facts.spots = {Spot("spot:far", 100, 0), Spot("spot:near", 40, -40)};
  RefreshFrom({0, 0, 0}, &facts);
  ASSERT_EQ(facts.spots.size(), 2u);
  EXPECT_EQ(facts.spots[0].id, "spot:near");
  EXPECT_EQ(facts.spots[0].dir, pb::NE);
  EXPECT_EQ(facts.spots[1].id, "spot:far");
  EXPECT_EQ(facts.spots[1].dir, pb::N);

  // From (0, -40): both (100, 0) and (40, -40) are N, so only the nearer is
  // kept.
  PositionFacts moved = facts;
  RefreshFrom({0, -40, 0}, &moved);
  ASSERT_EQ(moved.spots.size(), 1u);
  EXPECT_EQ(moved.spots[0].id, "spot:near");
  EXPECT_EQ(moved.spots[0].dir, pb::N);
}

TEST(PositionFactsTest, DropsWhatIsOutOfTravelRange) {
  PositionFacts facts;
  facts.places = {PlaceAt("place:near", 10, 0), PlaceAt("place:far", 3000, 0),
                  PlaceAt("place:ok", 500, 0)};
  RefreshFrom({0, 0, 0}, &facts);
  ASSERT_EQ(facts.places.size(), 1u);
  EXPECT_EQ(facts.places[0].id, "place:ok");
}

TEST(PositionFactsTest, MinimapShowsOnlyHubsInRangeMeasuredFromPos) {
  PositionFacts facts;
  pb::Place& near = facts.minimap_hubs.emplace_back();
  near.name = "Near";
  near.rel.position = {0, 120, 0};
  pb::Place& far = facts.minimap_hubs.emplace_back();
  far.name = "Far";
  far.rel.position = {190, 0, 0};

  pb::OptionFacts out;
  pb::Minimap minimap;
  AddPositionFacts(facts, {0, 0, 0}, {}, &out, &minimap);
  ASSERT_EQ(minimap.places.size(), 1u);
  EXPECT_EQ(minimap.places[0].name, "Near");
  EXPECT_FLOAT_EQ(minimap.places[0].rel.dist_yd, 120);
  EXPECT_EQ(minimap.places[0].rel.dir, pb::W);  // +y is west.
}

TEST(PositionFactsTest, ClusterDungeonHostilesGroupsRoomsAndKeepsNearestBoss) {
  const std::vector<DungeonHostile> hostiles = {
      {"Too Close Mob", {10, 0, 0}, false},
      {"Sunblade Guard", {70, 0, 0}, false},
      {"Sunblade Mage", {78, 5, 0}, false},
      {"Sunblade Sentinel", {140, 20, 0}, false},
      {"Selin Fireheart", {220, 0, 0}, true},
  };
  const std::vector<DungeonCluster> clusters = ClusterDungeonHostiles(
      {0, 0, 0}, hostiles, DungeonNavConfig{.max_cluster_candidates = 2});
  ASSERT_EQ(clusters.size(), 2u);
  EXPECT_EQ(clusters[0].label, "Sunblade Guard (2)");
  EXPECT_NEAR(clusters[0].dist_yd, 70.0f, 0.1f);
  EXPECT_FALSE(clusters[0].has_boss);
  // Nearest boss replaces last slot when beyond max_candidates trash packs.
  EXPECT_EQ(clusters[1].label, "Selin Fireheart (boss)");
  EXPECT_TRUE(clusters[1].has_boss);
}

TEST(PositionFactsTest, NearestTurnInAndNearestObjectiveBecomePlaces) {
  const std::vector<pb::Objective> tracker = {
      Objective(1, /*complete=*/true, 400), Objective(2, true, 200),
      Objective(3, false, 900), Objective(4, false, 300),
      Objective(5, false, 10)};  // Too near to walk to.
  pb::OptionFacts out;
  pb::Minimap minimap;
  AddPositionFacts({}, {0, 0, 0}, tracker, &out, &minimap);
  ASSERT_EQ(out.places.size(), 2u);
  EXPECT_EQ(out.places[0].id, "place:quest:2:turnin");
  EXPECT_EQ(out.places[0].services, "turn in: Quest 2");
  EXPECT_EQ(out.places[1].id, "place:quest:4:objective");
  EXPECT_EQ(out.places[1].services, "quest: Quest 4");
  // None is within minimap range to draw (the 10 yd one is skipped).
  EXPECT_TRUE(minimap.objective_areas.empty());
}

TEST(PositionFactsTest, ExploreRayProbesCoverAllEightCompassDirections) {
  const auto angles = ExploreRayAngles();
  ASSERT_EQ(angles.size(), 8u);
  uint32_t seen = 0;
  for (float a : angles) {
    const pb::Direction dir =
        DirFrom({0, 0, 0}, std::cos(a) * 50.0f, std::sin(a) * 50.0f);
    ASSERT_NE(dir, pb::DIRECTION_UNSPECIFIED);
    ASSERT_NE(dir, pb::HERE);
    seen |= 1u << static_cast<uint8_t>(dir);
  }
  EXPECT_EQ(std::popcount(seen), 8);

  const auto dists = ExploreRayDistancesYd();
  ASSERT_GE(dists.size(), 3u);
  for (float d : dists) {
    EXPECT_GE(d, kMinTravelDistYd);
  }
}

TEST(PositionFactsTest, WalkProgressDetectsPingPongWithoutBreakingDetours) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  WalkProgressTracker tracker{
      .progress_pos = {0, 0, 0},
      .last_progress_at = t0,
      .best_dist_to_dest = 50.0f,
      .last_closer_at = t0,
  };
  // Stationary for 10 s -> stuck.
  EXPECT_EQ(
      CheckWalkProgress({1, 0, 0}, 50.0f, t0 + base::Seconds(10), &tracker),
      WalkProgressVerdict::kStuck);

  // Moving sideways/detouring for 20 s without getting closer -> still ok.
  tracker = {.progress_pos = {0, 0, 0},
             .last_progress_at = t0,
             .best_dist_to_dest = 50.0f,
             .last_closer_at = t0};
  EXPECT_EQ(
      CheckWalkProgress({0, 10, 0}, 52.0f, t0 + base::Seconds(8), &tracker),
      WalkProgressVerdict::kContinue);
  EXPECT_EQ(
      CheckWalkProgress({0, 20, 0}, 55.0f, t0 + base::Seconds(16), &tracker),
      WalkProgressVerdict::kContinue);
  EXPECT_EQ(
      CheckWalkProgress({0, 30, 0}, 50.0f, t0 + base::Seconds(24), &tracker),
      WalkProgressVerdict::kContinue);

  // Ping-ponging for 30 s at 20 yd (outside 12 yd slack) -> stuck (not
  // arrived).
  EXPECT_EQ(
      CheckWalkProgress({0, 40, 0}, 20.0f, t0 + base::Seconds(30), &tracker),
      WalkProgressVerdict::kContinue);  // 20 < 50 - 3 resets last_closer_at.
  EXPECT_EQ(
      CheckWalkProgress({0, 50, 0}, 19.0f, t0 + base::Seconds(38), &tracker),
      WalkProgressVerdict::kContinue);
  EXPECT_EQ(
      CheckWalkProgress({0, 60, 0}, 20.0f, t0 + base::Seconds(60), &tracker),
      WalkProgressVerdict::kStuck);

  // Ping-ponging for 30 s within 12 yd of an unreachable hub centroid ->
  // arrived.
  tracker = {.progress_pos = {0, 0, 0},
             .last_progress_at = t0,
             .best_dist_to_dest = 11.5f,
             .last_closer_at = t0};
  EXPECT_EQ(
      CheckWalkProgress({6, 0, 0}, 11.5f, t0 + base::Seconds(30), &tracker),
      WalkProgressVerdict::kArrived);
}

TEST(PositionFactsTest, EmptyTrainerInteractionReturnsRefusalNotInfo) {
  const TalkOutcome broke =
      FormatTalkOutcome("Isabella", {}, "not enough money for new spells");
  EXPECT_EQ(broke.error, "not enough money for new spells");
  EXPECT_TRUE(broke.info.empty());

  const TalkOutcome nothing_new =
      FormatTalkOutcome("Isabella", {}, "no new spells to learn");
  EXPECT_EQ(nothing_new.error, "no new spells to learn");
  EXPECT_TRUE(nothing_new.info.empty());

  const TalkOutcome nothing = FormatTalkOutcome("Kaltunk", {}, "");
  EXPECT_EQ(nothing.error, "Kaltunk has nothing for you");
  EXPECT_TRUE(nothing.info.empty());

  // If another interaction succeeded (e.g. turned in a quest), it is info.
  const TalkOutcome mixed =
      FormatTalkOutcome("Isabella", {"turned in A New Plague"},
                        "not enough money for new spells");
  EXPECT_TRUE(mixed.error.empty());
  EXPECT_EQ(mixed.info, "talked to Isabella: turned in A New Plague");
}

TEST(PositionFactsTest, CorpseTakeableLootSkipsEmptyGroupRollCorpses) {
  // Dead + lootable + allowed, but 0 gold and 0 takeable slots (e.g. item
  // locked in a group roll) -> NOT lootable.
  EXPECT_FALSE(HasCorpseTakeableLoot(true, true, true, /*gold=*/0,
                                     /*takeable_slot_count=*/0));
  EXPECT_TRUE(HasCorpseTakeableLoot(true, true, true, /*gold=*/15,
                                    /*takeable_slot_count=*/0));
  EXPECT_TRUE(HasCorpseTakeableLoot(true, true, true, /*gold=*/0,
                                    /*takeable_slot_count=*/1));
  EXPECT_FALSE(HasCorpseTakeableLoot(false, true, true, 15, 1));
  EXPECT_FALSE(HasCorpseTakeableLoot(true, false, true, 15, 1));
  EXPECT_FALSE(HasCorpseTakeableLoot(true, true, false, 15, 1));
}

TEST(PositionFactsTest, ApproachOutOfLosTargetOnlyWhenValidAndHidden) {
  EXPECT_TRUE(
      ShouldApproachOutOfLosTarget(true, false, true, /*in_los=*/false));
  EXPECT_FALSE(
      ShouldApproachOutOfLosTarget(true, false, true, /*in_los=*/true));
  EXPECT_FALSE(ShouldApproachOutOfLosTarget(false, false, true, false));
  EXPECT_FALSE(ShouldApproachOutOfLosTarget(true, true, true, false));
  EXPECT_FALSE(ShouldApproachOutOfLosTarget(true, false, false, false));
}

TEST(PositionFactsTest, AttackStepClosesRangeAndFailsAfterEightSecondsIdle) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  base::TimeTicks idle_since;

  // Approaching around a corner: engages once in LOS and within 20 yd.
  EXPECT_EQ(CheckAttackStep(false, false, true, 25.0f, false, t0, &idle_since),
            AttackStepAction::kActive);
  EXPECT_EQ(CheckAttackStep(false, false, true, 18.0f, true, t0, &idle_since),
            AttackStepAction::kEngageWhileMoving);
  EXPECT_TRUE(idle_since.is_null());

  // Idle out of range/LOS -> retry walk; within range & LOS -> retry execute.
  EXPECT_EQ(CheckAttackStep(false, false, false, 30.0f, false, t0, &idle_since),
            AttackStepAction::kRetryWalk);
  EXPECT_EQ(idle_since, t0);
  EXPECT_EQ(CheckAttackStep(false, false, false, 15.0f, true,
                            t0 + base::Seconds(4), &idle_since),
            AttackStepAction::kRetryExecute);

  // Idle for 8 s -> stuck.
  EXPECT_EQ(CheckAttackStep(false, false, false, 15.0f, true,
                            t0 + base::Seconds(8), &idle_since),
            AttackStepAction::kStuck);

  // Entering combat resets idle_since.
  EXPECT_EQ(CheckAttackStep(true, false, false, 15.0f, true,
                            t0 + base::Seconds(9), &idle_since),
            AttackStepAction::kActive);
  EXPECT_TRUE(idle_since.is_null());
}

TEST(PositionFactsTest, ClearsStaleDeadTargetOnlyWhileAliveInCombat) {
  EXPECT_TRUE(ShouldClearStaleCombatTarget(
      /*in_combat=*/true, /*bot_alive=*/true, /*has_target=*/true,
      /*target_alive=*/false));
  EXPECT_FALSE(ShouldClearStaleCombatTarget(
      /*in_combat=*/true, /*bot_alive=*/true, /*has_target=*/true,
      /*target_alive=*/false, /*target_friendly=*/true));
  EXPECT_FALSE(ShouldClearStaleCombatTarget(true, true, true, true));
  EXPECT_FALSE(ShouldClearStaleCombatTarget(true, true, false, false));
  EXPECT_FALSE(ShouldClearStaleCombatTarget(false, true, true, false));
  EXPECT_FALSE(ShouldClearStaleCombatTarget(true, false, true, false));
}

}  // namespace
}  // namespace llm_brain
