// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/act_geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <numbers>
#include <optional>
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

TEST(ActGeometryTest, DodgePrefersSpotsClearOfUnpulledMobs) {
  const AnchorGeometry hazard{
      .pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 0.0f};
  const pb::Vec3 bot{5.0f, 0.0f, 0.0f};
  const std::vector<pb::Vec3> spots =
      CandidateSpotsForOutOf(bot, hazard, 10.0f);
  ASSERT_GE(spots.size(), 3u);
  // An unpulled pack just past the straight-out spot (+X): the first choice
  // is now a spot at least 18 yd from it; the pack-side spot moves to the
  // back but is kept.
  const pb::Vec3 pack{20.0f, 0.0f, 0.0f};
  const std::vector<pb::Vec3> ordered = PreferSpotsClearOf(spots, bot, {pack});
  ASSERT_EQ(ordered.size(), spots.size());
  EXPECT_GE(Dist2D(ordered.front(), pack), kUnpulledClearYd);
  EXPECT_LT(Dist2D(ordered.back(), pack), kUnpulledClearYd);
  // The clear tier keeps the fan's order (closest to the ideal bearing first).
  float nearest_yd = 1e9f;
  for (const pb::Vec3& s : spots) {
    nearest_yd = std::min(nearest_yd, Dist2D(bot, s));
  }
  std::vector<pb::Vec3> clear;
  for (const pb::Vec3& s : spots) {
    if (Dist2D(s, pack) >= kUnpulledClearYd &&
        Dist2D(bot, s) <= nearest_yd + kUnpulledMaxExtraWalkYd) {
      clear.push_back(s);
      // Never a long walk back through the hazard for a clear spot.
      EXPECT_LE(Dist2D(bot, s), nearest_yd + kUnpulledMaxExtraWalkYd);
    }
  }
  ASSERT_FALSE(clear.empty());
  for (size_t i = 0; i < clear.size(); ++i) {
    EXPECT_EQ(ordered[i].x, clear[i].x);
    EXPECT_EQ(ordered[i].y, clear[i].y);
  }

  // No unpulled mobs: unchanged.
  const std::vector<pb::Vec3> same = PreferSpotsClearOf(spots, bot, {});
  ASSERT_EQ(same.size(), spots.size());
  for (size_t i = 0; i < spots.size(); ++i) {
    EXPECT_EQ(same[i].x, spots[i].x);
    EXPECT_EQ(same[i].y, spots[i].y);
  }

  // Counter-test: boxed in by packs on every side, every way out of the
  // lethal hazard is still there, in the original order.
  const std::vector<pb::Vec3> boxed = PreferSpotsClearOf(spots, bot,
                                                         {{15.0f, 0.0f, 0.0f},
                                                          {-15.0f, 0.0f, 0.0f},
                                                          {0.0f, 15.0f, 0.0f},
                                                          {0.0f, -15.0f, 0.0f}},
                                                         /*clear_yd=*/40.0f);
  ASSERT_EQ(boxed.size(), spots.size());
  for (size_t i = 0; i < spots.size(); ++i) {
    EXPECT_EQ(boxed[i].x, spots[i].x);
    EXPECT_EQ(boxed[i].y, spots[i].y);
  }
}

TEST(ActGeometryTest, OutOfIncludesBotCombatReach) {
  const AnchorGeometry hazard{
      .pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 0.0f};
  // AzerothCore area auras add the target's combat reach (1.5 yd). At 9.0 yd
  // from an 8.0 yd ground area, a bot with 1.5 yd reach is still inside
  // (effective radius 9.5 + 0.5 = 10.0 yd) and must move out past
  // 9.5 + kDefaultOutOfMarginYd.
  std::vector<pb::Vec3> spots = CandidateSpotsForOutOf(
      {9.0f, 0.0f, 0.0f}, hazard, 8.0f, std::nullopt, kDefaultOutOfMarginYd,
      /*bot_radius_yd=*/1.5f);
  ASSERT_FALSE(spots.empty());
  EXPECT_NEAR(Dist2D(spots[0], hazard.pos), 9.5f + kDefaultOutOfMarginYd,
              1e-3f);

  // At 10.1 yd (>= 8.0 + 1.5 + 0.5), the bot is genuinely outside.
  EXPECT_TRUE(CandidateSpotsForOutOf({10.1f, 0.0f, 0.0f}, hazard, 8.0f,
                                     std::nullopt, kDefaultOutOfMarginYd,
                                     /*bot_radius_yd=*/1.5f)
                  .empty());
}

