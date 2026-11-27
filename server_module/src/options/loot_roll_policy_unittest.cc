// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/loot_roll_policy.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "bot_manager/popup_watch.h"
#include "gtest/gtest.h"

namespace llm_brain {
namespace {

constexpr uint8_t kClassWarrior = 1;
constexpr uint8_t kClassPaladin = 2;
constexpr uint8_t kClassHunter = 3;
constexpr uint8_t kClassRogue = 4;
constexpr uint8_t kClassPriest = 5;
constexpr uint8_t kClassDeathKnight = 6;
constexpr uint8_t kClassShaman = 7;
constexpr uint8_t kClassMage = 8;
constexpr uint8_t kClassDruid = 11;

constexpr uint8_t kStandardMask =
    kRollMaskPass | kRollMaskNeed | kRollMaskGreed;
constexpr uint8_t kDisenchantMask = kStandardMask | kRollMaskDisenchant;

LootRollPolicyInput ValidUpgradeInput(uint8_t mask = kStandardMask) {
  LootRollPolicyInput in;
  in.roll_vote_mask = mask;
  in.usable = true;
  in.bind_on_equip = false;
  in.armor_type_matches = true;
  in.shield_disallowed_for_spec = false;
  in.has_upgrade_slot = true;
  in.stat_fit = StatFit::kMatch;
  return in;
}

TEST(LootRollPolicyTest, DeferralWindowIsStrictlyShorterThanPopupFallback) {
  static_assert(kHumanLootRollDeferMs < kPopupFallbackDeadlineMs);
  EXPECT_LT(kHumanLootRollDeferMs, kPopupFallbackDeadlineMs);
}

TEST(LootRollPolicyTest, AllBotGroupVotesImmediatelyWithoutDeferral) {
  LootRollPolicyInput in = ValidUpgradeInput(kDisenchantMask);
  in.human_pending_vote = false;
  in.roll_elapsed_ms = 0;
  // Valid upgrade in all-bot group: Need immediately (even when Disenchant is
  // available).
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kNeed);

  // Non-upgrade with Disenchant available: Disenchant immediately.
  in.has_upgrade_slot = false;
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kDisenchant);

  // Non-upgrade without Disenchant available: Greed immediately.
  in.roll_vote_mask = kStandardMask;
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kGreed);

  // Only Pass available: Pass.
  in.roll_vote_mask = kRollMaskPass;
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kPass);
}

TEST(LootRollPolicyTest, DefersWhileHumanPendingThenRespectsHumanNeed) {
  LootRollPolicyInput in = ValidUpgradeInput(kDisenchantMask);
  in.human_pending_vote = true;
  in.roll_elapsed_ms = 0;
  EXPECT_EQ(DecidePolicyRollVote(in), std::nullopt);

  in.roll_elapsed_ms = kHumanLootRollDeferMs - 1;
  EXPECT_EQ(DecidePolicyRollVote(in), std::nullopt);

  // Human clicks Need at t = 2 s: bot immediately yields to Disenchant (never
  // Need against the human).
  in.roll_elapsed_ms = 2000;
  in.human_pending_vote = false;
  in.human_already_needed = true;
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kDisenchant);

  // Without an enchanter in the group, yields to Greed instead.
  in.roll_vote_mask = kStandardMask;
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kGreed);
}

TEST(LootRollPolicyTest, VotesNeedImmediatelyAfterHumanGreedsOrPasses) {
  LootRollPolicyInput in = ValidUpgradeInput(kDisenchantMask);
  in.human_pending_vote = true;
  in.roll_elapsed_ms = 500;
  EXPECT_EQ(DecidePolicyRollVote(in), std::nullopt);

  // Human clicked Greed or Pass: no longer pending, did not Need.
  in.human_pending_vote = false;
  in.human_already_needed = false;
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kNeed);
}

// Adversarial counter-test: if a human in the group is AFK and never votes on
// the roll, the bot falls back at kHumanLootRollDeferMs (before the 15 s
// popup-fallback deadline and 60 s WoW roll timer) and casts its vote.
TEST(LootRollPolicyTest, HumanTimeoutFallbackCastsVoteBeforeExpiry) {
  LootRollPolicyInput in = ValidUpgradeInput(kDisenchantMask);
  in.human_pending_vote = true;
  in.roll_elapsed_ms = kHumanLootRollDeferMs;
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kNeed);

  // Adversarial: if one human already Needed and a second human is AFK until
  // timeout, the fallback still respects the first human's Need.
  in.human_already_needed = true;
  EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kDisenchant);
}

