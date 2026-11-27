// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/corpse_run.h"

#include <array>
#include <optional>
#include <span>

#include "bot_manager/tick_owner.h"
#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

using wlb::snapshot::Vec3;

CorpseRunInput BaseGhostInput() {
  CorpseRunInput in;
  in.tick_owner = TickOwner::kBrain;
  in.alive = false;
  in.is_ghost = true;
  in.in_battleground = false;
  in.in_arena = false;
  in.has_corpse = true;
  in.being_teleported = false;
  in.can_move = true;
  in.bot_map_id = 0;  // Eastern Kingdoms
  in.bot_pos = {-10500.0f, 1050.0f, 50.0f};
  in.corpse_map_id = 0;
  in.corpse_pos = {-10200.0f, 1200.0f, 45.0f};
  in.within_reclaim_range = false;
  in.reclaim_ready = true;
  return in;
}

TEST(CorpseRunTest, SoloOpenWorldWalksThenReclaims) {
  CorpseRunInput in = BaseGhostInput();
  // Far from corpse on the same map -> walk toward corpse.
  CorpseRunDecision d = EvaluateCorpseRun(in);
  EXPECT_EQ(d.action, CorpseRunAction::kWalkToTarget);
  EXPECT_EQ(d.target_pos.x, in.corpse_pos.x);
  EXPECT_EQ(d.target_pos.y, in.corpse_pos.y);
  EXPECT_EQ(d.target_pos.z, in.corpse_pos.z);
  EXPECT_EQ(d.trigger_id, 0u);

  // Within reclaim radius, but 30 s reclaim delay not elapsed -> wait.
  in.within_reclaim_range = true;
  in.reclaim_ready = false;
  d = EvaluateCorpseRun(in);
  EXPECT_EQ(d.action, CorpseRunAction::kWaitReclaimDelay);

  // Within reclaim radius and delay elapsed -> reclaim corpse normally.
  in.reclaim_ready = true;
  d = EvaluateCorpseRun(in);
  EXPECT_EQ(d.action, CorpseRunAction::kReclaimCorpse);
}

TEST(CorpseRunTest, InstanceWipeWalksToPortalThenEntersAreaTrigger) {
  // Died inside Deadmines (map 36), released to Westfall graveyard (map 0).
  const InstanceEntranceCandidate deadmines_portal{
      .trigger_id = 119,
      .trigger_map_id = 0,
      .trigger_pos = {-11208.0f, 1673.0f, 24.0f},
      .target_map_id = 36,
      .target_pos = {-16.4f, -383.0f, 61.7f},
      .bot_inside_trigger = false,
  };
  const InstanceEntranceCandidate stockades_portal{
      .trigger_id = 101,
      .trigger_map_id = 0,
      .trigger_pos = {-8768.0f, 845.0f, 88.0f},
      .target_map_id = 34,
      .target_pos = {48.0f, 0.5f, -16.4f},
      .bot_inside_trigger = false,
  };
  const std::array<InstanceEntranceCandidate, 2> candidates = {
      stockades_portal, deadmines_portal};

  CorpseRunInput in = BaseGhostInput();
  in.bot_map_id = 0;
  in.corpse_map_id = 36;
  in.corpse_pos = {-120.0f, -500.0f, 30.0f};
  in.instance_entrance = SelectInstanceEntrance(
      in.bot_map_id, in.bot_pos, in.corpse_map_id, in.corpse_pos, candidates);
  ASSERT_TRUE(in.instance_entrance.has_value());
  EXPECT_EQ(in.instance_entrance->trigger_id, 119u);

  // Outside the portal -> walk to the Deadmines AreaTrigger.
  CorpseRunDecision d = EvaluateCorpseRun(in);
  EXPECT_EQ(d.action, CorpseRunAction::kWalkToTarget);
  EXPECT_EQ(d.trigger_id, 119u);
  EXPECT_EQ(d.target_pos.x, deadmines_portal.trigger_pos.x);
  EXPECT_EQ(d.target_pos.y, deadmines_portal.trigger_pos.y);

  // Ghost reaches the portal radius -> fire CMSG_AREATRIGGER.
  in.instance_entrance->bot_inside_trigger = true;
  d = EvaluateCorpseRun(in);
  EXPECT_EQ(d.action, CorpseRunAction::kEnterInstanceTrigger);
  EXPECT_EQ(d.trigger_id, 119u);
}