TEST(ActGeometryTest, MoveCutsLongCastsButLetsNearlyDoneOnesLand) {
  EXPECT_TRUE(CutCastForMove(2500, /*channel_allows_move=*/false));
  EXPECT_TRUE(CutCastForMove(kLetCastLandMs + 1, false));
  EXPECT_FALSE(CutCastForMove(kLetCastLandMs, false));
  EXPECT_FALSE(CutCastForMove(0, false));
  // A channel the caster may move in never holds the move: it isn't cut.
  EXPECT_FALSE(CutCastForMove(8000, /*channel_allows_move=*/true));
}

TEST(ActGeometryTest, OnlySpellsThatHoldTheBotStillWaitOutADodge) {
  EXPECT_TRUE(CastHoldsMoveOut(1500, /*channeled=*/false, false));
  // Instants (shocks, shouts, instant heals) still go out on the move.
  EXPECT_FALSE(CastHoldsMoveOut(0, /*channeled=*/false, false));
  EXPECT_TRUE(CastHoldsMoveOut(0, /*channeled=*/true, false));
  EXPECT_FALSE(CastHoldsMoveOut(0, /*channeled=*/true,
                                /*channel_allows_move=*/true));
}

TEST(ActGeometryTest, SafeFollowKeepsFollowersOffUnpulledPackEdge) {
  // Leader at origin facing +X (0 rad); unpulled pack directly behind at (-16,
  // 0) so the naive "directly behind" spot (-3, 0) would step closer to the
  // pack than the leader is.
  const AnchorGeometry leader{
      .pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 0.0f};
  const pb::Vec3 pack{-16.0f, 0.0f, 0.0f};
  const std::vector<pb::Vec3> spots =
      CandidateSpotsForSafeFollow({-5.0f, 0.0f, 0.0f}, leader, {pack});
  ASSERT_FALSE(spots.empty());
  // First candidate must be at least as far from `pack` as the leader (16 yd).
  EXPECT_GE(Dist2D(spots.front(), pack), Dist2D(leader.pos, pack));

  // With a pack to the left (+Y at 15 yd), the directly-behind spot (-3, 0)
  // is farther from the pack than the leader is, so it stays first.
  const pb::Vec3 left_pack{0.0f, 15.0f, 0.0f};
  const std::vector<pb::Vec3> left_spots =
      CandidateSpotsForSafeFollow({-4.0f, 0.0f, 0.0f}, leader, {left_pack});
  ASSERT_FALSE(left_spots.empty());
  EXPECT_NEAR(left_spots.front().x, -kSafeFollowDistYd, 1e-3f);
  EXPECT_NEAR(left_spots.front().y, 0.0f, 1e-3f);

  // Adversarial counter-test: boxed in by packs on every side still returns
  // all candidates (never empty).
  const std::vector<pb::Vec3> boxed =
      CandidateSpotsForSafeFollow({0.0f, 0.0f, 0.0f}, leader,
                                  {{10.0f, 0.0f, 0.0f},
                                   {-10.0f, 0.0f, 0.0f},
                                   {0.0f, 10.0f, 0.0f},
                                   {0.0f, -10.0f, 0.0f}});
  EXPECT_EQ(boxed.size(), 12u);
}

TEST(ActGeometryTest, BotEncroachesUnpulledDetectsAggroEdgeDrift) {
  const pb::Vec3 leader{0.0f, 0.0f, 0.0f};
  const pb::Vec3 pack{0.0f, 19.0f, 0.0f};
  // Follower drifted 3 yd toward the pack (16 yd < 18 yd guard and closer than
  // leader's 19 yd).
  EXPECT_TRUE(BotEncroachesUnpulled({0.0f, 3.0f, 0.0f}, leader, {pack}));
  // Follower on the safe side (22 yd from pack, farther than leader).
  EXPECT_FALSE(BotEncroachesUnpulled({0.0f, -3.0f, 0.0f}, leader, {pack}));
  // Pack far away (30 yd > guard): not encroaching.
  EXPECT_FALSE(
      BotEncroachesUnpulled({0.0f, 3.0f, 0.0f}, leader, {{0.0f, 35.0f, 0.0f}}));
  // No unpulled packs: never encroaching.
  EXPECT_FALSE(BotEncroachesUnpulled({0.0f, 3.0f, 0.0f}, leader, {}));
}

