// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/errand_hold.h"

#include <cstdint>
#include <limits>

#include "gtest/gtest.h"
#include "options/action_types.h"

namespace llm_brain {
namespace {

ErrandHoldInput GroupedFollower(uint32_t now_ms = 10000) {
  return ErrandHoldInput{
      .in_group = true,
      .is_leader = false,
      .alive = true,
      .in_combat = false,
      .party_in_combat = false,
      .in_battleground = false,
      .harness_connected = true,
      .map_id = 0,
      .instance_id = 0,
      .now_ms = now_ms,
  };
}

TEST(ErrandHoldTest, ClassifiesBrainErrandsAndNonErrands) {
  EXPECT_EQ(ErrandKindForAction(LootAction{"0x1", "Defias"}),
            ErrandKind::kLoot);
  EXPECT_EQ(ErrandKindForAction(GatherAction{"0x2", "Peacebloom"}),
            ErrandKind::kGather);
  EXPECT_EQ(ErrandKindForAction(UseObjectAction{"0x3", "Quest Crate"}),
            ErrandKind::kUseObject);
  EXPECT_EQ(ErrandKindForAction(UseBagItemAction{6948, "Hearthstone"}),
            ErrandKind::kUseCast);
  EXPECT_EQ(ErrandKindForAction(MountAction{472, "Pinto"}),
            ErrandKind::kUseCast);
  EXPECT_EQ(ErrandKindForAction(DismountAction{}), ErrandKind::kNone);
  EXPECT_EQ(ErrandKindForAction(TalkAction{"0x4", "Marshal Dughan"}),
            ErrandKind::kTalk);
  EXPECT_EQ(
      ErrandKindForAction(NpcAction{NpcAction::Op::kQuestAccept, "0x4", 62, 0}),
      ErrandKind::kNpcInteract);
  EXPECT_EQ(
      ErrandKindForAction(NpcAction{NpcAction::Op::kQuestTurnIn, "0x4", 62, 0}),
      ErrandKind::kNpcInteract);
  EXPECT_EQ(ErrandKindForAction(NpcAction{NpcAction::Op::kVendor, "0x4", 0, 0}),
            ErrandKind::kNpcInteract);
  EXPECT_EQ(ErrandKindForAction(NpcAction{NpcAction::Op::kTrain, "0x4", 0, 0}),
            ErrandKind::kNpcInteract);
  EXPECT_EQ(ErrandKindForAction(
                NpcAction{NpcAction::Op::kTrainRiding, "0x4", 33388, 0}),
            ErrandKind::kNpcInteract);
  EXPECT_EQ(
      ErrandKindForAction(NpcAction{NpcAction::Op::kGossipOpen, "0x4", 0, 0}),
      ErrandKind::kNpcInteract);
  EXPECT_EQ(ErrandKindForAction(
                NpcAction{NpcAction::Op::kGossipSelect, "0x4", 500, 1}),
            ErrandKind::kNpcInteract);
  EXPECT_EQ(ErrandKindForAction(
                WalkAction{.id = "place:goldshire", .name = "Goldshire"}),
            ErrandKind::kWalk);
  EXPECT_EQ(ErrandKindForAction(RestAction{}), ErrandKind::kRest);

  // Flight paths and non-errands do not start an errand hold.
  EXPECT_EQ(ErrandKindForAction(NpcAction{NpcAction::Op::kFlyTo, "0x4", 2, 0}),
            ErrandKind::kNone);
  EXPECT_EQ(
      ErrandKindForAction(DestroyItemAction{255, 23, 4865, "Ruined Pelt"}),
      ErrandKind::kNone);
  EXPECT_EQ(ErrandKindForAction(AttackAction{"0x5", "Kobold"}),
            ErrandKind::kNone);
  EXPECT_EQ(ErrandKindForAction(FollowLeaderAction{"0x6"}), ErrandKind::kNone);
  EXPECT_EQ(ErrandKindForAction(KeepAction{}), ErrandKind::kNone);
  EXPECT_EQ(ErrandKindForAction(ReadyCheckAction{}), ErrandKind::kNone);
  EXPECT_EQ(ErrandKindForAction(LootRollAction{}), ErrandKind::kNone);
}

TEST(ErrandHoldTest, ActionCancelsErrandHoldForCombatFollowAndActs) {
  EXPECT_TRUE(ActionCancelsErrandHold(AttackAction{"0x5", "Kobold"}));
  EXPECT_TRUE(ActionCancelsErrandHold(TauntAction{355, "0x5", "Taunt"}));
  EXPECT_TRUE(ActionCancelsErrandHold(CrowdControlAction{118, "0x5", "Sheep"}));
  EXPECT_TRUE(ActionCancelsErrandHold(FollowLeaderAction{"0x6"}));
  EXPECT_TRUE(ActionCancelsErrandHold(
      GroupPolicyAction{GroupPolicyAction::Op::kFollow}));
  EXPECT_TRUE(ActionCancelsErrandHold(StandingActAction{}));

  // Keep and errand actions do not cancel an ongoing errand hold.
  EXPECT_FALSE(ActionCancelsErrandHold(KeepAction{}));
  EXPECT_FALSE(ActionCancelsErrandHold(LootAction{"0x1", "Defias"}));
  EXPECT_FALSE(
      ActionCancelsErrandHold(GroupPolicyAction{GroupPolicyAction::Op::kStay}));
}

TEST(ErrandHoldTest, GroupedFollowerErrandHoldsFollowAndLeashUntilDone) {
  ErrandHoldState state;
  ErrandHoldInput in = GroupedFollower(10000);

  ASSERT_TRUE(StartErrandHold(state, ErrandKind::kLoot, in));
  EXPECT_TRUE(state.active);
  EXPECT_EQ(state.kind, ErrandKind::kLoot);
  EXPECT_EQ(state.timeout_ms, kDefaultErrandTimeoutMs);

  // Still holding 1 ms before timeout.
  in.now_ms = 10000 + kDefaultErrandTimeoutMs - 1;
  EXPECT_TRUE(EvaluateErrandHold(state, in));
  EXPECT_TRUE(state.active);

  // Explicit clear on errand completion resumes follow immediately.
  EXPECT_TRUE(ClearErrandHold(state));
  EXPECT_FALSE(state.active);
  EXPECT_FALSE(EvaluateErrandHold(state, in));
  EXPECT_FALSE(ClearErrandHold(state));
}

TEST(ErrandHoldTest, WalkErrandUsesLongerTimeoutAndHandlesWrapAround) {
  ErrandHoldState state;
  // Start 10 s before uint32_t wrap-around.
  ErrandHoldInput in =
      GroupedFollower(std::numeric_limits<uint32_t>::max() - 10000u);
  ASSERT_TRUE(StartErrandHold(state, ErrandKind::kWalk, in));
  EXPECT_EQ(state.timeout_ms, kWalkErrandTimeoutMs);

  // 45 s later (wrapped past 0): still active for a 60 s walk errand.
  in.now_ms = 35000u;
  EXPECT_TRUE(EvaluateErrandHold(state, in));

  // 60 s later: timed out and cleared so 40 yd leash / follow resume.
  in.now_ms = 50000u;
  EXPECT_FALSE(EvaluateErrandHold(state, in));
  EXPECT_FALSE(state.active);
}

// Adversarial counter-test: P0 combat on the bot immediately preempts an active
// errand hold so combat and follow reflexes resume on the very same tick.
TEST(ErrandHoldTest, SelfCombatPreemptsErrandImmediately) {
  ErrandHoldState state;
  ErrandHoldInput in = GroupedFollower(5000);
  ASSERT_TRUE(StartErrandHold(state, ErrandKind::kUseObject, in));

  in.now_ms = 5001;
  in.in_combat = true;
  EXPECT_FALSE(EvaluateErrandHold(state, in));
  EXPECT_FALSE(state.active);

  // Cannot start a new errand hold while in combat.
  EXPECT_FALSE(StartErrandHold(state, ErrandKind::kLoot, in));
  EXPECT_FALSE(state.active);
}

// Adversarial counter-test: P0 party combat (e.g. tank pulls a pack while a
// follower is walking to loot or gather) immediately preempts the errand hold.
TEST(ErrandHoldTest, PartyCombatPreemptsErrandImmediately) {
  ErrandHoldState state;
  ErrandHoldInput in = GroupedFollower(5000);
  ASSERT_TRUE(StartErrandHold(state, ErrandKind::kGather, in));

  in.now_ms = 5100;
  in.party_in_combat = true;
  EXPECT_FALSE(EvaluateErrandHold(state, in));
  EXPECT_FALSE(state.active);

  // Cannot start a new errand hold while the party is in combat.
  EXPECT_FALSE(StartErrandHold(state, ErrandKind::kTalk, in));
  EXPECT_FALSE(state.active);
}

// Adversarial counter-test: a stuck errand times out automatically and resumes
// follow / 40 yd leash so the bot is never permanently orphaned.
TEST(ErrandHoldTest, TimeoutResumesFollowAndLeash) {
  ErrandHoldState state;
  ErrandHoldInput in = GroupedFollower(20000);
  ASSERT_TRUE(StartErrandHold(state, ErrandKind::kNpcInteract, in));
  EXPECT_EQ(state.timeout_ms, kNpcInteractionGraceMs);

  in.now_ms = 20000 + kNpcInteractionGraceMs - 1;
  EXPECT_TRUE(EvaluateErrandHold(state, in));

  in.now_ms = 20000 + kNpcInteractionGraceMs;
  EXPECT_FALSE(EvaluateErrandHold(state, in));
  EXPECT_FALSE(state.active);

  // Subsequent ticks stay cleared even if time does not advance.
  EXPECT_FALSE(EvaluateErrandHold(state, in));
}

// Adversarial counter-test: solo bots and group leaders are completely
// unaffected by errand holds, and leaving a group or becoming leader mid-errand
// clears the hold immediately.
TEST(ErrandHoldTest, SoloAndLeaderAreUnaffected) {
  ErrandHoldState state;

  // Solo cannot start an errand hold.
  ErrandHoldInput solo = GroupedFollower(1000);
  solo.in_group = false;
  EXPECT_FALSE(StartErrandHold(state, ErrandKind::kLoot, solo));
  EXPECT_FALSE(state.active);

  // Group leader cannot start an errand hold.
  ErrandHoldInput leader = GroupedFollower(1000);
  leader.is_leader = true;
  EXPECT_FALSE(StartErrandHold(state, ErrandKind::kLoot, leader));
  EXPECT_FALSE(state.active);

  // Leaving a group mid-errand clears the hold.
  ErrandHoldInput follower = GroupedFollower(1000);
  ASSERT_TRUE(StartErrandHold(state, ErrandKind::kTalk, follower));
  follower.in_group = false;
  EXPECT_FALSE(EvaluateErrandHold(state, follower));
  EXPECT_FALSE(state.active);

  // Becoming leader mid-errand clears the hold.
  follower = GroupedFollower(2000);
  ASSERT_TRUE(StartErrandHold(state, ErrandKind::kTalk, follower));
  follower.is_leader = true;
  EXPECT_FALSE(EvaluateErrandHold(state, follower));
  EXPECT_FALSE(state.active);
}

// Adversarial counter-test: death, map/instance change, battleground entry, and
// harness disconnect all clear an active errand hold immediately.
TEST(ErrandHoldTest, DeathMapChangeBattlegroundAndDisconnectClearHold) {
  const auto check_clears = [](auto mutate) {
    ErrandHoldState state;
    ErrandHoldInput in = GroupedFollower(3000);
    in.map_id = 36;
    in.instance_id = 7;
    ASSERT_TRUE(StartErrandHold(state, ErrandKind::kLoot, in));
    mutate(in);
    EXPECT_FALSE(EvaluateErrandHold(state, in));
    EXPECT_FALSE(state.active);
  };

  check_clears([](ErrandHoldInput& in) { in.alive = false; });
  check_clears([](ErrandHoldInput& in) { in.map_id = 0; });
  check_clears([](ErrandHoldInput& in) { in.instance_id = 8; });
  check_clears([](ErrandHoldInput& in) { in.in_battleground = true; });
  check_clears([](ErrandHoldInput& in) { in.harness_connected = false; });
}

}  // namespace
}  // namespace llm_brain