TEST(LootRollPolicyTest, BoeAndOffSpecAndWrongArmorNeverNeed) {
  // BoE upgrade -> Disenchant if eligible, else Greed.
  {
    LootRollPolicyInput in = ValidUpgradeInput(kDisenchantMask);
    in.bind_on_equip = true;
    EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kDisenchant);
    in.roll_vote_mask = kStandardMask;
    EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kGreed);
  }
  // Off-spec stat mismatch -> never Need.
  {
    LootRollPolicyInput in = ValidUpgradeInput(kDisenchantMask);
    in.stat_fit = StatFit::kMismatch;
    EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kDisenchant);
  }
  // Wrong armor type -> never Need.
  {
    LootRollPolicyInput in = ValidUpgradeInput(kStandardMask);
    in.armor_type_matches = false;
    EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kGreed);
  }
  // Shield disallowed for non-tank melee spec -> never Need.
  {
    LootRollPolicyInput in = ValidUpgradeInput(kStandardMask);
    in.shield_disallowed_for_spec = true;
    EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kGreed);
  }
  // Neutral stats (e.g. Stamina-only or plain armor) -> Need allowed.
  {
    LootRollPolicyInput in = ValidUpgradeInput(kDisenchantMask);
    in.stat_fit = StatFit::kNeutral;
    EXPECT_EQ(DecidePolicyRollVote(in), LootRollChoice::kNeed);
  }
}

TEST(LootRollPolicyTest, StatFitMatchesWotlkClassAndSpecProfiles) {
  const ItemStatFlags caster_stats{
      .intellect = true, .spirit = true, .stamina = true, .spell_power = true};
  const ItemStatFlags str_dps_stats{
      .strength = true, .stamina = true, .attack_power = true};
  const ItemStatFlags tank_stats{
      .strength = true, .stamina = true, .tank_avoidance = true};
  const ItemStatFlags agi_dps_stats{
      .agility = true, .stamina = true, .attack_power = true};
  const ItemStatFlags hunter_mail_stats{
      .agility = true, .intellect = true, .stamina = true};
  const ItemStatFlags stamina_only{.stamina = true};

  // Fury Warrior (melee strength): wants str DPS, rejects Holy Paladin plate,
  // pure tank avoidance, and pure agility rings/cloaks.
  const LootStatProfile fury = LootStatProfileForSpec(
      kClassWarrior, /*spec_tab=*/1, /*has_talents=*/true,
      /*is_tank=*/false, /*is_healer=*/false);
  EXPECT_EQ(fury, LootStatProfile::kMeleeStrength);
  EXPECT_TRUE(IsShieldDisallowedForProfile(fury));
  EXPECT_EQ(EvaluateStatFit(fury, kClassWarrior, str_dps_stats),
            StatFit::kMatch);
  EXPECT_EQ(EvaluateStatFit(fury, kClassWarrior, caster_stats),
            StatFit::kMismatch);
  EXPECT_EQ(
      EvaluateStatFit(fury, kClassWarrior, agi_dps_stats),
      StatFit::kMatch);  // Has attack_power; pure agility below is rejected.
  EXPECT_EQ(
      EvaluateStatFit(fury, kClassWarrior, ItemStatFlags{.agility = true}),
      StatFit::kMismatch);
  EXPECT_EQ(EvaluateStatFit(fury, kClassWarrior,
                            ItemStatFlags{.tank_avoidance = true}),
            StatFit::kMismatch);
  EXPECT_EQ(EvaluateStatFit(fury, kClassWarrior, stamina_only),
            StatFit::kNeutral);

  // Protection Warrior (tank): wants tank/str gear & shields, rejects caster.
  const LootStatProfile prot = LootStatProfileForSpec(
      kClassWarrior, /*spec_tab=*/2, /*has_talents=*/true,
      /*is_tank=*/true, /*is_healer=*/false);
  EXPECT_EQ(prot, LootStatProfile::kTankStrength);
  EXPECT_FALSE(IsShieldDisallowedForProfile(prot));
  EXPECT_EQ(EvaluateStatFit(prot, kClassWarrior, tank_stats), StatFit::kMatch);
  EXPECT_EQ(EvaluateStatFit(prot, kClassWarrior, caster_stats),
            StatFit::kMismatch);

  // Holy Paladin (healer): wants caster plate & shields, rejects str/tank.
  const LootStatProfile holy_pal = LootStatProfileForSpec(
      kClassPaladin, /*spec_tab=*/0, /*has_talents=*/true,
      /*is_tank=*/false, /*is_healer=*/true);
  EXPECT_EQ(holy_pal, LootStatProfile::kSpellCaster);
  EXPECT_FALSE(IsShieldDisallowedForProfile(holy_pal));
  EXPECT_EQ(EvaluateStatFit(holy_pal, kClassPaladin, caster_stats),
            StatFit::kMatch);
  EXPECT_EQ(EvaluateStatFit(holy_pal, kClassPaladin, tank_stats),
            StatFit::kMismatch);

  // Hunter: wants Agi+Int mail, rejects pure caster mail and pure Str gear.
  const LootStatProfile hunter =
      LootStatProfileForSpec(kClassHunter, /*spec_tab=*/0, /*has_talents=*/true,
                             /*is_tank=*/false, /*is_healer=*/false);
  EXPECT_EQ(EvaluateStatFit(hunter, kClassHunter, hunter_mail_stats),
            StatFit::kMatch);
  EXPECT_EQ(EvaluateStatFit(hunter, kClassHunter, caster_stats),
            StatFit::kMismatch);
  EXPECT_EQ(
      EvaluateStatFit(hunter, kClassHunter, ItemStatFlags{.strength = true}),
      StatFit::kMismatch);

  // Rogue: rejects Int-only leather, accepts Agi leather.
  const LootStatProfile rogue =
      LootStatProfileForSpec(kClassRogue, /*spec_tab=*/1, /*has_talents=*/true,
                             /*is_tank=*/false, /*is_healer=*/false);
  EXPECT_EQ(
      EvaluateStatFit(rogue, kClassRogue, ItemStatFlags{.intellect = true}),
      StatFit::kMismatch);
  EXPECT_EQ(EvaluateStatFit(rogue, kClassRogue, agi_dps_stats),
            StatFit::kMatch);

  // Priest / Mage: accepts caster stats, rejects physical stats.
  const LootStatProfile priest =
      LootStatProfileForSpec(kClassPriest, /*spec_tab=*/1, /*has_talents=*/true,
                             /*is_tank=*/false, /*is_healer=*/true);
  EXPECT_EQ(EvaluateStatFit(priest, kClassPriest, caster_stats),
            StatFit::kMatch);
  EXPECT_EQ(EvaluateStatFit(priest, kClassPriest, agi_dps_stats),
            StatFit::kMismatch);
  EXPECT_EQ(
      EvaluateStatFit(LootStatProfileForSpec(kClassMage, 2, true, false, false),
                      kClassMage, str_dps_stats),
      StatFit::kMismatch);

  // Feral Druid (DPS and Tank): accepts both Agi and Str, rejects caster.
  const LootStatProfile feral_dps =
      LootStatProfileForSpec(kClassDruid, /*spec_tab=*/1, /*has_talents=*/true,
                             /*is_tank=*/false, /*is_healer=*/false);
  EXPECT_EQ(
      EvaluateStatFit(feral_dps, kClassDruid, ItemStatFlags{.strength = true}),
      StatFit::kMatch);
  EXPECT_EQ(EvaluateStatFit(feral_dps, kClassDruid, caster_stats),
            StatFit::kMismatch);
  const LootStatProfile feral_tank =
      LootStatProfileForSpec(kClassDruid, /*spec_tab=*/1, /*has_talents=*/true,
                             /*is_tank=*/true, /*is_healer=*/false);
  EXPECT_EQ(feral_tank, LootStatProfile::kTankAgility);
  EXPECT_EQ(EvaluateStatFit(feral_tank, kClassDruid, tank_stats),
            StatFit::kMatch);

  // Blood/Frost DK: tank vs melee strength.
  EXPECT_EQ(LootStatProfileForSpec(kClassDeathKnight, 0, true, true, false),
            LootStatProfile::kTankStrength);
  EXPECT_EQ(LootStatProfileForSpec(kClassDeathKnight, 1, true, false, false),
            LootStatProfile::kMeleeStrength);

  // Enhancement vs Resto Shaman.
  EXPECT_EQ(LootStatProfileForSpec(kClassShaman, 1, true, false, false),
            LootStatProfile::kPhysicalAgility);
  EXPECT_EQ(LootStatProfileForSpec(kClassShaman, 2, true, false, true),
            LootStatProfile::kSpellCaster);
}

