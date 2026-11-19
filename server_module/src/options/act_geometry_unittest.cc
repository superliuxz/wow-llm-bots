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
  // Bot is directly +Y from anchor -> 8 yd from anchor should be (0, 8, 0).
  const pb::Vec3 bot{0.0f, 3.0f, 0.0f};

  std::vector<pb::Vec3> yd_from =
      CandidateSpotsForPlace(pb::Command_Act::YD_FROM, bot, anchor, 8.0f);
  ASSERT_GE(yd_from.size(), 10u);
  EXPECT_NEAR(yd_from[0].x, 0.0f, 1e-3f);
  EXPECT_NEAR(yd_from[0].y, 8.0f, 1e-3f);

  std::vector<pb::Vec3> max_range = CandidateSpotsForPlace(
      pb::Command_Act::MAX_RANGE, bot, anchor, 0.0f, 28.0f);
  ASSERT_GE(max_range.size(), 10u);
  EXPECT_NEAR(max_range[0].x, 0.0f, 1e-3f);
  EXPECT_NEAR(max_range[0].y, 28.0f, 1e-3f);
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

}  // namespace
}  // namespace llm_brain
