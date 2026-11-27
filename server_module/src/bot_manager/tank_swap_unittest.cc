// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/tank_swap.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(TankSwapTest, ReflexTauntActionNameClassification) {
  // All 4 WotLK tank classes' reflex taunt actions are recognized.
  EXPECT_TRUE(IsReflexTauntActionName("taunt"));
  EXPECT_TRUE(IsReflexTauntActionName("taunt spell"));
  EXPECT_TRUE(IsReflexTauntActionName("taunt on snare target"));
  EXPECT_TRUE(IsReflexTauntActionName("heroic throw taunt"));
  EXPECT_TRUE(IsReflexTauntActionName("battle shout taunt"));
  EXPECT_TRUE(IsReflexTauntActionName("mocking blow"));
  EXPECT_TRUE(IsReflexTauntActionName("challenging shout"));
  EXPECT_TRUE(IsReflexTauntActionName("hand of reckoning"));
  EXPECT_TRUE(IsReflexTauntActionName("righteous defense"));
  EXPECT_TRUE(IsReflexTauntActionName("growl"));
  EXPECT_TRUE(IsReflexTauntActionName("challenging roar"));
  EXPECT_TRUE(IsReflexTauntActionName("dark command"));

  // Adversarial counter-tests: normal tanking/defensive/healing actions are
  // never classified as reflex taunts.
  EXPECT_FALSE(IsReflexTauntActionName(""));
  EXPECT_FALSE(IsReflexTauntActionName("shield slam"));
  EXPECT_FALSE(IsReflexTauntActionName("shield wall"));
  EXPECT_FALSE(IsReflexTauntActionName("revenge"));
  EXPECT_FALSE(IsReflexTauntActionName("consecration"));
  EXPECT_FALSE(IsReflexTauntActionName("mangle (bear)"));
  EXPECT_FALSE(IsReflexTauntActionName("death strike"));
  EXPECT_FALSE(IsReflexTauntActionName("heal"));
  EXPECT_FALSE(IsReflexTauntActionName("follow"));
}

TEST(TankSwapTest, SuppressesOldTankReflexAfterBrainTaunt) {
  constexpr uint64_t kOldTank = 0x101;
  constexpr uint64_t kNewTank = 0x102;
  constexpr uint64_t kBoss = 0xF130;

  // Phase 1: New tank's brain just issued TauntAction (within grace window,
  // before aggro has even flipped). Old tank's reflex taunt must yield.
  EXPECT_TRUE(ShouldSuppressReflexTaunt({
      .in_group = true,
      .bot_guid = kOldTank,
      .target_guid = kBoss,
      .victim_is_other_living_tank = false,
      .target_taunted_by_other_tank = false,
      .has_active_claim = true,
      .claiming_tank_guid = kNewTank,
      .claiming_tank_alive = true,
      .claim_within_grace = true,
  }));

  // Phase 2: Grace window expires, but new tank now holds aggro on the boss.
  // Old tank's "lose aggro" reflex must STILL stay suppressed (no ping-pong).
  EXPECT_TRUE(ShouldSuppressReflexTaunt({
      .in_group = true,
      .bot_guid = kOldTank,
      .target_guid = kBoss,
      .victim_is_other_living_tank = true,
      .target_taunted_by_other_tank = false,
      .has_active_claim = true,
      .claiming_tank_guid = kNewTank,
      .claiming_tank_alive = true,
      .claim_within_grace = false,
  }));

  // Phase 3: Human co-tank taunts without going through brain TauntAction
  // (active SPELL_AURA_MOD_TAUNT from another living tank). Bot tank yields.
  EXPECT_TRUE(ShouldSuppressReflexTaunt({
      .in_group = true,
      .bot_guid = kOldTank,
      .target_guid = kBoss,
      .victim_is_other_living_tank = false,
      .target_taunted_by_other_tank = true,
      .has_active_claim = false,
  }));
}