TEST(LootRollPolicyTest, ChoiceVerbsAndPendingRollTracking) {
  EXPECT_STREQ(LootRollChoiceVerb(LootRollChoice::kNeed), "Need");
  EXPECT_STREQ(LootRollChoiceVerb(LootRollChoice::kGreed), "Greed");
  EXPECT_STREQ(LootRollChoiceVerb(LootRollChoice::kDisenchant), "Disenchant");
  EXPECT_STREQ(LootRollChoiceVerb(LootRollChoice::kPass), "Pass");

  std::vector<PendingRollSeen> seen;
  const RollKey r1{0x100, 0};
  const RollKey r2{0x100, 1};

  EXPECT_EQ(RecordAndGetRollElapsedMs(&seen, r1, 1000), 0u);
  EXPECT_EQ(RecordAndGetRollElapsedMs(&seen, r1, 3500), 2500u);
  EXPECT_EQ(RecordAndGetRollElapsedMs(&seen, r2, 3500), 0u);
  // Clock skew safety: now < first_seen returns 0 instead of underflowing.
  EXPECT_EQ(RecordAndGetRollElapsedMs(&seen, r1, 500), 0u);

  // Prune removes closed rolls while keeping active ones intact.
  PrunePendingRolls(&seen, {r2});
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].item_guid, r2.item_guid);
  EXPECT_EQ(seen[0].item_slot, r2.item_slot);
  EXPECT_EQ(RecordAndGetRollElapsedMs(&seen, r2, 5000), 1500u);

  PrunePendingRolls(&seen, {});
  EXPECT_TRUE(seen.empty());
}

}  // namespace
}  // namespace llm_brain
