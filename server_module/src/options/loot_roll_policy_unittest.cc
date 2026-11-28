// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/loot_roll_policy.h"

#include <cstdint>
#include <optional>
#include <string>
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
  EXPECT_EQ(EvaluateStatFit(fury, kClassWarrior, agi_dps_stats),
            StatFit::kMismatch);  // Attack Power does NOT override Agility.
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

// Adversarial counter-test: 10 s human-first roll deferral applies identically
// in both brain-driven mode (`ShouldDeferRollForHuman`) and auto-popup mode
// (`DecidePolicyRollVote`), with zero delay when no human is pending.
TEST(LootRollPolicyTest, HumanRollDelayAppliesInBothBrainAndAutoPopupModes) {
  LootRollPolicyInput auto_in = ValidUpgradeInput(kDisenchantMask);
  auto_in.human_pending_vote = true;

  // Before 10 s: BOTH brain-driven and auto-popup defer.
  for (uint64_t t :
       {uint64_t{0}, uint64_t{1}, uint64_t{5000}, kHumanLootRollDeferMs - 1}) {
    auto_in.roll_elapsed_ms = t;
    EXPECT_TRUE(ShouldDeferRollForHuman(/*human_pending_vote=*/true, t)) << t;
    EXPECT_EQ(DecidePolicyRollVote(auto_in), std::nullopt) << t;
  }

  // At 10 s timeout (human AFK): BOTH release deferral so the roll resolves
  // well before the 15 s fallback and 60 s WoW roll timer.
  auto_in.roll_elapsed_ms = kHumanLootRollDeferMs;
  EXPECT_FALSE(ShouldDeferRollForHuman(/*human_pending_vote=*/true,
                                       kHumanLootRollDeferMs));
  EXPECT_EQ(DecidePolicyRollVote(auto_in), LootRollChoice::kNeed);

  // Human votes early (at t = 2 s): BOTH release deferral immediately.
  auto_in.roll_elapsed_ms = 2000;
  auto_in.human_pending_vote = false;
  EXPECT_FALSE(ShouldDeferRollForHuman(/*human_pending_vote=*/false, 2000));
  EXPECT_EQ(DecidePolicyRollVote(auto_in), LootRollChoice::kNeed);

  // All-bot / solo group (no human pending): immediate at t = 0 in BOTH modes.
  auto_in.roll_elapsed_ms = 0;
  EXPECT_FALSE(ShouldDeferRollForHuman(/*human_pending_vote=*/false, 0));
  EXPECT_EQ(DecidePolicyRollVote(auto_in), LootRollChoice::kNeed);
}