TEST(ActGeometryTest, HealerNeedsTankRepositionAndAdversarialCounterTests) {
  HealerPositionCheck base{
      .bot_alive = true,
      .is_healer = true,
      .in_group = true,
      .stay = false,
      .forced_move_active = false,
      .casting = false,
      .tank_alive = true,
      .tank_in_combat = true,
      .dist_to_tank_yd = 20.0f,
      .in_los_of_tank = true,
  };
  // Within range and in LoS: no reposition needed.
  EXPECT_FALSE(HealerNeedsTankReposition(base));

  // Beyond reposition threshold (> 34 yd): must close range.
  HealerPositionCheck far = base;
  far.dist_to_tank_yd = kHealerRepositionDistYd + 1.0f;
  EXPECT_TRUE(HealerNeedsTankReposition(far));

  // Out of LoS even at 15 yd: must reposition around the corner.
  HealerPositionCheck no_los = base;
  no_los.dist_to_tank_yd = 15.0f;
  no_los.in_los_of_tank = false;
  EXPECT_TRUE(HealerNeedsTankReposition(no_los));

  // Adversarial counter-tests: never override solo, non-healer, stay policy,
  // spread/combat position hold, active brain dodge (forced_move_active),
  // active cast, dead bot/tank, out-of-combat tank, two-tank coverage
  // (already in range & LoS of an off-tank), or an active friendly heal target
  // (e.g. Valithria Dreamwalker).
  auto check_blocked = [&](auto mutate) {
    HealerPositionCheck c = no_los;
    mutate(c);
    EXPECT_FALSE(HealerNeedsTankReposition(c));
  };
  check_blocked([](auto& c) { c.in_group = false; });
  check_blocked([](auto& c) { c.is_healer = false; });
  check_blocked([](auto& c) { c.stay = true; });
  check_blocked([](auto& c) { c.spread_or_position_hold = true; });
  check_blocked([](auto& c) { c.forced_move_active = true; });
  check_blocked([](auto& c) { c.casting = true; });
  check_blocked([](auto& c) { c.bot_alive = false; });
  check_blocked([](auto& c) { c.tank_alive = false; });
  check_blocked([](auto& c) { c.tank_in_combat = false; });
  check_blocked([](auto& c) { c.any_tank_in_range_and_los = true; });
  check_blocked([](auto& c) { c.heal_target_in_range_and_los = true; });

  // Counter-test: normal single-tank leash still fires when the healer is NOT
  // spread, NOT covering another tank, and NOT on a friendly heal target.
  HealerPositionCheck normal_far = far;
  normal_far.spread_or_position_hold = false;
  normal_far.any_tank_in_range_and_los = false;
  normal_far.heal_target_in_range_and_los = false;
  EXPECT_TRUE(HealerNeedsTankReposition(normal_far));
}

