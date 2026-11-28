// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/bot_leader_guard.h"

#include "base/time/time.h"
#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(BotLeaderGuardTest, BlocksUnaskedActionsWhenHumanIsInGroup) {
  const base::TimeTicks t0 = base::TimeTicks::Now();
  const BotLeaderApprovalState empty;

  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue,
                                         /*group_has_human=*/true, empty, t0));
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kReadyCheck,
                                         /*group_has_human=*/true, empty, t0));
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite,
                                         /*group_has_human=*/true, empty, t0,
                                         "Stranger"));
}

// Adversarial counter-test: all-bot groups (and solo bots) are never blocked.
TEST(BotLeaderGuardTest, StillAllowedInAllBotGroupsAndSolo) {
  const base::TimeTicks t0 = base::TimeTicks::Now();
  const BotLeaderApprovalState empty;

  EXPECT_TRUE(CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue,
                                        /*group_has_human=*/false, empty, t0));
  EXPECT_TRUE(CanBotLeaderPerformAction(BotLeaderActionKind::kReadyCheck,
                                        /*group_has_human=*/false, empty, t0));
  EXPECT_TRUE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite,
                                        /*group_has_human=*/false, empty, t0,
                                        "Stranger"));
}

TEST(BotLeaderGuardTest, HumanChatApprovalAllowsRequestedActionOnly) {
  const base::TimeTicks t0 = base::TimeTicks::Now();
  BotLeaderApprovalState state;

  // Asking to queue Random Dungeon approves only LFG, not ready check/invite.
  EXPECT_TRUE(RecordHumanLeaderApproval(state, "can you queue rdf?", t0));
  EXPECT_TRUE(CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue, true,
                                        state, t0 + base::Seconds(5)));
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kReadyCheck, true,
                                         state, t0 + base::Seconds(5)));
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite, true,
                                         state, t0 + base::Seconds(5), "Bob"));

  // One-shot consumption blocks repeated autonomous queueing.
  ConsumeBotLeaderApproval(state, BotLeaderActionKind::kLfgQueue);
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue, true,
                                         state, t0 + base::Seconds(6)));

  // Asking for a ready check approves ready check within the window.
  EXPECT_TRUE(RecordHumanLeaderApproval(state, "do a ready check pls", t0));
  EXPECT_TRUE(CanBotLeaderPerformAction(BotLeaderActionKind::kReadyCheck, true,
                                        state, t0 + kBotLeaderApprovalWindow));
  // Expires after window.
  EXPECT_FALSE(CanBotLeaderPerformAction(
      BotLeaderActionKind::kReadyCheck, true, state,
      t0 + kBotLeaderApprovalWindow + base::Seconds(1)));
}

TEST(BotLeaderGuardTest, NamedInviteOnlyApprovesNamedPlayer) {
  const base::TimeTicks t0 = base::TimeTicks::Now();
  BotLeaderApprovalState state;

  EXPECT_TRUE(RecordHumanLeaderApproval(state, "invite Kelra please", t0));
  EXPECT_TRUE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite, true,
                                        state, t0 + base::Seconds(2), "Kelra"));
  EXPECT_TRUE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite, true,
                                        state, t0 + base::Seconds(2), "kelra"));
  // Adversarial: stranger must remain blocked when a specific name was asked.
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite, true,
                                         state, t0 + base::Seconds(2),
                                         "Stranger"));

  // Generic invite request ("invite someone") allows any candidate until used.
  EXPECT_TRUE(RecordHumanLeaderApproval(state, "inv someone to fill", t0));
  EXPECT_TRUE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite, true,
                                        state, t0 + base::Seconds(2),
                                        "Stranger"));
  ConsumeBotLeaderApproval(state, BotLeaderActionKind::kInvite);
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite, true,
                                         state, t0 + base::Seconds(3),
                                         "Stranger"));
}

// Adversarial counter-test: negated chat lines never grant approval and revoke
// any previously recorded approval; ClearBotLeaderApprovals resets everything.
TEST(BotLeaderGuardTest, NegationsRevokeAndNeverApprove) {
  const base::TimeTicks t0 = base::TimeTicks::Now();
  BotLeaderApprovalState state;

  EXPECT_FALSE(RecordHumanLeaderApproval(state, "don't queue lfg yet", t0));
  EXPECT_FALSE(RecordHumanLeaderApproval(state, "no ready check", t0));
  EXPECT_FALSE(RecordHumanLeaderApproval(state, "do not invite anyone", t0));

  EXPECT_TRUE(RecordHumanLeaderApproval(state, "queue random dungeon", t0));
  EXPECT_TRUE(RecordHumanLeaderApproval(state, "ready check", t0));
  EXPECT_TRUE(RecordHumanLeaderApproval(state, "invite Bob", t0));

  EXPECT_TRUE(RecordHumanLeaderApproval(state, "don't queue", t0));
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue, true,
                                         state, t0));
  EXPECT_TRUE(RecordHumanLeaderApproval(state, "no ready check", t0));
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kReadyCheck, true,
                                         state, t0));
  EXPECT_TRUE(RecordHumanLeaderApproval(state, "don't invite", t0));
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite, true,
                                         state, t0, "Bob"));

  EXPECT_TRUE(RecordHumanLeaderApproval(state, "queue rdf", t0));
  ClearBotLeaderApprovals(state);
  EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue, true,
                                         state, t0));
}

}  // namespace
}  // namespace llm_brain