// Adversarial counter-test: Attack Power is NOT a universal fit for both Str
// and Agi classes, Ranged AP is Hunter-only, and a Warrior MUST NOT Need a
// Hunter's bow even when the bow is usable and has AP/Crit/Hit stats.
TEST(LootRollPolicyTest, WarriorDoesNotNeedHunterBowAndApIsNotUniversal) {
  const LootStatProfile fury =
      LootStatProfileForSpec(kClassWarrior, 1, true, false, false);
  const LootStatProfile hunter =
      LootStatProfileForSpec(kClassHunter, 0, true, false, false);
  const LootStatProfile rogue =
      LootStatProfileForSpec(kClassRogue, 1, true, false, false);

  // Str + AP item is a mismatch for Hunter and Rogue (AP doesn't override Str).
  const ItemStatFlags str_ap{.strength = true, .attack_power = true};
  EXPECT_EQ(EvaluateStatFit(fury, kClassWarrior, str_ap), StatFit::kMatch);
  EXPECT_EQ(EvaluateStatFit(hunter, kClassHunter, str_ap), StatFit::kMismatch);
  EXPECT_EQ(EvaluateStatFit(rogue, kClassRogue, str_ap), StatFit::kMismatch);

  // Agi + AP item is a mismatch for Warrior (AP doesn't override Agi).
  const ItemStatFlags agi_ap{.agility = true, .attack_power = true};
  EXPECT_EQ(EvaluateStatFit(fury, kClassWarrior, agi_ap), StatFit::kMismatch);
  EXPECT_EQ(EvaluateStatFit(hunter, kClassHunter, agi_ap), StatFit::kMatch);

  // Ranged AP is Hunter-only.
  const ItemStatFlags ranged_ap{.ranged_attack_power = true, .crit = true};
  EXPECT_EQ(EvaluateStatFit(hunter, kClassHunter, ranged_ap), StatFit::kMatch);
  EXPECT_EQ(EvaluateStatFit(fury, kClassWarrior, ranged_ap),
            StatFit::kMismatch);
  EXPECT_EQ(EvaluateStatFit(rogue, kClassRogue, ranged_ap), StatFit::kMismatch);

  // Weapon suitability: Bow/Gun/Crossbow are Hunter primary weapons.
  EXPECT_TRUE(
      IsWeaponDisallowedForSpec(fury, kClassWarrior, kWeaponSubclassBow));
  EXPECT_TRUE(
      IsWeaponDisallowedForSpec(fury, kClassWarrior, kWeaponSubclassGun));
  EXPECT_TRUE(
      IsWeaponDisallowedForSpec(fury, kClassWarrior, kWeaponSubclassCrossbow));
  EXPECT_TRUE(
      IsWeaponDisallowedForSpec(rogue, kClassRogue, kWeaponSubclassBow));
  EXPECT_FALSE(
      IsWeaponDisallowedForSpec(hunter, kClassHunter, kWeaponSubclassBow));
  EXPECT_FALSE(
      IsWeaponDisallowedForSpec(hunter, kClassHunter, kWeaponSubclassGun));

  // Even a neutral AP+Crit+Hit bow (no Agi) is NOT Needed by a Warrior.
  LootRollPolicyInput warrior_bow = ValidUpgradeInput(kStandardMask);
  warrior_bow.stat_fit = EvaluateStatFit(
      fury, kClassWarrior,
      ItemStatFlags{.attack_power = true, .hit = true, .crit = true});
  warrior_bow.weapon_disallowed_for_spec =
      IsWeaponDisallowedForSpec(fury, kClassWarrior, kWeaponSubclassBow);
  EXPECT_EQ(DecidePolicyRollVote(warrior_bow), LootRollChoice::kGreed);

  // The same bow IS Needed by a Hunter.
  LootRollPolicyInput hunter_bow = ValidUpgradeInput(kStandardMask);
  hunter_bow.stat_fit = EvaluateStatFit(
      hunter, kClassHunter,
      ItemStatFlags{.attack_power = true, .hit = true, .crit = true});
  hunter_bow.weapon_disallowed_for_spec =
      IsWeaponDisallowedForSpec(hunter, kClassHunter, kWeaponSubclassBow);
  EXPECT_EQ(DecidePolicyRollVote(hunter_bow), LootRollChoice::kNeed);

  // Wand is caster-only; Thrown is non-Hunter physical.
  const LootStatProfile mage =
      LootStatProfileForSpec(kClassMage, 2, true, false, false);
  EXPECT_FALSE(
      IsWeaponDisallowedForSpec(mage, kClassMage, kWeaponSubclassWand));
  EXPECT_TRUE(
      IsWeaponDisallowedForSpec(fury, kClassWarrior, kWeaponSubclassWand));
  EXPECT_TRUE(
      IsWeaponDisallowedForSpec(hunter, kClassHunter, kWeaponSubclassThrown));
  EXPECT_FALSE(
      IsWeaponDisallowedForSpec(fury, kClassWarrior, kWeaponSubclassThrown));
}

