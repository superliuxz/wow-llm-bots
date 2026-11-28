// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/npc_gossip_and_bags.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(NpcGossipTest, SanitizesColorCodesNewlinesQuotesAndTruncates) {
  EXPECT_EQ(SanitizeGossipText("|cFF00FF00Tell me more.|r"), "Tell me more.");
  EXPECT_EQ(SanitizeGossipText("|TInterface\\Icons\\INV_Misc:16|t \"Ready!\"\n"
                               "  1. Fake option"),
            "'Ready!' 1. Fake option");
  EXPECT_EQ(SanitizeGossipText("|Hitem:1234|h[Magic Stone]|h"),
            "[Magic Stone]");
  EXPECT_EQ(SanitizeGossipText("   \r\n\t  "), "");
  const std::string long_msg(100, 'a');
  const std::string truncated = SanitizeGossipText(long_msg, 20);
  EXPECT_EQ(truncated.size(), 20u);
  EXPECT_TRUE(truncated.ends_with("..."));
}

TEST(NpcGossipTest, SelectsDbAndScriptedDialogueOptionsAndFiltersServices) {
  std::vector<GossipOptionCandidate> candidates = {
      // DB service options (sender == 0, non-gossip option_type): filtered out.
      {.list_id = 0,
       .sender = 0,
       .option_type = 3 /*VENDOR*/,
       .message = "Browse goods"},
      {.list_id = 1,
       .sender = 0,
       .option_type = 5 /*TRAINER*/,
       .message = "Train me"},
      {.list_id = 2,
       .sender = 0,
       .option_type = 8 /*INNKEEPER*/,
       .message = "Make this inn your home"},
      // DB dialogue option: included.
      {.list_id = 5,
       .sender = 0,
       .option_type = kGossipOptionGossip,
       .message = "We are ready to begin."},
      // Scripted gossip option (sender != 0, arbitrary action id): included.
      {.list_id = 3,
       .sender = 1,
       .option_type = 1001,
       .message = "Teleport me to the upper spire.",
       .box_money = 500},
      // Coded option: filtered out.
      {.list_id = 4,
       .sender = 1,
       .option_type = 1002,
       .message = "Enter secret passphrase",
       .is_coded = true},
      // Unaffordable option: filtered out.
      {.list_id = 6,
       .sender = 1,
       .option_type = 1003,
       .message = "Bribe the guard",
       .box_money = 50000},
      // Empty after sanitization: filtered out.
      {.list_id = 7,
       .sender = 1,
       .option_type = 1004,
       .message = "|cFFFFFFFF   |r"},
  };

  const std::vector<GossipOptionChoice> choices =
      SelectGossipOptions(/*menu_id=*/42, candidates, /*bot_money_copper=*/1000,
                          "Brann Bronzebeard");
  ASSERT_EQ(choices.size(), 2u);
  // Sorted by list_id ascending.
  EXPECT_EQ(choices[0].menu_id, 42u);
  EXPECT_EQ(choices[0].list_id, 3u);
  EXPECT_EQ(choices[0].label,
            "Say to Brann Bronzebeard: \"Teleport me to the upper spire.\" "
            "(5s)");
  EXPECT_EQ(choices[1].menu_id, 42u);
  EXPECT_EQ(choices[1].list_id, 5u);
  EXPECT_EQ(choices[1].label,
            "Say to Brann Bronzebeard: \"We are ready to begin.\"");
}

TEST(NpcGossipTest, ValidateGossipSelectEnforcesCombatRangeStaleMenuAndMoney) {
  EXPECT_TRUE(ValidateGossipSelect(/*in_combat=*/false, /*npc_in_range=*/true,
                                   /*menu_matches_npc=*/true,
                                   /*menu_id_matches=*/true,
                                   /*item_exists=*/true, /*is_coded=*/false,
                                   /*box_money=*/100, /*bot_money=*/100)
                  .empty());
  // Adversarial counter-tests: P0 combat, out of range, stale/closed menu,
  // coded option, insufficient money.
  EXPECT_EQ(ValidateGossipSelect(true, true, true, true, true, false, 0, 100),
            "you are in combat");
  EXPECT_EQ(ValidateGossipSelect(false, false, true, true, true, false, 0, 100),
            "that NPC is not in talk range");
  EXPECT_EQ(ValidateGossipSelect(false, true, false, true, true, false, 0, 100),
            "that dialogue option is no longer open");
  EXPECT_EQ(ValidateGossipSelect(false, true, true, false, true, false, 0, 100),
            "that dialogue option is no longer open");
  EXPECT_EQ(ValidateGossipSelect(false, true, true, true, false, false, 0, 100),
            "that dialogue option is no longer open");
  EXPECT_EQ(ValidateGossipSelect(false, true, true, true, true, true, 0, 100),
            "that dialogue option requires a text code");
  EXPECT_EQ(
      ValidateGossipSelect(false, true, true, true, true, false, 500, 100),
      "not enough money");
}