TEST(CorpseRunTest, MultiWingInstancePicksWingMatchingCorpse) {
  // Scarlet Monastery (map 189) has multiple exterior portals on map 0.
  const InstanceEntranceCandidate library{
      .trigger_id = 610,
      .trigger_map_id = 0,
      .trigger_pos = {2870.0f, -764.0f, 160.0f},
      .target_map_id = 189,
      .target_pos = {253.0f, -206.0f, 18.0f},
      .bot_inside_trigger = false,
  };
  const InstanceEntranceCandidate cathedral{
      .trigger_id = 614,
      .trigger_map_id = 0,
      .trigger_pos = {2915.0f, -802.0f, 160.0f},
      .target_map_id = 189,
      .target_pos = {853.0f, 1319.0f, 18.0f},
      .bot_inside_trigger = false,
  };
  const std::array<InstanceEntranceCandidate, 2> candidates = {library,
                                                               cathedral};

  // Corpse is inside Cathedral -> picks Cathedral portal (614) even if the
  // ghost is closer to Library outside.
  const auto chosen = SelectInstanceEntrance(
      /*bot_map_id=*/0, Vec3{2860.0f, -760.0f, 160.0f},
      /*corpse_map_id=*/189, Vec3{900.0f, 1350.0f, 18.0f}, candidates);
  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(chosen->trigger_id, 614u);

  // If the ghost is already standing inside the Library portal, enter it
  // immediately (entering any portal to map 189 resurrects the ghost).
  std::array<InstanceEntranceCandidate, 2> inside_lib = {library, cathedral};
  inside_lib[0].bot_inside_trigger = true;
  const auto immediate = SelectInstanceEntrance(
      /*bot_map_id=*/0, Vec3{2870.0f, -764.0f, 160.0f},
      /*corpse_map_id=*/189, Vec3{900.0f, 1350.0f, 18.0f}, inside_lib);
  ASSERT_TRUE(immediate.has_value());
  EXPECT_EQ(immediate->trigger_id, 610u);
}

// Adversarial counter-tests: no cheat teleports, no action when harness
// disconnects, no action for non-ghosts or battlegrounds, and mismatched
// entrance triggers are rejected.
TEST(CorpseRunTest, HarnessDisconnectFallsBackToPlayerbots) {
  CorpseRunInput in = BaseGhostInput();
  in.tick_owner = TickOwner::kPlayerbots;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);

  // Also during instance wipe when harness disconnects.
  in.corpse_map_id = 36;
  in.instance_entrance = InstanceEntranceCandidate{
      .trigger_id = 119,
      .trigger_map_id = 0,
      .trigger_pos = {-11208.0f, 1673.0f, 24.0f},
      .target_map_id = 36,
      .target_pos = {},
      .bot_inside_trigger = true,
  };
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);
}

TEST(CorpseRunTest, NoCheatTeleportWhenEntranceMissingOrMismatched) {
  CorpseRunInput in = BaseGhostInput();
  in.bot_map_id = 0;
  in.corpse_map_id = 36;
  // No entrance trigger found on current map -> kNone (never teleports).
  in.instance_entrance = std::nullopt;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);

  // Mismatched target_map_id -> rejected.
  in.instance_entrance = InstanceEntranceCandidate{
      .trigger_id = 101,
      .trigger_map_id = 0,
      .trigger_pos = {-8768.0f, 845.0f, 88.0f},
      .target_map_id = 34,
      .target_pos = {},
      .bot_inside_trigger = true,
  };
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);

  // Mismatched trigger_map_id -> rejected.
  in.instance_entrance->trigger_map_id = 1;
  in.instance_entrance->target_map_id = 36;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);

  // SelectInstanceEntrance also ignores same-map calls and wrong maps.
  const std::array<InstanceEntranceCandidate, 1> wrong_map = {
      *in.instance_entrance};
  EXPECT_FALSE(
      SelectInstanceEntrance(0, in.bot_pos, 36, in.corpse_pos, wrong_map)
          .has_value());
  EXPECT_FALSE(
      SelectInstanceEntrance(36, in.bot_pos, 36, in.corpse_pos, wrong_map)
          .has_value());
}

TEST(CorpseRunTest, IgnoresLivingUnrleasedBgArenaAndTeleporting) {
  CorpseRunInput in = BaseGhostInput();

  // Alive bot -> kNone.
  in.alive = true;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);
  in.alive = false;

  // Dead but hasn't released spirit yet -> kNone.
  in.is_ghost = false;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);
  in.is_ghost = true;

  // Battleground / arena -> kNone.
  in.in_battleground = true;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);
  in.in_battleground = false;

  in.in_arena = true;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);
  in.in_arena = false;

  // No corpse -> kNone.
  in.has_corpse = false;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);
  in.has_corpse = true;

  // Mid-teleport -> kNone (prevents double CMSG_AREATRIGGER).
  in.being_teleported = true;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);
  in.being_teleported = false;

  // Cannot move (e.g. controlled motion) -> kNone when needing to walk,
  // but still allows reclaim when already in range.
  in.can_move = false;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kNone);
  in.within_reclaim_range = true;
  in.reclaim_ready = true;
  EXPECT_EQ(EvaluateCorpseRun(in).action, CorpseRunAction::kReclaimCorpse);
}

}  // namespace
}  // namespace llm_brain