// Adversarial counter-test: gear & quest reward scoring accounts for armor
// proficiency and primary/secondary stats rather than item level alone.
TEST(LootRollPolicyTest, GearScoringPrefersClassStatsAndArmorOverRawIlvl) {
  // 1. Caster (Mage & Holy Paladin) prefers Spell Power + Intellect over
  //    higher-ilvl melee Plate (+Str/AP/Crit).
  GearItemInfo melee_plate;
  melee_plate.item_level = 200;
  melee_plate.is_armor = true;
  melee_plate.armor_subclass = kArmorSubclassPlate;
  melee_plate.armor = 1400;
  melee_plate.strength = 80;
  melee_plate.stamina = 60;
  melee_plate.crit = 40;

  GearItemInfo caster_cloth;
  caster_cloth.item_level = 150;
  caster_cloth.is_armor = true;
  caster_cloth.armor_subclass = kArmorSubclassCloth;
  caster_cloth.armor = 220;
  caster_cloth.intellect = 45;
  caster_cloth.spell_power = 65;
  caster_cloth.haste = 30;
  caster_cloth.mp5 = 15;

  const LootStatProfile mage =
      LootStatProfileForSpec(kClassMage, 2, true, false, false);
  EXPECT_GT(ScoreGearItem(mage, kClassMage, kArmorSubclassCloth, caster_cloth),
            ScoreGearItem(mage, kClassMage, kArmorSubclassCloth, melee_plate));

  const LootStatProfile holy_pal =
      LootStatProfileForSpec(kClassPaladin, 0, true, false, true);
  EXPECT_GT(
      ScoreGearItem(holy_pal, kClassPaladin, kArmorSubclassPlate, caster_cloth),
      ScoreGearItem(holy_pal, kClassPaladin, kArmorSubclassPlate, melee_plate));

  // 2. Plate Warrior prefers Plate over higher-ilvl Leather/Cloth.
  GearItemInfo lower_plate;
  lower_plate.item_level = 180;
  lower_plate.is_armor = true;
  lower_plate.armor_subclass = kArmorSubclassPlate;
  lower_plate.armor = 1200;
  lower_plate.strength = 60;
  lower_plate.crit = 35;
  lower_plate.hit = 30;

  GearItemInfo higher_leather = lower_plate;
  higher_leather.item_level = 200;
  higher_leather.armor_subclass = kArmorSubclassLeather;
  higher_leather.armor = 400;

  const LootStatProfile fury =
      LootStatProfileForSpec(kClassWarrior, 1, true, false, false);
  EXPECT_GT(
      ScoreGearItem(fury, kClassWarrior, kArmorSubclassPlate, lower_plate),
      ScoreGearItem(fury, kClassWarrior, kArmorSubclassPlate, higher_leather));

  // 3. Secondary stats (hit, crit, expertise) beat higher ilvl with empty
  //    secondary budget.
  GearItemInfo high_ilvl_sta_only;
  high_ilvl_sta_only.item_level = 200;
  high_ilvl_sta_only.is_armor = true;
  high_ilvl_sta_only.armor_subclass = kArmorSubclassPlate;
  high_ilvl_sta_only.armor = 1300;
  high_ilvl_sta_only.strength = 35;
  high_ilvl_sta_only.stamina = 80;

  GearItemInfo lower_ilvl_with_secondaries;
  lower_ilvl_with_secondaries.item_level = 180;
  lower_ilvl_with_secondaries.is_armor = true;
  lower_ilvl_with_secondaries.armor_subclass = kArmorSubclassPlate;
  lower_ilvl_with_secondaries.armor = 1200;
  lower_ilvl_with_secondaries.strength = 45;
  lower_ilvl_with_secondaries.hit = 35;
  lower_ilvl_with_secondaries.crit = 35;
  lower_ilvl_with_secondaries.expertise = 25;

  EXPECT_GT(ScoreGearItem(fury, kClassWarrior, kArmorSubclassPlate,
                          lower_ilvl_with_secondaries),
            ScoreGearItem(fury, kClassWarrior, kArmorSubclassPlate,
                          high_ilvl_sta_only));

  // 4. Tank values defense/dodge/parry over pure DPS crit/haste at same ilvl.
  const LootStatProfile prot =
      LootStatProfileForSpec(kClassWarrior, 2, true, true, false);
  GearItemInfo tank_plate;
  tank_plate.item_level = 200;
  tank_plate.is_armor = true;
  tank_plate.armor_subclass = kArmorSubclassPlate;
  tank_plate.armor = 1600;
  tank_plate.stamina = 70;
  tank_plate.defense = 45;
  tank_plate.dodge = 35;
  EXPECT_GT(
      ScoreGearItem(prot, kClassWarrior, kArmorSubclassPlate, tank_plate),
      ScoreGearItem(prot, kClassWarrior, kArmorSubclassPlate, melee_plate));
}

