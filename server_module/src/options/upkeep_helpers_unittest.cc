// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/upkeep_helpers.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

bool HasReagent(const std::vector<ClassReagentSpec>& specs, uint32_t item_id) {
  return std::any_of(
      specs.begin(), specs.end(),
      [&](const ClassReagentSpec& s) { return s.item_id == item_id; });
}

TEST(UpkeepHelpersTest, StackBuyCount20WorksAndCapsAtAffordableAndBagSpace) {
  // Single-item vendor unit (buy_count=1), target=20, plenty of gold & space.
  {
    const StackBuyPlan plan = ComputeStackBuyPlan(StackBuyRequest{
        .current_count = 0,
        .target_stock = 20,
        .buy_count = 1,
        .max_stack = 20,
        .unit_price_copper = 50,
        .bot_money_copper = 5000,
    });
    EXPECT_EQ(plan.purchase_units, 20u);
    EXPECT_EQ(plan.total_items, 20u);
    EXPECT_EQ(plan.total_copper, 1000u);
    EXPECT_EQ(FormatStackBuyLabel("Maple Seed", "Reagent Vendor", plan),
              "Buy Maple Seed x20 (10s) from Reagent Vendor");
  }

  // Vendor stacks of 5 (e.g. food/drink): 4 purchase units = 20 items.
  {
    const StackBuyPlan plan = ComputeStackBuyPlan(StackBuyRequest{
        .current_count = 2,
        .target_stock = 20,
        .buy_count = 5,
        .max_stack = 20,
        .unit_price_copper = 25,
        .bot_money_copper = 1000,
    });
    EXPECT_EQ(plan.purchase_units, 4u);
    EXPECT_EQ(plan.total_items, 20u);
    EXPECT_EQ(plan.total_copper, 100u);
    EXPECT_EQ(FormatStackBuyLabel("Tough Jerky", "Innkeeper", plan),
              "Buy Tough Jerky x20 (1s) from Innkeeper");
  }

  // Caps at affordable money.
  {
    const StackBuyPlan plan = ComputeStackBuyPlan(StackBuyRequest{
        .current_count = 0,
        .target_stock = 20,
        .buy_count = 1,
        .max_stack = 20,
        .unit_price_copper = 100,
        .bot_money_copper = 750,
    });
    EXPECT_EQ(plan.purchase_units, 7u);
    EXPECT_EQ(plan.total_items, 7u);
    EXPECT_EQ(plan.total_copper, 700u);
  }

  // Cannot afford even 1 unit -> empty plan.
  {
    const StackBuyPlan plan = ComputeStackBuyPlan(StackBuyRequest{
        .current_count = 0,
        .target_stock = 20,
        .buy_count = 1,
        .max_stack = 20,
        .unit_price_copper = 100,
        .bot_money_copper = 99,
    });
    EXPECT_EQ(plan.purchase_units, 0u);
    EXPECT_EQ(plan.total_items, 0u);
  }

  // Caps at available bag space.
  {
    const StackBuyPlan plan = ComputeStackBuyPlan(StackBuyRequest{
        .current_count = 0,
        .target_stock = 20,
        .buy_count = 1,
        .max_stack = 20,
        .unit_price_copper = 10,
        .bot_money_copper = 5000,
        .max_storable_items = 6,
    });
    EXPECT_EQ(plan.purchase_units, 6u);
    EXPECT_EQ(plan.total_items, 6u);
  }

  // Caps at limited vendor stock.
  {
    const StackBuyPlan plan = ComputeStackBuyPlan(StackBuyRequest{
        .current_count = 0,
        .target_stock = 20,
        .buy_count = 5,
        .max_stack = 20,
        .unit_price_copper = 10,
        .bot_money_copper = 5000,
        .vendor_stock = 12,
    });
    EXPECT_EQ(plan.purchase_units, 2u);
    EXPECT_EQ(plan.total_items, 10u);
  }

  // Hunter ammo stack (buy_count=200, max_stack=1000).
  {
    const StackBuyPlan plan = ComputeStackBuyPlan(StackBuyRequest{
        .current_count = 150,
        .target_stock = kAmmoTargetStock,
        .buy_count = 200,
        .max_stack = 1000,
        .unit_price_copper = 10,
        .bot_money_copper = 5000,
    });
    EXPECT_EQ(plan.purchase_units, 5u);
    EXPECT_EQ(plan.total_items, 1000u);
    EXPECT_EQ(plan.total_copper, 50u);
  }

  // Adversarial counter-test: already at or above target stock -> 0.
  {
    const StackBuyPlan plan = ComputeStackBuyPlan(StackBuyRequest{
        .current_count = 20,
        .target_stock = 20,
        .buy_count = 5,
        .max_stack = 20,
        .unit_price_copper = 10,
        .bot_money_copper = 5000,
    });
    EXPECT_EQ(plan.purchase_units, 0u);
  }

  // Single-item free label omits x1 and copper cost.
  EXPECT_EQ(FormatStackBuyLabel(
                "Ankh", "Vendor",
                StackBuyPlan{
                    .purchase_units = 1, .total_items = 1, .total_copper = 0}),
            "Buy Ankh from Vendor");
}

