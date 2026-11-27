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
  // Protected item classes (container, quiver, key, projectile).
  for (uint32_t cls : {kItemClassContainer, kItemClassQuiver, kItemClassKey,
                       kItemClassProjectile}) {
    auto c = safe;
    c.item_class = cls;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty()) << cls;
  }
  // Uncommon (green) and higher quality items are never offered for
  // destruction.
  for (uint32_t q : {2u, 3u, 4u, 5u, 7u}) {
    auto c = safe;
    c.quality = q;
    EXPECT_FALSE(DestroySafetyBlockReason(c).empty()) << q;
  }
}

TEST(FullBagsDestroyTest, OnlyOfferedWhenBagsFullAliveOutOfCombatNotTrading) {
  const std::vector<DestroyItemCandidate> items = {
      MakeGreyJunk(255, 23, 4865, "Ruined Pelt", 5, 1),
  };
  EXPECT_TRUE(
      SelectDestroyItemOptions(/*free_bag_slots=*/1, true, false, false, items)
          .empty());
  EXPECT_TRUE(SelectDestroyItemOptions(0, /*alive=*/false, false, false, items)
                  .empty());
  EXPECT_TRUE(
      SelectDestroyItemOptions(0, true, /*in_combat=*/true, false, items)
          .empty());
  EXPECT_TRUE(SelectDestroyItemOptions(0, true, false, /*in_trade=*/true, items)
                  .empty());
  EXPECT_EQ(SelectDestroyItemOptions(0, true, false, false, items).size(), 1u);
}

TEST(FullBagsDestroyTest, RanksGreyBeforeWhiteCheapestFirstAndDeduplicates) {
  auto white_food = MakeGreyJunk(255, 24, 117, "Tough Jerky", 1, 2);
  white_food.quality = kItemQualityNormal;
  auto quest_item = MakeGreyJunk(255, 25, 9999, "Boar Snout", 0, 1);
  quest_item.required_for_active_quest = true;
  auto green_sword = MakeGreyJunk(255, 26, 2000, "Green Blade", 10, 1);
  green_sword.quality = 2;

  const std::vector<DestroyItemCandidate> items = {
      white_food,
      quest_item,
      green_sword,
      MakeGreyJunk(255, 27, 4865, "Ruined Pelt", 5, 4),  // 20c
      MakeGreyJunk(255, 28, 4865, "Ruined Pelt", 5, 1),  // 5c (cheaper stack)
      MakeGreyJunk(255, 29, 7098, "Splintered Tusk", 0, 1),  // 0c
      MakeGreyJunk(19, 0, 7099, "Cracked Fang", 8, 1),       // 8c
      MakeGreyJunk(19, 1, 7100, "Chipped Claw", 12, 1),      // 12c
  };

  const std::vector<DestroyItemOption> opts =
      SelectDestroyItemOptions(0, true, false, false, items, /*max_options=*/4);
  ASSERT_EQ(opts.size(), 4u);
  EXPECT_EQ(opts[0].guid, "destroy:255:29:7098");
  EXPECT_EQ(opts[0].label, "Destroy Splintered Tusk (grey, no value)");
  // Deduplicates 4865, keeping the cheaper single-item stack in slot 28.
  EXPECT_EQ(opts[1].guid, "destroy:255:28:4865");
  EXPECT_EQ(opts[1].label, "Destroy Ruined Pelt (grey, 5c)");
  EXPECT_EQ(opts[2].guid, "destroy:19:0:7099");
  EXPECT_EQ(opts[2].label, "Destroy Cracked Fang (grey, 8c)");
  EXPECT_EQ(opts[3].guid, "destroy:19:1:7100");
  EXPECT_EQ(opts[3].label, "Destroy Chipped Claw (grey, 12c)");
}

}  // namespace
}  // namespace llm_brain
