// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/action_types.h"

#include <utility>
#include <variant>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(ActionTypesTest, DecodesTypedNpcAndGroupActions) {
  {
    BotAction a = DecodeOptionFactAction("npc_qturnin:0x1234:788:2", "Turn in");
    const auto* npc = std::get_if<NpcAction>(&a);
    ASSERT_NE(npc, nullptr);
    EXPECT_EQ(npc->op, NpcAction::Op::kQuestTurnIn);
    EXPECT_EQ(npc->npc_guid, "0x1234");
    EXPECT_EQ(npc->arg1, 788u);
    EXPECT_EQ(npc->arg2, 2u);
  }
  {
    BotAction a = DecodeOptionFactAction("roll:0x100:3:1", "Need Sword");
    const auto* roll = std::get_if<LootRollAction>(&a);
    ASSERT_NE(roll, nullptr);
    EXPECT_EQ(roll->item_guid, "0x100");
    EXPECT_EQ(roll->slot, 3u);
    EXPECT_EQ(roll->vote, 1u);
  }
  {
    BotAction a = DecodeOptionFactAction("ready:start", "Start ready check");
    const auto* rc = std::get_if<ReadyCheckAction>(&a);
    ASSERT_NE(rc, nullptr);
    EXPECT_TRUE(rc->start);
  }
  {
    BotAction a = DecodeOptionFactAction("cc:118:0xF130", "Polymorph");
    const auto* cc = std::get_if<CrowdControlAction>(&a);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->spell_id, 118u);
    EXPECT_EQ(cc->target_guid, "0xF130");
    EXPECT_TRUE(std::holds_alternative<InvalidAction>(
        DecodeOptionFactAction("cc:0:0xF130", "bad")));
  }
  {
    BotAction a = DecodeOptionFactAction("taunt:355:0xF130",
                                         "Taunt Deathbringer Saurfang");
    const auto* taunt = std::get_if<TauntAction>(&a);
    ASSERT_NE(taunt, nullptr);
    EXPECT_EQ(taunt->spell_id, 355u);
    EXPECT_EQ(taunt->target_guid, "0xF130");
    EXPECT_EQ(taunt->label, "Taunt Deathbringer Saurfang");
    EXPECT_EQ(ActionKindName(a), "taunt");
    EXPECT_FALSE(IsDeadAllowedAction(a));
    for (const char* bad :
         {"taunt:0:0xF130", "taunt:355:", "taunt:bad:0xF130", "taunt:355"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
  {
    BotAction a = DecodeOptionFactAction(
        "brez:48477:0x10", "Battle res Hele (healer) with Rebirth");
    const auto* brez = std::get_if<BattleResAction>(&a);
    ASSERT_NE(brez, nullptr);
    EXPECT_EQ(brez->spell_id, 48477u);
    EXPECT_EQ(brez->item_entry, 0u);
    EXPECT_EQ(brez->target_guid, "0x10");
    EXPECT_EQ(ActionKindName(a), "battle_res");
    EXPECT_FALSE(IsDeadAllowedAction(a));
    BotAction ss = DecodeOptionFactAction("soulstone:36895:0x11",
                                          "Use Demonic Soulstone on Hele");
    const auto* ss_act = std::get_if<BattleResAction>(&ss);
    ASSERT_NE(ss_act, nullptr);
    EXPECT_EQ(ss_act->spell_id, 0u);
    EXPECT_EQ(ss_act->item_entry, 36895u);
    EXPECT_EQ(ss_act->target_guid, "0x11");
    for (const char* bad : {"brez:0:0x10", "brez:48477:", "soulstone:0:0x11",
                            "soulstone:36895"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
  {
    BotAction a = DecodeOptionFactAction("roll:0x100:3:3", "Disenchant Sword");
    const auto* roll = std::get_if<LootRollAction>(&a);
    ASSERT_NE(roll, nullptr);
    EXPECT_EQ(roll->vote, 3u);
    EXPECT_TRUE(IsDeadAllowedAction(a));
  }
  {
    BotAction a = DecodeOptionFactAction(
        "rez:spirit", "Resurrect at Spirit Healer (resurrection sickness)");
    const auto* rez = std::get_if<ResurrectAction>(&a);
    ASSERT_NE(rez, nullptr);
    EXPECT_EQ(rez->mode, ResurrectAction::Mode::kSpiritHealer);
    EXPECT_TRUE(IsDeadAllowedAction(a));
  }
  {
    BotAction a = DecodeOptionFactAction("summon_stone:0x9010:0x42",
                                         "Summon Human at Meeting Stone");
    const auto* ms = std::get_if<MeetingStoneSummonAction>(&a);
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->object_guid, "0x9010");
    EXPECT_EQ(ms->target_guid, "0x42");
    EXPECT_EQ(ActionKindName(a), "meeting_stone_summon");
    EXPECT_FALSE(IsDeadAllowedAction(a));
    for (const char* bad : {"summon_stone:", "summon_stone:0x9010:",
                            "summon_stone::0x42", "summon_stone:0x1:0x2:0x3"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
  {
    BotAction a = DecodeOptionFactAction("mark:0xabc:7", "Mark Skull");
    const auto* mark = std::get_if<MarkTargetAction>(&a);
    ASSERT_NE(mark, nullptr);
    EXPECT_EQ(mark->target_guid, "0xabc");
    EXPECT_EQ(mark->icon, 7u);
  }
  {
    BotAction a = DecodeOptionFactAction("lfg:accept:42", "Accept");
    const auto* lfg = std::get_if<LfgAction>(&a);
    ASSERT_NE(lfg, nullptr);
    EXPECT_EQ(lfg->op, LfgAction::Op::kAcceptProposal);
    EXPECT_EQ(lfg->value, 42u);
  }
  {
    BotAction a = DecodeOptionFactAction("grouppolicy:stay", "Wait here");
    const auto* gp = std::get_if<GroupPolicyAction>(&a);
    ASSERT_NE(gp, nullptr);
    EXPECT_EQ(gp->op, GroupPolicyAction::Op::kStay);
  }
  {
    BotAction p = DecodeOptionFactAction("group:promote:0x42",
                                         "Promote Arthas to party leader");
    const auto* gl = std::get_if<GroupLeaderAction>(&p);
    ASSERT_NE(gl, nullptr);
    EXPECT_EQ(gl->op, GroupLeaderAction::Op::kPromote);
    EXPECT_EQ(gl->target_guid, "0x42");
    EXPECT_EQ(ActionKindName(p), "group_leader");
    EXPECT_FALSE(IsDeadAllowedAction(p));

    BotAction d = DecodeOptionFactAction("group:difficulty:1",
                                         "Set dungeon difficulty to Heroic");
    const auto* gld = std::get_if<GroupLeaderAction>(&d);
    ASSERT_NE(gld, nullptr);
    EXPECT_EQ(gld->op, GroupLeaderAction::Op::kSetDifficulty);
    EXPECT_EQ(gld->value, 1u);

    BotAction l = DecodeOptionFactAction("group:loot:0",
                                         "Set loot method to Free-for-All");
    const auto* gll = std::get_if<GroupLeaderAction>(&l);
    ASSERT_NE(gll, nullptr);
    EXPECT_EQ(gll->op, GroupLeaderAction::Op::kSetLootMethod);
    EXPECT_EQ(gll->value, 0u);

    BotAction r = DecodeOptionFactAction("group:reset", "Reset all instances");
    const auto* glr = std::get_if<GroupLeaderAction>(&r);
    ASSERT_NE(glr, nullptr);
    EXPECT_EQ(glr->op, GroupLeaderAction::Op::kResetInstances);

    for (const char* bad : {"group:promote:", "group:difficulty:2",
                            "group:loot:3", "group:loot:5", "group:bogus"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
  {
    BotAction a = DecodeOptionFactAction("qabandon:176",
                                         "Abandon quest \"Wanted: Hogger\"");
    const auto* ab = std::get_if<AbandonQuestAction>(&a);
    ASSERT_NE(ab, nullptr);
    EXPECT_EQ(ab->quest_id, 176u);
    EXPECT_EQ(ActionKindName(a), "abandon_quest");
    EXPECT_FALSE(IsDeadAllowedAction(a));
    EXPECT_TRUE(std::holds_alternative<InvalidAction>(
        DecodeOptionFactAction("qabandon:0", "bad")));
    EXPECT_TRUE(std::holds_alternative<InvalidAction>(
        DecodeOptionFactAction("qabandon:abc", "bad")));
  }
  {
    BotAction a = DecodeOptionFactAction(
        "go_qaccept:0xF1100001:176",
        "Accept quest \"Wanted: \"Hogger\"\" from Wanted Poster");
    const auto* oq = std::get_if<ObjectQuestAction>(&a);
    ASSERT_NE(oq, nullptr);
    EXPECT_EQ(oq->op, ObjectQuestAction::Op::kAccept);
    EXPECT_EQ(oq->object_guid, "0xF1100001");
    EXPECT_EQ(oq->quest_id, 176u);
    EXPECT_EQ(ActionKindName(a), "object_quest");
    BotAction ti = DecodeOptionFactAction(
        "go_qturnin:0xF1100001:176:1",
        "Turn in \"Wanted: \"Hogger\"\" to Wanted Poster");
    const auto* oq2 = std::get_if<ObjectQuestAction>(&ti);
    ASSERT_NE(oq2, nullptr);
    EXPECT_EQ(oq2->op, ObjectQuestAction::Op::kTurnIn);
    EXPECT_EQ(oq2->reward_idx, 1u);
    for (const char* bad :
         {"go_qaccept::176", "go_qaccept:0x1:0", "go_qturnin:0x1:176"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
  {
    BotAction a = DecodeOptionFactAction(
        "item_qaccept:1972:184",
        "Accept quest \"Furlbrow's Deed\" from Westfall Deed");
    const auto* iq = std::get_if<ItemQuestAction>(&a);
    ASSERT_NE(iq, nullptr);
    EXPECT_EQ(iq->item_entry, 1972u);
    EXPECT_EQ(iq->quest_id, 184u);
    EXPECT_EQ(ActionKindName(a), "item_quest");
    for (const char* bad :
         {"item_qaccept:0:184", "item_qaccept:1972:0", "item_qaccept:1972"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
  {
    BotAction a = DecodeOptionFactAction("mount:23249", "Mount Raptor");
    const auto* m = std::get_if<MountAction>(&a);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->spell_id, 23249u);
    EXPECT_TRUE(IsPendingCastAction(a));
    EXPECT_TRUE(std::holds_alternative<InvalidAction>(
        DecodeOptionFactAction("mount:0", "bad mount")));
  }
  {
    BotAction a = DecodeOptionFactAction("dismount", "Dismount");
    EXPECT_TRUE(std::holds_alternative<DismountAction>(a));
    EXPECT_EQ(ActionKindName(a), "dismount");
    EXPECT_FALSE(IsDeadAllowedAction(a));
    EXPECT_FALSE(IsPendingCastAction(a));
  }
  {
    BotAction leave = DecodeOptionFactAction("vehicle:leave", "Leave vehicle");
    EXPECT_TRUE(std::holds_alternative<LeaveVehicleAction>(leave));
    EXPECT_EQ(ActionKindName(leave), "leave_vehicle");
    EXPECT_FALSE(IsDeadAllowedAction(leave));
    EXPECT_FALSE(IsPendingCastAction(leave));

    BotAction spell = DecodeOptionFactAction("vehicle:spell:50240",
                                             "Use Flame Spike (vehicle)");
    const auto* vs = std::get_if<VehicleSpellAction>(&spell);
    ASSERT_NE(vs, nullptr);
    EXPECT_EQ(vs->spell_id, 50240u);
    EXPECT_EQ(vs->label, "Use Flame Spike (vehicle)");
    EXPECT_EQ(ActionKindName(spell), "vehicle_spell");
    EXPECT_FALSE(IsDeadAllowedAction(spell));
    for (const char* bad : {"vehicle:spell:", "vehicle:spell:0",
                            "vehicle:spell:abc", "vehicle:bogus"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
  {
    BotAction a =
        DecodeOptionFactAction("npc_train_riding:0x1234:33388",
                               "Train Apprentice Riding (4g) at Randal Hunter");
    const auto* npc = std::get_if<NpcAction>(&a);
    ASSERT_NE(npc, nullptr);
    EXPECT_EQ(npc->op, NpcAction::Op::kTrainRiding);
    EXPECT_EQ(npc->npc_guid, "0x1234");
    EXPECT_EQ(npc->arg1, 33388u);
    for (const char* bad :
         {"npc_train_riding:0x1234", "npc_train_riding:0x1234:0",
          "npc_train_riding:0x1234:abc"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
  {
    BotAction single = DecodeOptionFactAction("npc_buy:0x1234:4498",
                                              "Buy Brown Leather Satchel");
    const auto* npc_single = std::get_if<NpcAction>(&single);
    ASSERT_NE(npc_single, nullptr);
    EXPECT_EQ(npc_single->op, NpcAction::Op::kBuy);
    EXPECT_EQ(npc_single->npc_guid, "0x1234");
    EXPECT_EQ(npc_single->arg1, 4498u);
    EXPECT_EQ(npc_single->arg2, 1u);

    BotAction stack = DecodeOptionFactAction(
        "npc_buy:0x1234:17034:20", "Buy Maple Seed x20 (10s) from Vendor");
    const auto* npc_stack = std::get_if<NpcAction>(&stack);
    ASSERT_NE(npc_stack, nullptr);
    EXPECT_EQ(npc_stack->op, NpcAction::Op::kBuy);
    EXPECT_EQ(npc_stack->npc_guid, "0x1234");
    EXPECT_EQ(npc_stack->arg1, 17034u);
    EXPECT_EQ(npc_stack->arg2, 20u);

    for (const char* bad :
         {"npc_buy:0x1234", "npc_buy:0x1234:0", "npc_buy:0x1234:17034:0",
          "npc_buy:0x1234:17034:256", "npc_buy:0x1234:17034:abc"}) {
      EXPECT_TRUE(std::holds_alternative<InvalidAction>(
          DecodeOptionFactAction(bad, "bad")))
          << bad;
    }
  }
}

TEST(ActionTypesTest, DecodesTradeWindowChoices) {
  {
    BotAction a = DecodeOptionFactAction("trade:open:0x0000000000000042",
                                         "Open a trade with Hele");
    const auto* t = std::get_if<TradeAction>(&a);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->op, TradeAction::Op::kOpen);
    EXPECT_EQ(t->target_guid, "0x0000000000000042");
    EXPECT_EQ(ActionKindName(a), "trade");
    EXPECT_FALSE(IsDeadAllowedAction(a));
  }
  {
    BotAction a = DecodeOptionFactAction("trade:put:255:23:5350",
                                         "Put Conjured Water in the trade");
    const auto* t = std::get_if<TradeAction>(&a);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->op, TradeAction::Op::kPutItem);
    EXPECT_EQ(t->bag, 255u);
    EXPECT_EQ(t->slot, 23u);
    EXPECT_EQ(t->entry, 5350u);
  }
  for (const auto& [guid, op] :
       {std::pair{"trade:accept", TradeAction::Op::kAccept},
        std::pair{"trade:cancel", TradeAction::Op::kCancel}}) {
    BotAction a = DecodeOptionFactAction(guid, "x");
    const auto* t = std::get_if<TradeAction>(&a);
    ASSERT_NE(t, nullptr) << guid;
    EXPECT_EQ(t->op, op) << guid;
  }
  // Counter-tests: malformed trade guids never become another action.
  for (const char* bad :
       {"trade:", "trade:open:", "trade:put:255:23", "trade:put:256:1:5350",
        "trade:put:255:1:0", "trade:put:a:b:c", "trade:give:1"}) {
    EXPECT_TRUE(std::holds_alternative<InvalidAction>(
        DecodeOptionFactAction(bad, "bad")))
        << bad;
  }
}

TEST(ActionTypesTest, RejectsMalformedFactGuids) {
  EXPECT_TRUE(std::holds_alternative<InvalidAction>(
      DecodeOptionFactAction("roll:0x100:0:9", "bad vote")));
  EXPECT_TRUE(std::holds_alternative<InvalidAction>(
      DecodeOptionFactAction("mark:0xabc:8", "bad icon")));
  EXPECT_TRUE(std::holds_alternative<InvalidAction>(
      DecodeOptionFactAction("talent:5", "bad tab")));
  EXPECT_TRUE(std::holds_alternative<InvalidAction>(
      DecodeOptionFactAction("item:0", "zero entry")));
}

TEST(ActionTypesTest, ValidateTradeGoldRejectsInvalidStateAndAmounts) {
  EXPECT_TRUE(ValidateTradeGold(/*has_trade=*/true, 1, 1).empty());
  EXPECT_TRUE(ValidateTradeGold(/*has_trade=*/true, 50000, 50000).empty());
  EXPECT_TRUE(ValidateTradeGold(/*has_trade=*/true, 25000, 50000).empty());
  // Counter-tests: non-trade state, zero, and exceeding bot's money.
  EXPECT_EQ(ValidateTradeGold(/*has_trade=*/false, 100, 50000),
            "no trade window is open");
  EXPECT_EQ(ValidateTradeGold(/*has_trade=*/true, 0, 50000),
            "invalid trade gold amount");
  EXPECT_EQ(ValidateTradeGold(/*has_trade=*/true, 50001, 50000),
            "not enough money");
  EXPECT_EQ(ValidateTradeGold(/*has_trade=*/true, 1, 0), "not enough money");
}

TEST(ActionTypesTest, DecodesGossipAndDestroyActionsAndRejectsMalformed) {
  {
    BotAction a = DecodeOptionFactAction("npc_gossip_open:0x1234",
                                         "Talk to Brann (dialogue)");
    const auto* npc = std::get_if<NpcAction>(&a);
    ASSERT_NE(npc, nullptr);
    EXPECT_EQ(npc->op, NpcAction::Op::kGossipOpen);
    EXPECT_EQ(npc->npc_guid, "0x1234");
    EXPECT_FALSE(IsDeadAllowedAction(a));
  }
  {
    BotAction a = DecodeOptionFactAction("npc_gossip:0x1234:500:2",
                                         "Say to Brann: \"Ready\"");
    const auto* npc = std::get_if<NpcAction>(&a);
    ASSERT_NE(npc, nullptr);
    EXPECT_EQ(npc->op, NpcAction::Op::kGossipSelect);
    EXPECT_EQ(npc->npc_guid, "0x1234");
    EXPECT_EQ(npc->arg1, 500u);
    EXPECT_EQ(npc->arg2, 2u);
    EXPECT_FALSE(IsDeadAllowedAction(a));
  }
  for (const char* bad :
       {"npc_gossip_open:", "npc_gossip_open:0x1:extra", "npc_gossip:0x1234",
        "npc_gossip:0x1234:500", "npc_gossip::500:2", "npc_gossip:0x1234:a:2",
        "npc_gossip:0x1234:500:b"}) {
    EXPECT_TRUE(std::holds_alternative<InvalidAction>(
        DecodeOptionFactAction(bad, "bad")))
        << bad;
  }

  {
    BotAction a = DecodeOptionFactAction("destroy:255:23:4865",
                                         "Destroy Ruined Pelt (grey, 5c)");
    const auto* d = std::get_if<DestroyItemAction>(&a);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->bag, 255u);
    EXPECT_EQ(d->slot, 23u);
    EXPECT_EQ(d->entry, 4865u);
    EXPECT_EQ(d->name, "Destroy Ruined Pelt (grey, 5c)");
    EXPECT_EQ(ActionKindName(a), "destroy_item");
    EXPECT_FALSE(IsDeadAllowedAction(a));
    EXPECT_FALSE(IsPendingCastAction(a));
  }
  for (const char* bad :
       {"destroy:", "destroy:255:23", "destroy:256:23:4865",
        "destroy:255:256:4865", "destroy:255:23:0", "destroy:a:b:c"}) {
    EXPECT_TRUE(std::holds_alternative<InvalidAction>(
        DecodeOptionFactAction(bad, "bad")))
        << bad;
  }

  {
    BotAction a = DecodeOptionFactAction(
        "npc_sell:0x1234:255:24:2000",
        "Sell Green Blade to Brother Danil (green, ilvl 18, 5s)");
    const auto* npc = std::get_if<NpcAction>(&a);
    ASSERT_NE(npc, nullptr);
    EXPECT_EQ(npc->op, NpcAction::Op::kSellItem);
    EXPECT_EQ(npc->npc_guid, "0x1234");
    EXPECT_EQ(npc->arg1, 2000u);
    EXPECT_EQ(npc->arg2, (255u << 8) | 24u);
    EXPECT_FALSE(IsDeadAllowedAction(a));
    EXPECT_FALSE(IsPendingCastAction(a));
  }
  for (const char* bad :
       {"npc_sell:", "npc_sell:0x1234:255:24", "npc_sell::255:24:2000",
        "npc_sell:0x1234:256:24:2000", "npc_sell:0x1234:255:256:2000",
        "npc_sell:0x1234:255:24:0", "npc_sell:0x1234:a:b:c"}) {
    EXPECT_TRUE(std::holds_alternative<InvalidAction>(
        DecodeOptionFactAction(bad, "bad")))
        << bad;
  }
}

}  // namespace
}  // namespace llm_brain