TEST(UpkeepHelpersTest, FoodAndDrinkOfferedAtLowCountNotJustZero) {
  // Food offered at 0, 2, 9; not offered at 10+.
  EXPECT_TRUE(NeedsFoodRestock(0));
  EXPECT_TRUE(NeedsFoodRestock(2));
  EXPECT_TRUE(NeedsFoodRestock(kFoodRestockThreshold - 1));
  EXPECT_FALSE(NeedsFoodRestock(kFoodRestockThreshold));
  EXPECT_FALSE(NeedsFoodRestock(20));

  // Drink offered at low count for mana users, never for non-mana classes.
  EXPECT_TRUE(NeedsDrinkRestock(0, /*uses_mana=*/true));
  EXPECT_TRUE(NeedsDrinkRestock(2, /*uses_mana=*/true));
  EXPECT_TRUE(
      NeedsDrinkRestock(kDrinkRestockThreshold - 1, /*uses_mana=*/true));
  EXPECT_FALSE(NeedsDrinkRestock(kDrinkRestockThreshold, /*uses_mana=*/true));
  EXPECT_FALSE(NeedsDrinkRestock(0, /*uses_mana=*/false));
  EXPECT_FALSE(NeedsDrinkRestock(2, /*uses_mana=*/false));
}

TEST(UpkeepHelpersTest, ClassReagentsAndRoguePoisonsMatchWotlk335a) {
  // Druid Rebirth seeds & Gift of the Wild reagents scale by level.
  EXPECT_TRUE(ClassReagentSpecsFor(kClassDruid, 10).empty());
  EXPECT_TRUE(
      HasReagent(ClassReagentSpecsFor(kClassDruid, 25), 17034));  // Maple Seed
  EXPECT_TRUE(HasReagent(ClassReagentSpecsFor(kClassDruid, 80),
                         44614));  // Starleaf Seed
  EXPECT_TRUE(HasReagent(ClassReagentSpecsFor(kClassDruid, 80),
                         44605));  // Wild Spineleaf

  // Shaman Ankh at level 30+.
  EXPECT_TRUE(ClassReagentSpecsFor(kClassShaman, 20).empty());
  EXPECT_TRUE(HasReagent(ClassReagentSpecsFor(kClassShaman, 30), 17030));

  // Mage runes & Arcane Powder.
  const auto mage80 = ClassReagentSpecsFor(kClassMage, 80);
  EXPECT_TRUE(HasReagent(mage80, 17031));  // Rune of Teleportation
  EXPECT_TRUE(HasReagent(mage80, 17032));  // Rune of Portals
  EXPECT_TRUE(HasReagent(mage80, 17020));  // Arcane Powder

  // Paladin Symbol of Divinity & Symbol of Kings.
  const auto pal80 = ClassReagentSpecsFor(kClassPaladin, 80);
  EXPECT_TRUE(HasReagent(pal80, 17033));
  EXPECT_TRUE(HasReagent(pal80, 21177));

  // Priest candles & DK Corpse Dust.
  EXPECT_TRUE(HasReagent(ClassReagentSpecsFor(kClassPriest, 60),
                         17029));  // Sacred Candle
  EXPECT_TRUE(HasReagent(ClassReagentSpecsFor(kClassPriest, 80),
                         44615));  // Devout Candle
  EXPECT_TRUE(HasReagent(ClassReagentSpecsFor(kClassDeathKnight, 80), 37201));

  // Warriors/Hunters/Rogues have no reagent vendor specs here.
  EXPECT_TRUE(ClassReagentSpecsFor(kClassWarrior, 80).empty());
  EXPECT_TRUE(ClassReagentSpecsFor(kClassHunter, 80).empty());
  EXPECT_TRUE(ClassReagentSpecsFor(kClassRogue, 80).empty());

  // Rogue poison family classification.
  EXPECT_EQ(ClassifyRoguePoison("Instant Poison IX"),
            RoguePoisonFamily::kInstant);
  EXPECT_EQ(ClassifyRoguePoison("Deadly Poison IX"),
            RoguePoisonFamily::kDeadly);
  EXPECT_EQ(ClassifyRoguePoison("Wound Poison VII"), RoguePoisonFamily::kWound);
  EXPECT_EQ(ClassifyRoguePoison("Crippling Poison"),
            RoguePoisonFamily::kCrippling);
  EXPECT_EQ(ClassifyRoguePoison("Mind-Numbing Poison"),
            RoguePoisonFamily::kMindNumbing);
  EXPECT_EQ(ClassifyRoguePoison("Anesthetic Poison II"),
            RoguePoisonFamily::kAnesthetic);
  EXPECT_EQ(ClassifyRoguePoison("Refreshing Spring Water"),
            RoguePoisonFamily::kNone);
}