// Trade window exposes >8 distinct tradeable item kinds, deduplicates stacks,
// skips non-tradeable/already-offered items, and prioritizes non-grey items
// ahead of grey junk.
TEST(LootRollPolicyTest, TradeWindowExposesMoreThanEightTradeableItemKinds) {
  std::vector<TradeBagItem> bags;
  // 10 distinct grey junk items in bag 0.
  for (uint8_t i = 0; i < 10; ++i) {
    bags.push_back(TradeBagItem{
        .bag = 255,
        .slot = static_cast<uint8_t>(23 + i),
        .item_id = static_cast<uint32_t>(1000 + i),
        .name = "Grey Scrap " + std::to_string(i),
        .count = 1,
        .quality = 0,
        .can_be_traded = true,
    });
  }
  // 4 useful non-grey items in bag 1 (plus a duplicate stack, an
  // already-in-trade item, a soulbound item, and a bound-not-with-trader item).
  bags.push_back(TradeBagItem{.bag = 19,
                              .slot = 0,
                              .item_id = 8079,
                              .name = "Conjured Crystal Water",
                              .count = 20,
                              .quality = 1});
  bags.push_back(TradeBagItem{.bag = 19,
                              .slot = 1,
                              .item_id = 8079,  // Duplicate stack -> skipped.
                              .name = "Conjured Crystal Water",
                              .count = 12,
                              .quality = 1});
  bags.push_back(TradeBagItem{.bag = 19,
                              .slot = 2,
                              .item_id = 5512,
                              .name = "Healthstone",
                              .count = 1,
                              .quality = 1});
  bags.push_back(TradeBagItem{.bag = 19,
                              .slot = 3,
                              .item_id = 2589,
                              .name = "Linen Cloth",
                              .count = 15,
                              .quality = 1});
  bags.push_back(TradeBagItem{.bag = 19,
                              .slot = 4,
                              .item_id = 12345,
                              .name = "BoE Green Sword",
                              .count = 1,
                              .quality = 2});
  bags.push_back(TradeBagItem{.bag = 19,
                              .slot = 5,
                              .item_id = 9991,
                              .name = "Already Offered",
                              .count = 1,
                              .quality = 1,
                              .already_in_trade = true});
  bags.push_back(TradeBagItem{.bag = 19,
                              .slot = 6,
                              .item_id = 9992,
                              .name = "Soulbound Gear",
                              .count = 1,
                              .quality = 3,
                              .can_be_traded = false});
  bags.push_back(TradeBagItem{.bag = 19,
                              .slot = 7,
                              .item_id = 9993,
                              .name = "Bound To Other",
                              .count = 1,
                              .quality = 3,
                              .bound_not_with_trader = true});

  const std::vector<TradeOfferOption> offers =
      SelectTradeItemOffers(/*has_free_trade_slot=*/true, bags);
  // All 14 distinct tradeable kinds (10 grey + 4 non-grey) are exposed (>8!).
  ASSERT_EQ(offers.size(), 14u);
  // Non-grey items are prioritized ahead of grey junk in bag order.
  EXPECT_EQ(offers[0].guid, "trade:put:19:0:8079");
  EXPECT_EQ(offers[0].label, "Put Conjured Crystal Water x20 in the trade");
  EXPECT_EQ(offers[1].guid, "trade:put:19:2:5512");
  EXPECT_EQ(offers[1].label, "Put Healthstone in the trade");
  EXPECT_EQ(offers[2].guid, "trade:put:19:3:2589");
  EXPECT_EQ(offers[2].label, "Put Linen Cloth x15 in the trade");
  EXPECT_EQ(offers[3].guid, "trade:put:19:4:12345");
  EXPECT_EQ(offers[3].label, "Put BoE Green Sword in the trade");
  EXPECT_EQ(offers[4].guid, "trade:put:255:23:1000");
  EXPECT_EQ(offers[13].guid, "trade:put:255:32:1009");

  // Counter-test: when all 6 trade slots are full, no Put options are offered.
  EXPECT_TRUE(
      SelectTradeItemOffers(/*has_free_trade_slot=*/false, bags).empty());
}

}  // namespace
}  // namespace llm_brain
