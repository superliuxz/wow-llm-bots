// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/action_types.h"

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
    BotAction a = DecodeOptionFactAction("roll:0x100:3:3", "Disenchant Sword");
    const auto* roll = std::get_if<LootRollAction>(&a);
    ASSERT_NE(roll, nullptr);
    EXPECT_EQ(roll->vote, 3u);
    EXPECT_TRUE(IsDeadAllowedAction(a));
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
    BotAction a = DecodeOptionFactAction("mount:23249", "Mount Raptor");
    const auto* m = std::get_if<MountAction>(&a);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->spell_id, 23249u);
    EXPECT_TRUE(IsPendingCastAction(a));
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

}  // namespace
}  // namespace llm_brain