TEST(TankSwapTest, AdversarialCounterTestsSoloEmergencyAndDeadCoTank) {
  constexpr uint64_t kOldTank = 0x101;
  constexpr uint64_t kNewTank = 0x102;
  constexpr uint64_t kBoss = 0xF130;

  // Solo tank: NEVER suppressed even if flags were somehow set.
  EXPECT_FALSE(ShouldSuppressReflexTaunt({
      .in_group = false,
      .bot_guid = kOldTank,
      .target_guid = kBoss,
      .victim_is_other_living_tank = true,
      .target_taunted_by_other_tank = true,
      .has_active_claim = true,
      .claiming_tank_guid = kNewTank,
      .claiming_tank_alive = true,
      .claim_within_grace = true,
  }));

  // Claiming tank died: old tank MUST be allowed to taunt back immediately!
  EXPECT_FALSE(ShouldSuppressReflexTaunt({
      .in_group = true,
      .bot_guid = kOldTank,
      .target_guid = kBoss,
      .victim_is_other_living_tank = false,
      .target_taunted_by_other_tank = false,
      .has_active_claim = true,
      .claiming_tank_guid = kNewTank,
      .claiming_tank_alive = false,
      .claim_within_grace = true,
  }));

  // Healer or DPS pulled aggro after grace window (victim is NOT a tank):
  // old tank MUST be allowed to taunt to rescue the raid!
  EXPECT_FALSE(ShouldSuppressReflexTaunt({
      .in_group = true,
      .bot_guid = kOldTank,
      .target_guid = kBoss,
      .victim_is_other_living_tank = false,
      .target_taunted_by_other_tank = false,
      .has_active_claim = true,
      .claiming_tank_guid = kNewTank,
      .claiming_tank_alive = true,
      .claim_within_grace = false,
  }));

  // Claiming tank itself loses aggro to a DPS/healer: claiming tank MUST be
  // allowed to taunt its own claimed boss!
  EXPECT_FALSE(ShouldSuppressReflexTaunt({
      .in_group = true,
      .bot_guid = kNewTank,
      .target_guid = kBoss,
      .victim_is_other_living_tank = false,
      .target_taunted_by_other_tank = false,
      .has_active_claim = true,
      .claiming_tank_guid = kNewTank,
      .claiming_tank_alive = true,
      .claim_within_grace = true,
  }));

  // No target: never suppresses.
  EXPECT_FALSE(ShouldSuppressReflexTaunt({
      .in_group = true,
      .bot_guid = kOldTank,
      .target_guid = 0,
      .victim_is_other_living_tank = true,
  }));
}

TEST(TankSwapTest, TrackerLifecycleSwapBackAndMapInstanceIsolation) {
  TankSwapTracker tracker;
  constexpr uint64_t kTankA = 0x201;
  constexpr uint64_t kTankB = 0x202;
  constexpr uint64_t kBoss = 0xF200;
  constexpr uint64_t kGroup = 77;
  constexpr uint32_t kMap = 631;  // ICC
  constexpr uint32_t kInst = 5;

  // Tank B's brain taunts Boss at t = 1000.
  tracker.RecordClaim({
      .target_guid = kBoss,
      .claiming_tank_guid = kTankB,
      .group_guid = kGroup,
      .map_id = kMap,
      .instance_id = kInst,
      .claimed_at_ms = 1000,
  });
  EXPECT_EQ(tracker.size(), 1u);

  const auto found = tracker.FindClaim(kBoss, kGroup, kMap, kInst, 2000);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->claiming_tank_guid, kTankB);
  EXPECT_TRUE(
      TankSwapTracker::IsWithinGrace(*found, 1000 + kBrainTauntGraceMs));
  EXPECT_FALSE(
      TankSwapTracker::IsWithinGrace(*found, 1000 + kBrainTauntGraceMs + 1));

  // Counter-test: different map, instance, or group never sees the claim.
  EXPECT_FALSE(tracker.FindClaim(kBoss, kGroup, kMap + 1, kInst, 2000));
  EXPECT_FALSE(tracker.FindClaim(kBoss, kGroup, kMap, kInst + 1, 2000));
  EXPECT_FALSE(tracker.FindClaim(kBoss, kGroup + 1, kMap, kInst, 2000));

  // Expired claim past kMaxTankSwapClaimAgeMs is ignored.
  EXPECT_FALSE(tracker.FindClaim(kBoss, kGroup, kMap, kInst,
                                 1000 + kMaxTankSwapClaimAgeMs + 1));

  // Tank A's brain taunts back at t = 15000: overwrites Tank B's claim cleanly.
  tracker.RecordClaim({
      .target_guid = kBoss,
      .claiming_tank_guid = kTankA,
      .group_guid = kGroup,
      .map_id = kMap,
      .instance_id = kInst,
      .claimed_at_ms = 15000,
  });
  EXPECT_EQ(tracker.size(), 1u);
  const auto swapped = tracker.FindClaim(kBoss, kGroup, kMap, kInst, 16000);
  ASSERT_TRUE(swapped.has_value());
  EXPECT_EQ(swapped->claiming_tank_guid, kTankA);

  // Map change / disconnect / logout clears claims owned by that bot.
  tracker.ClearForBot(kTankA);
  EXPECT_EQ(tracker.size(), 0u);
  EXPECT_FALSE(tracker.FindClaim(kBoss, kGroup, kMap, kInst, 16500));

  // Target death / ClearForTarget removes only that target's claim.
  tracker.RecordClaim({
      .target_guid = kBoss,
      .claiming_tank_guid = kTankB,
      .group_guid = kGroup,
      .map_id = kMap,
      .instance_id = kInst,
      .claimed_at_ms = 20000,
  });
  tracker.ClearForTarget(kBoss);
  EXPECT_EQ(tracker.size(), 0u);

  // Solo / zero-GUID claims are ignored.
  tracker.RecordClaim({
      .target_guid = kBoss,
      .claiming_tank_guid = kTankB,
      .group_guid = 0,
      .map_id = kMap,
      .instance_id = kInst,
      .claimed_at_ms = 21000,
  });
  EXPECT_EQ(tracker.size(), 0u);
}

}  // namespace
}  // namespace llm_brain
