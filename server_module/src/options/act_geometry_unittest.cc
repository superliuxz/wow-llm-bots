// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/act_geometry.h"

#include <gtest/gtest.h>

#include <numbers>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr float kPi = std::numbers::pi_v<float>;

TEST(ActGeometryTest, BehindAndInFrontUseFacing) {
  // Anchor at (10, 0, 0) facing +X (0 rad), radius 2 yd -> dist = 4.5 yd.
  const AnchorGeometry anchor{
      .pos = {10.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 2.0f};
  const pb::Vec3 bot{0.0f, 0.0f, 0.0f};

  std::vector<pb::Vec3> behind =
      CandidateSpotsForPlace(pb::Command_Act::BEHIND, bot, anchor, 0.0f);
  ASSERT_GE(behind.size(), 5u);
  EXPECT_NEAR(behind[0].x, 5.5f, 1e-3f);
  EXPECT_NEAR(behind[0].y, 0.0f, 1e-3f);

  std::vector<pb::Vec3> front =
      CandidateSpotsForPlace(pb::Command_Act::IN_FRONT, bot, anchor, 0.0f);
  ASSERT_GE(front.size(), 3u);
  EXPECT_NEAR(front[0].x, 14.5f, 1e-3f);
  EXPECT_NEAR(front[0].y, 0.0f, 1e-3f);
}

TEST(ActGeometryTest, YdFromAndMaxRangeFollowBotBearing) {
  const AnchorGeometry anchor{
      .pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 1.0f};
  // Bot is directly +Y from anchor (radius 1 yd) -> 8 yd edge-to-edge is (0, 9,
  // 0).
  const pb::Vec3 bot{0.0f, 3.0f, 0.0f};

  std::vector<pb::Vec3> yd_from =
      CandidateSpotsForPlace(pb::Command_Act::YD_FROM, bot, anchor, 8.0f);
  ASSERT_GE(yd_from.size(), 10u);
  EXPECT_NEAR(yd_from[0].x, 0.0f, 1e-3f);
  EXPECT_NEAR(yd_from[0].y, 9.0f, 1e-3f);

  std::vector<pb::Vec3> max_range = CandidateSpotsForPlace(
      pb::Command_Act::MAX_RANGE, bot, anchor, 0.0f, 28.0f);
  ASSERT_GE(max_range.size(), 10u);
  EXPECT_NEAR(max_range[0].x, 0.0f, 1e-3f);
  EXPECT_NEAR(max_range[0].y, 29.0f, 1e-3f);
}

TEST(ActGeometryTest, AwayReturnsEmptyWhenAlreadyFarEnough) {
  const AnchorGeometry anchor{
      .pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 0.0f};
  // Already 15 yd away when 10 yd requested -> empty (done immediately).
  EXPECT_TRUE(
      CandidateSpotsForAway({15.0f, 0.0f, 0.0f}, anchor, 10.0f).empty());

  // 4 yd away when 10 yd requested -> first spot at (11.5, 0, 0).
  std::vector<pb::Vec3> spots =
      CandidateSpotsForAway({4.0f, 0.0f, 0.0f}, anchor, 10.0f);
  ASSERT_FALSE(spots.empty());
  EXPECT_NEAR(spots[0].x, 11.5f, 1e-3f);
  EXPECT_NEAR(spots[0].y, 0.0f, 1e-3f);

  // Unspecified yd (0) moves at least kDefaultAwayYd away from current pos.
  spots = CandidateSpotsForAway({4.0f, 0.0f, 0.0f}, anchor, 0.0f);
  ASSERT_FALSE(spots.empty());
  EXPECT_GE(Dist2D(spots[0], anchor.pos), 4.0f + kDefaultAwayYd - 1e-3f);
}

TEST(ActGeometryTest, AnchorMovedSignificantly) {
  const AnchorGeometry a{.pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f};
  AnchorGeometry b = a;
  b.pos.x = 1.0f;
  EXPECT_FALSE(AnchorMovedSignificantly(a, b, /*cares_about_facing=*/true));
  b.pos.x = 2.5f;
  EXPECT_TRUE(AnchorMovedSignificantly(a, b, /*cares_about_facing=*/false));

  AnchorGeometry c = a;
  c.facing_rad = kPi / 2.0f;
  EXPECT_FALSE(AnchorMovedSignificantly(a, c, /*cares_about_facing=*/false));
  EXPECT_TRUE(AnchorMovedSignificantly(a, c, /*cares_about_facing=*/true));
}

TEST(ActGeometryTest, KiteGeneratesTangentialArcAroundAnchor) {
  const AnchorGeometry anchor{.pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f};
  // Bot is at (10, 0, 0): first kite spot should be at 20 yd, +45 deg.
  std::vector<pb::Vec3> spots =
      CandidateSpotsForKite({10.0f, 0.0f, 0.0f}, anchor, 20.0f);
  ASSERT_GE(spots.size(), 6u);
  EXPECT_NEAR(Dist2D(spots[0], anchor.pos), 20.0f, 1e-3f);
  EXPECT_GT(spots[0].x, 0.0f);
  EXPECT_GT(spots[0].y, 0.0f);
}

TEST(ActGeometryTest, DistancesAreEdgeToEdgeWithCombatReaches) {
  // Boss with 7.5 yd combat reach + bot with 1.5 yd reach = 9.0 yd sum.
  const AnchorGeometry boss{
      .pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 7.5f};
  const float bot_reach = 1.5f;

  // Bot at (20, 0, 0) is only 11 yd edge-to-edge from boss: `away 25` must
  // NOT be empty, and must place the first spot at 25 + 1.5 + 9 = 35.5 yd
  // centre-to-centre (>= 25 yd edge-to-edge).
  std::vector<pb::Vec3> away =
      CandidateSpotsForAway({20.0f, 0.0f, 0.0f}, boss, 25.0f, bot_reach);
  ASSERT_FALSE(away.empty());
  EXPECT_NEAR(EdgeDist2D(away[0], bot_reach, boss), 26.5f, 1e-3f);

  // `away` with no number (yd=0) from 20 yd edge-to-edge moves +12 yd further.
  std::vector<pb::Vec3> bare_away =
      CandidateSpotsForAway({29.0f, 0.0f, 0.0f}, boss, 0.0f, bot_reach);
  ASSERT_FALSE(bare_away.empty());
  EXPECT_NEAR(EdgeDist2D(bare_away[0], bot_reach, boss), 20.0f + kDefaultAwayYd,
              1e-3f);

  // `15 yd from boss` places first spot at 15 yd edge-to-edge (24 yd centre).
  std::vector<pb::Vec3> yd_from =
      CandidateSpotsForPlace(pb::Command_Act::YD_FROM, {15.0f, 0.0f, 0.0f},
                             boss, 15.0f, 28.0f, bot_reach);
  ASSERT_FALSE(yd_from.empty());
  EXPECT_NEAR(EdgeDist2D(yd_from[0], bot_reach, boss), 15.0f, 1e-3f);
}

TEST(ActGeometryTest, OutOfSkipsWhenAlreadyOutsideOrBiasesTowardGroup) {
  const AnchorGeometry hazard{
      .pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 0.0f};
  // Already outside 20 yd radius (+0.5 yd buffer) -> empty (skip dodge).
  EXPECT_TRUE(
      CandidateSpotsForOutOf({21.0f, 0.0f, 0.0f}, hazard, 20.0f).empty());

  // Inside 20 yd radius at (10, 0) with tank/group at (15, 15) (+45 deg, within
  // 90 deg of outward +0 deg): first candidate moves just past 20 + margin
  // toward the group bearing (+45 deg).
  std::vector<pb::Vec3> spots = CandidateSpotsForOutOf(
      {10.0f, 0.0f, 0.0f}, hazard, 20.0f, pb::Vec3{15.0f, 15.0f, 0.0f});
  ASSERT_FALSE(spots.empty());
  EXPECT_NEAR(Dist2D(spots[0], hazard.pos), 20.0f + kDefaultOutOfMarginYd,
              1e-3f);
  EXPECT_GT(spots[0].x, 0.0f);
  EXPECT_GT(spots[0].y, 0.0f);

  // If preferred point is on the opposite side (-20, 0), do NOT walk through
  // the hazard center: stay on the outward (+X) bearing.
  std::vector<pb::Vec3> opposite = CandidateSpotsForOutOf(
      {10.0f, 0.0f, 0.0f}, hazard, 20.0f, pb::Vec3{-20.0f, 0.0f, 0.0f});
  ASSERT_FALSE(opposite.empty());
  EXPECT_GT(opposite[0].x, 20.0f);
  EXPECT_NEAR(opposite[0].y, 0.0f, 1e-3f);
}

TEST(ActGeometryTest, CanEscapeBeforeImpactChecksTimeVsRunSpeed) {
  // 10 yd from 34 yd blast (24.5 yd needed) at 7 yd/s = 3.5 s:
  // With 4.5 s left -> escapable; with 1.5 s left -> too late (skip).
  EXPECT_TRUE(CanEscapeBeforeImpact(10.0f, 34.0f, 7.0f, 4.5f));
  EXPECT_FALSE(CanEscapeBeforeImpact(10.0f, 34.0f, 7.0f, 1.5f));
  // Active persistent ground area (lands_in_s == 0) is always escapable.
  EXPECT_TRUE(CanEscapeBeforeImpact(2.0f, 8.0f, 7.0f, 0.0f));
}

}  // namespace
}  // namespace llm_brain