DestroyItemCandidate MakeGreyJunk(uint8_t bag,
                                  uint8_t slot,
                                  uint32_t entry,
                                  const char* name,
                                  uint32_t sell_price,
                                  uint32_t stack = 1) {
  return DestroyItemCandidate{
      .bag = bag,
      .slot = slot,
      .entry = entry,
      .name = name,
      .quality = kItemQualityPoor,
      .item_class = 15,  // Misc
      .sell_price = sell_price,
      .stack_count = stack,
      .in_general_bag = true,
      .is_equipped_or_bag_slot = false,
      .required_for_active_quest = false,
  };
}

TEST(FullBagsDestroyTest, SafetyInvariantsProtectQuestEquippedValuableItems) {
  const DestroyItemCandidate safe =
      MakeGreyJunk(255, 23, 4865, "Ruined Pelt", 5, 3);
  EXPECT_TRUE(DestroySafetyBlockReason(safe).empty());

  // Equipped / bag slot.
  {
    auto c = safe;
    c.is_equipped_or_bag_slot = true;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty());
  }
  // Special bag (quiver / herb bag): doesn't free a general bag slot.
  {
    auto c = safe;
    c.in_general_bag = false;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty());
  }
  // Hearthstone.
  {
    auto c = safe;
    c.entry = kHearthstoneEntry;
    c.name = "Hearthstone";
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty());
  }
  // ITEM_FLAG_NO_USER_DESTROY.
  {
    auto c = safe;
    c.flags = kItemFlagNoUserDestroy;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty());
  }
  // Quest class, quest bonding, start_quest, and active-quest-required item.
  {
    auto c = safe;
    c.item_class = kItemClassQuest;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty());
  }
  {
    auto c = safe;
    c.bonding = kItemBondQuest;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty());
  }
  {
    auto c = safe;
    c.start_quest = 1234;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty());
  }
  {
    auto c = safe;
    c.required_for_active_quest = true;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty());
  }
  // Protected item classes (container, quiver, key, projectile, reagent).
  for (uint32_t cls : {kItemClassContainer, kItemClassQuiver, kItemClassKey,
                       kItemClassProjectile, kItemClassReagent}) {
    auto c = safe;
    c.item_class = cls;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty()) << cls;
  }
  // Uncommon (green) and rare (blue) items are allowed for brain-chosen
  // destruction; epic and higher quality items remain protected.
  for (uint32_t q : {kItemQualityUncommon, kItemQualityRare}) {
    auto c = safe;
    c.quality = q;
    EXPECT_TRUE(DestroySafetyBlockReason(c).empty()) << q;
  }
  for (uint32_t q : {kItemQualityEpic, 5u, 6u, 7u}) {
    auto c = safe;
    c.quality = q;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty()) << q;
  }
}

TEST(FullBagsDestroyTest,
     OnlyOfferedWhenBagsFullOrNearlyFullAliveOutOfCombatNotTrading) {
  const std::vector<DestroyItemCandidate> items = {
      MakeGreyJunk(255, 23, 4865, "Ruined Pelt", 5, 1),
  };
  EXPECT_TRUE(
      SelectDestroyItemOptions(/*free_bag_slots=*/2, true, false, false, items)
          .empty());
  EXPECT_EQ(
      SelectDestroyItemOptions(/*free_bag_slots=*/1, true, false, false, items)
          .size(),
      1u);
  EXPECT_TRUE(SelectDestroyItemOptions(0, /*alive=*/false, false, false, items)
                  .empty());
  EXPECT_TRUE(
      SelectDestroyItemOptions(0, true, /*in_combat=*/true, false, items)
          .empty());
  EXPECT_TRUE(SelectDestroyItemOptions(0, true, false, /*in_trade=*/true, items)
                  .empty());
  EXPECT_EQ(SelectDestroyItemOptions(0, true, false, false, items).size(), 1u);
}