TEST(ActGeometryTest, SpreadAssignsDistinctEvenlySpacedBearings) {
  const AnchorGeometry center{
      .pos = {100.0f, 200.0f, 10.0f}, .facing_rad = 0.0f, .radius_yd = 0.0f};
  // 5-bot raid: all 5 slots get distinct spots at ~10 yd from center and
  // >11 yd from each other.
  std::vector<pb::Vec3> five;
  for (size_t i = 0; i < 5; ++i) {
    const std::vector<pb::Vec3> cands = CandidateSpotsForSpread(
        center.pos, center, i, 5, kDefaultSpreadRadiusYd);
    ASSERT_GT(cands.size(), 5u);
    EXPECT_NEAR(Dist2D(cands.front(), center.pos), kDefaultSpreadRadiusYd,
                1e-2f);
    five.push_back(cands.front());
  }
  for (size_t i = 0; i < five.size(); ++i) {
    for (size_t j = i + 1; j < five.size(); ++j) {
      EXPECT_GT(Dist2D(five[i], five[j]), 11.0f);
    }
  }
  // 10-bot raid (>6): concentric rings keep >= 12 yd pairwise spacing.
  std::vector<pb::Vec3> ten;
  for (size_t i = 0; i < 10; ++i) {
    const std::vector<pb::Vec3> cands = CandidateSpotsForSpread(
        center.pos, center, i, 10, kDefaultSpreadRadiusYd);
    ASSERT_FALSE(cands.empty());
    ten.push_back(cands.front());
  }
  for (size_t i = 0; i < ten.size(); ++i) {
    for (size_t j = i + 1; j < ten.size(); ++j) {
      EXPECT_GE(Dist2D(ten[i], ten[j]), 11.9f);
    }
  }

  // 25-man raid: multi-ring layout keeps every pair >= 12 yd apart while
  // staying within 36 yd (40 yd heal/spell range) of the center.
  std::vector<pb::Vec3> twenty_five;
  for (size_t i = 0; i < 25; ++i) {
    const std::vector<pb::Vec3> cands = CandidateSpotsForSpread(
        center.pos, center, i, 25, kDefaultSpreadRadiusYd);
    ASSERT_FALSE(cands.empty());
    EXPECT_LE(Dist2D(cands.front(), center.pos), 36.1f);
    twenty_five.push_back(cands.front());
  }
  float min_pair_yd = 1e9f;
  for (size_t i = 0; i < twenty_five.size(); ++i) {
    for (size_t j = i + 1; j < twenty_five.size(); ++j) {
      min_pair_yd =
          std::min(min_pair_yd, Dist2D(twenty_five[i], twenty_five[j]));
    }
  }
  EXPECT_GE(min_pair_yd, 11.9f);
}

TEST(ActGeometryTest, OutOfRetryRotatesEscapeVectorAndIncludesTightFallback) {
  const AnchorGeometry hazard{
      .pos = {0.0f, 0.0f, 0.0f}, .facing_rad = 0.0f, .radius_yd = 0.0f};
  const pb::Vec3 bot{5.0f, 0.0f, 0.0f};  // Directly +X from center.

  // Attempt 0 goes straight outward (+X).
  const std::vector<pb::Vec3> a0 =
      CandidateSpotsForOutOf(bot, hazard, 10.0f, std::nullopt,
                             kDefaultOutOfMarginYd, 0.0f, /*attempt=*/0);
  ASSERT_GT(a0.size(), 12u);  // 12 primary + 8 tight fallback spots.
  EXPECT_NEAR(a0.front().x, 10.0f + kDefaultOutOfMarginYd, 1e-2f);
  EXPECT_NEAR(a0.front().y, 0.0f, 1e-2f);
  // Tight fallback ring is strictly outside hazard radius (11.0 yd).
  EXPECT_NEAR(Dist2D(a0.back(), hazard.pos), 11.0f, 1e-2f);

  // Attempt 1 rotates +90 deg (+Y), ignoring preferred_dir_pos on +X so a
  // blocked path tries an alternate escape vector first.
  const std::vector<pb::Vec3> a1 =
      CandidateSpotsForOutOf(bot, hazard, 10.0f, pb::Vec3{20.0f, 0.0f, 0.0f},
                             kDefaultOutOfMarginYd, 0.0f, /*attempt=*/1);
  ASSERT_FALSE(a1.empty());
  EXPECT_NEAR(a1.front().x, 0.0f, 1e-2f);
  EXPECT_NEAR(a1.front().y, 10.0f + kDefaultOutOfMarginYd, 1e-2f);

  // Attempt 2 rotates -90 deg (-Y), attempt 3 rotates 180 deg (-X).
  const std::vector<pb::Vec3> a2 =
      CandidateSpotsForOutOf(bot, hazard, 10.0f, std::nullopt,
                             kDefaultOutOfMarginYd, 0.0f, /*attempt=*/2);
  EXPECT_NEAR(a2.front().y, -(10.0f + kDefaultOutOfMarginYd), 1e-2f);
  const std::vector<pb::Vec3> a3 =
      CandidateSpotsForOutOf(bot, hazard, 10.0f, std::nullopt,
                             kDefaultOutOfMarginYd, 0.0f, /*attempt=*/3);
  EXPECT_NEAR(a3.front().x, -(10.0f + kDefaultOutOfMarginYd), 1e-2f);
}

}  // namespace
}  // namespace llm_brain