TEST(UpkeepHelpersTest, HunterAmmoSelectionEquipsValidArrowsAndBullets) {
  const std::vector<HunterAmmoCandidate> bag_arrows = {
      {.item_id = 2512,
       .item_level = 5,
       .count = 200,
       .matches_ranged_weapon = true,
       .usable = true},
      {.item_id = 2515,
       .item_level = 15,
       .count = 400,
       .matches_ranged_weapon = true,
       .usable = true},
      {.item_id = 2516,
       .item_level = 25,
       .count = 200,
       .matches_ranged_weapon = false,  // Bullets while holding a bow.
       .usable = true},
      {.item_id = 52021,
       .item_level = 80,
       .count = 1000,
       .matches_ranged_weapon = true,
       .usable = false},  // Level requirement too high.
  };

  // No ammo loaded -> picks best matching usable arrows (2515).
  EXPECT_EQ(
      PickHunterAmmoToEquip(/*is_hunter=*/true, /*alive=*/true,
                            /*current_ammo_id=*/0,
                            /*current_ammo_count=*/0,
                            /*current_ammo_ilvl=*/0,
                            /*current_ammo_matches_ranged=*/false, bag_arrows),
      2515u);

  // Loaded ammo ran dry (count == 0) -> reloads from bags.
  EXPECT_EQ(PickHunterAmmoToEquip(/*is_hunter=*/true, /*alive=*/true,
                                  /*current_ammo_id=*/2515,
                                  /*current_ammo_count=*/0,
                                  /*current_ammo_ilvl=*/15,
                                  /*current_ammo_matches_ranged=*/true,
                                  {bag_arrows[0]}),
            2512u);

  // Switched weapon (current ammo no longer matches ranged weapon) -> reloads.
  EXPECT_EQ(
      PickHunterAmmoToEquip(/*is_hunter=*/true, /*alive=*/true,
                            /*current_ammo_id=*/2516,
                            /*current_ammo_count=*/200,
                            /*current_ammo_ilvl=*/25,
                            /*current_ammo_matches_ranged=*/false, bag_arrows),
      2515u);

  // Already has optimal matching ammo -> no-op (0).
  EXPECT_EQ(
      PickHunterAmmoToEquip(/*is_hunter=*/true, /*alive=*/true,
                            /*current_ammo_id=*/2515,
                            /*current_ammo_count=*/400,
                            /*current_ammo_ilvl=*/15,
                            /*current_ammo_matches_ranged=*/true, bag_arrows),
      0u);

  // Adversarial counter-tests: non-hunter or dead never calls SetAmmo.
  EXPECT_EQ(PickHunterAmmoToEquip(/*is_hunter=*/false, /*alive=*/true, 0, 0, 0,
                                  false, bag_arrows),
            0u);
  EXPECT_EQ(PickHunterAmmoToEquip(/*is_hunter=*/true, /*alive=*/false, 0, 0, 0,
                                  false, bag_arrows),
            0u);
}

TEST(UpkeepHelpersTest, LeaderErrandValidationEnforcesCombatAndDeadInvariants) {
  EXPECT_TRUE(ValidateLeaderErrandCommand(/*alive=*/true, /*in_combat=*/false,
                                          /*party_in_combat=*/false,
                                          /*in_battleground=*/false)
                  .empty());
  // Adversarial P0 combat priority & state counter-tests.
  EXPECT_FALSE(ValidateLeaderErrandCommand(/*alive=*/true, /*in_combat=*/true,
                                           /*party_in_combat=*/false,
                                           /*in_battleground=*/false)
                   .empty());
  EXPECT_FALSE(ValidateLeaderErrandCommand(/*alive=*/true, /*in_combat=*/false,
                                           /*party_in_combat=*/true,
                                           /*in_battleground=*/false)
                   .empty());
  EXPECT_FALSE(ValidateLeaderErrandCommand(/*alive=*/false, /*in_combat=*/false,
                                           /*party_in_combat=*/false,
                                           /*in_battleground=*/false)
                   .empty());
  EXPECT_FALSE(ValidateLeaderErrandCommand(/*alive=*/true, /*in_combat=*/false,
                                           /*party_in_combat=*/false,
                                           /*in_battleground=*/true)
                   .empty());
}

}  // namespace
}  // namespace llm_brain