TEST(FullBagsDestroyTest, RanksByQualityThenCheapestAndDeduplicates) {
  auto white_food = MakeGreyJunk(255, 24, 117, "Tough Jerky", 1, 2);
  white_food.quality = kItemQualityNormal;
  auto quest_item = MakeGreyJunk(255, 25, 9999, "Boar Snout", 0, 1);
  quest_item.required_for_active_quest = true;
  auto green_sword = MakeGreyJunk(255, 26, 2000, "Green Blade", 10, 1);
  green_sword.quality = kItemQualityUncommon;
  auto epic_sword = MakeGreyJunk(255, 30, 2001, "Epic Blade", 50, 1);
  epic_sword.quality = kItemQualityEpic;

  const std::vector<DestroyItemCandidate> items = {
      epic_sword,
      green_sword,
      white_food,
      quest_item,
      MakeGreyJunk(255, 27, 4865, "Ruined Pelt", 5, 4),      // 20c
      MakeGreyJunk(255, 28, 4865, "Ruined Pelt", 5, 1),      // 5c (cheaper)
      MakeGreyJunk(255, 29, 7098, "Splintered Tusk", 0, 1),  // 0c
  };

  const std::vector<DestroyItemOption> opts =
      SelectDestroyItemOptions(0, true, false, false, items, /*max_options=*/4);
  ASSERT_EQ(opts.size(), 4u);
  EXPECT_EQ(opts[0].guid, "destroy:255:29:7098");
  EXPECT_EQ(opts[0].label, "Destroy Splintered Tusk (grey, no value)");
  // Deduplicates 4865, keeping the cheaper single-item stack in slot 28.
  EXPECT_EQ(opts[1].guid, "destroy:255:28:4865");
  EXPECT_EQ(opts[1].label, "Destroy Ruined Pelt (grey, 5c)");
  EXPECT_EQ(opts[2].guid, "destroy:255:24:117");
  EXPECT_EQ(opts[2].label, "Destroy Tough Jerky x2 (white, 2c)");
  EXPECT_EQ(opts[3].guid, "destroy:255:26:2000");
  EXPECT_EQ(opts[3].label, "Destroy Green Blade (green, 10c)");
}

VendorSellCandidate MakeGreenSword(uint8_t bag,
                                   uint8_t slot,
                                   uint32_t entry,
                                   const char* name,
                                   uint32_t sell_price,
                                   uint32_t ilvl = 18) {
  return VendorSellCandidate{
      .bag = bag,
      .slot = slot,
      .entry = entry,
      .name = name,
      .quality = kItemQualityUncommon,
      .item_class = kItemClassWeapon,
      .item_level = ilvl,
      .sell_price = sell_price,
      .stack_count = 1,
      .is_equipped_or_bag_slot = false,
      .is_non_empty_bag = false,
      .is_refundable = false,
      .required_for_active_quest = false,
  };
}

TEST(VendorSellTest, SafetyInvariantsProtectEquippedQuestHearthstoneReagents) {
  const VendorSellCandidate safe =
      MakeGreenSword(255, 23, 2000, "Green Blade", 1250, 18);
  EXPECT_TRUE(VendorSellSafetyBlockReason(safe).empty());

  // Equipped item / bag slot.
  {
    auto c = safe;
    c.is_equipped_or_bag_slot = true;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  // Non-empty bag.
  {
    auto c = safe;
    c.is_non_empty_bag = true;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  // Refundable item.
  {
    auto c = safe;
    c.is_refundable = true;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  // Zero sell price.
  {
    auto c = safe;
    c.sell_price = 0;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  // Hearthstone.
  {
    auto c = safe;
    c.entry = kHearthstoneEntry;
    c.name = "Hearthstone";
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  // Quest items (class, bonding, start_quest, active-quest-required).
  {
    auto c = safe;
    c.item_class = kItemClassQuest;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  {
    auto c = safe;
    c.bonding = kItemBondQuest;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  {
    auto c = safe;
    c.start_quest = 42;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  {
    auto c = safe;
    c.required_for_active_quest = true;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty());
  }
  // Protected classes: keys, reagents, projectiles, quivers.
  for (uint32_t cls : {kItemClassKey, kItemClassReagent, kItemClassProjectile,
                       kItemClassQuiver}) {
    auto c = safe;
    c.item_class = cls;
    EXPECT_FALSE(VendorSellSafetyBlockReason(c).empty()) << cls;
  }
  // Empty spare container in inventory IS sellable.
  {
    auto c = safe;
    c.item_class = kItemClassContainer;
    c.is_non_empty_bag = false;
    EXPECT_TRUE(VendorSellSafetyBlockReason(c).empty());
  }
}

TEST(VendorSellTest, SelectsNonGreyItemsRankedAndDeduplicated) {
  auto grey_junk = MakeGreenSword(255, 23, 4865, "Ruined Pelt", 5, 1);
  grey_junk.quality = kItemQualityPoor;
  auto white_meat = MakeGreenSword(255, 24, 117, "Tough Jerky", 15, 5);
  white_meat.quality = kItemQualityNormal;
  white_meat.item_class = 0;  // Consumable
  white_meat.stack_count = 3;
  auto green_dup_expensive = MakeGreenSword(255, 25, 2000, "Green Blade", 500);
  auto green_dup_cheap = MakeGreenSword(255, 26, 2000, "Green Blade", 500);
  auto blue_helm = MakeGreenSword(19, 0, 3000, "Cobalt Helm", 2500, 32);
  blue_helm.quality = kItemQualityRare;
  blue_helm.item_class = kItemClassArmor;
  auto quest_green = MakeGreenSword(19, 1, 3001, "Quest Blade", 100, 15);
  quest_green.required_for_active_quest = true;

  const std::vector<VendorSellOption> opts = SelectVendorSellOptions(
      "0x1234", "Brother Danil",
      {blue_helm, grey_junk, quest_green, green_dup_expensive, green_dup_cheap,
       white_meat});
  ASSERT_EQ(opts.size(), 3u);
  EXPECT_EQ(opts[0].action_guid, "npc_sell:0x1234:255:24:117");
  EXPECT_EQ(opts[0].label, "Sell Tough Jerky x3 to Brother Danil (white, 45c)");
  EXPECT_EQ(opts[1].action_guid, "npc_sell:0x1234:255:25:2000");
  EXPECT_EQ(opts[1].label,
            "Sell Green Blade to Brother Danil (green, ilvl 18, 5s)");
  EXPECT_EQ(opts[2].action_guid, "npc_sell:0x1234:19:0:3000");
  EXPECT_EQ(opts[2].label,
            "Sell Cobalt Helm to Brother Danil (blue, ilvl 32, 25s)");
}

TEST(BagEquipAndBuyTest, PickBagEquipSlotPrefersEmptyThenSmallestGeneralBag) {
  // 1. Prefers first empty bag slot over a smaller occupied bag.
  {
    const std::vector<EquippedBagSlotInfo> slots = {
        {.slot = 19,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 6,
         .used_slots = 4,
         .name = "Small Brown Pouch"},
        {.slot = 20, .occupied = false, .name = ""},
        {.slot = 21, .occupied = false, .name = ""},
        {.slot = 22, .occupied = false, .name = ""},
    };
    const auto target =
        PickBagEquipSlot(/*new_bag_slots=*/8, /*source_bag=*/255,
                         /*can_relocate=*/true, slots);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target->dest_slot, 20u);
    EXPECT_FALSE(target->replaces_existing);
    EXPECT_EQ(FormatBagEquipDetail(8, *target), "8-slot bag, empty bag slot");
  }

  // 2. All 4 slots occupied: replaces smallest general bag, skipping quiver.
  {
    const std::vector<EquippedBagSlotInfo> slots = {
        {.slot = 19,
         .occupied = true,
         .is_general_bag = false,
         .container_slots = 6,
         .used_slots = 6,
         .name = "Light Quiver"},
        {.slot = 20,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 10,
         .used_slots = 8,
         .name = "Linen Bag"},
        {.slot = 21,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 6,
         .used_slots = 6,
         .name = "Small Brown Pouch"},
        {.slot = 22,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 8,
         .used_slots = 5,
         .name = "Woolen Bag"},
    };
    const auto target = PickBagEquipSlot(/*new_bag_slots=*/12,
                                         /*source_bag=*/255,
                                         /*can_relocate=*/true, slots);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target->dest_slot, 21u);
    EXPECT_TRUE(target->replaces_existing);
    EXPECT_FALSE(target->needs_relocate);
    EXPECT_EQ(FormatBagEquipDetail(12, *target),
              "12-slot bag, replaces 6-slot Small Brown Pouch");
  }

  // 3. Adversarial: new bag sits INSIDE the only smaller bag (slot 21).
  // Without relocation space, refuse; with relocation space, flag relocate.
  {
    const std::vector<EquippedBagSlotInfo> slots = {
        {.slot = 19,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 12,
         .used_slots = 12,
         .name = "Silk Pack"},
        {.slot = 20,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 12,
         .used_slots = 12,
         .name = "Silk Pack"},
        {.slot = 21,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 6,
         .used_slots = 6,
         .name = "Small Brown Pouch"},
        {.slot = 22,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 12,
         .used_slots = 12,
         .name = "Silk Pack"},
    };
    EXPECT_FALSE(PickBagEquipSlot(10, /*source_bag=*/21,
                                  /*can_relocate=*/false, slots)
                     .has_value());
    const auto target = PickBagEquipSlot(10, /*source_bag=*/21,
                                         /*can_relocate=*/true, slots);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target->dest_slot, 21u);
    EXPECT_TRUE(target->needs_relocate);
  }

  // 4. Not an upgrade when all equipped general bags are >= new_bag_slots.
  {
    const std::vector<EquippedBagSlotInfo> slots = {
        {.slot = 19,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 12,
         .name = ""},
        {.slot = 20,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 14,
         .name = ""},
    };
    EXPECT_FALSE(PickBagEquipSlot(12, 255, true, slots).has_value());
  }
}

TEST(BagEquipAndBuyTest, SelectBagPurchasesHandlesEmptySmallerAndFullBags) {
  const std::vector<BagVendorCandidate> vendor_bags = {
      {.item_id = 4496,
       .name = "Small Brown Pouch",
       .container_slots = 6,
       .price_copper = 500},
      {.item_id = 4497,
       .name = "Heavy Brown Bag",
       .container_slots = 10,
       .price_copper = 20000},
      {.item_id = 4498,
       .name = "Brown Leather Satchel",
       .container_slots = 8,
       .price_copper = 2500},
      {.item_id = 4499,
       .name = "Expensive 8-Slot",
       .container_slots = 8,
       .price_copper = 5000},
      {.item_id = 4500,
       .name = "Rep-Locked 14-Slot",
       .container_slots = 14,
       .price_copper = 1000,
       .meets_requirements = false},
  };

  // Empty bag slot: even with 0 free inventory slots, can buy directly into
  // empty bag slot; deduplicates 8-slot keeping cheaper one, largest first.
  {
    const std::vector<EquippedBagSlotInfo> slots = {
        {.slot = 19,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 6,
         .name = ""},
        {.slot = 20, .occupied = false, .name = ""},
    };
    const auto picks =
        SelectBagPurchases(vendor_bags, /*bot_money=*/5000,
                           /*free_bag_slots=*/0, slots, "Bag Merchant");
    ASSERT_EQ(picks.size(), 2u);
    EXPECT_EQ(picks[0].item_id, 4498u);
    EXPECT_EQ(picks[0].label,
              "Buy Brown Leather Satchel (8-slot bag, 25s) from Bag Merchant");
    EXPECT_EQ(picks[1].item_id, 4496u);
    EXPECT_EQ(picks[1].label,
              "Buy Small Brown Pouch (6-slot bag, 5s) from Bag Merchant");
  }

  // All bag slots occupied with 6-slot bags:
  // - With 0 free inventory slots, cannot buy until a slot is freed.
  // - With >=1 free inventory slot, offers only strictly larger bags (>6).
  {
    const std::vector<EquippedBagSlotInfo> full_slots = {
        {.slot = 19,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 6,
         .name = ""},
        {.slot = 20,
         .occupied = true,
         .is_general_bag = true,
         .container_slots = 8,
         .name = ""},
    };
    EXPECT_TRUE(SelectBagPurchases(vendor_bags, 50000, /*free_bag_slots=*/0,
                                   full_slots, "Bag Merchant")
                    .empty());
    const auto picks = SelectBagPurchases(
        vendor_bags, 50000, /*free_bag_slots=*/1, full_slots, "Bag Merchant");
    ASSERT_EQ(picks.size(), 2u);
    EXPECT_EQ(picks[0].item_id, 4497u);
    EXPECT_EQ(
        picks[0].label,
        "Buy Heavy Brown Bag (10-slot bag vs 6-slot, 2g) from Bag Merchant");
    EXPECT_EQ(picks[1].item_id, 4498u);
    EXPECT_EQ(picks[1].label,
              "Buy Brown Leather Satchel (8-slot bag vs 6-slot, 25s) from Bag "
              "Merchant");
  }
}

}  // namespace
}  // namespace llm_brain
