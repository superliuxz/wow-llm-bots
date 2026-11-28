// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/corpse_run.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "bot_manager/tick_owner.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

float DistSq3D(wlb::snapshot::Vec3 a, wlb::snapshot::Vec3 b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}

}  // namespace

std::optional<InstanceEntranceCandidate> SelectInstanceEntrance(
    uint32_t bot_map_id,
    wlb::snapshot::Vec3 bot_pos,
    uint32_t corpse_map_id,
    wlb::snapshot::Vec3 corpse_pos,
    std::span<const InstanceEntranceCandidate> candidates) {
  if (bot_map_id == corpse_map_id) {
    return std::nullopt;
  }
  const InstanceEntranceCandidate* best = nullptr;
  float best_target_dist_sq = std::numeric_limits<float>::infinity();
  float best_bot_dist_sq = std::numeric_limits<float>::infinity();

  for (const InstanceEntranceCandidate& cand : candidates) {
    if (cand.trigger_id == 0 || cand.trigger_map_id != bot_map_id ||
        cand.target_map_id != corpse_map_id) {
      continue;
    }
    if (cand.bot_inside_trigger) {
      return cand;
    }
    const float target_dist_sq = DistSq3D(cand.target_pos, corpse_pos);
    const float bot_dist_sq = DistSq3D(cand.trigger_pos, bot_pos);
    const bool better = !best || target_dist_sq < best_target_dist_sq ||
                        (target_dist_sq == best_target_dist_sq &&
                         (bot_dist_sq < best_bot_dist_sq ||
                          (bot_dist_sq == best_bot_dist_sq &&
                           cand.trigger_id < best->trigger_id)));
    if (better) {
      best = &cand;
      best_target_dist_sq = target_dist_sq;
      best_bot_dist_sq = bot_dist_sq;
    }
  }
  if (!best) {
    return std::nullopt;
  }
  return *best;
}

CorpseRunDecision EvaluateCorpseRun(const CorpseRunInput& input) {
  // Yield cleanly to playerbots when the harness is disconnected, LLM is
  // unhealthy, or the bot is in a battleground.
  if (input.tick_owner != TickOwner::kBrain) {
    return {};
  }
  if (input.alive || !input.is_ghost || input.in_battleground ||
      input.in_arena || !input.has_corpse || input.being_teleported) {
    return {};
  }

  // Open-world (or same-map) corpse run: walk to the corpse and reclaim once
  // inside CORPSE_RECLAIM_RADIUS and past the reclaim delay.
  if (input.bot_map_id == input.corpse_map_id) {
    if (input.within_reclaim_range) {
      return CorpseRunDecision{
          .action = input.reclaim_ready ? CorpseRunAction::kReclaimCorpse
                                        : CorpseRunAction::kWaitReclaimDelay,
          .target_pos = {},
          .trigger_id = 0};
    }
    if (!input.can_move) {
      return {};
    }
    return CorpseRunDecision{.action = CorpseRunAction::kWalkToTarget,
                             .target_pos = input.corpse_pos,
                             .trigger_id = 0};
  }

  // Instance wipe: corpse is on another map, ghost released to an exterior
  // graveyard. Walk to the matching instance entrance AreaTrigger and trigger
  // it once inside its radius (never cheat-teleport).
  if (!input.instance_entrance.has_value()) {
    return {};
  }
  const InstanceEntranceCandidate& ent = *input.instance_entrance;
  if (ent.trigger_id == 0 || ent.trigger_map_id != input.bot_map_id ||
      ent.target_map_id != input.corpse_map_id) {
    return {};
  }
  if (ent.bot_inside_trigger) {
    return {.action = CorpseRunAction::kEnterInstanceTrigger,
            .target_pos = ent.trigger_pos,
            .trigger_id = ent.trigger_id};
  }
  if (!input.can_move) {
    return {};
  }
  return {.action = CorpseRunAction::kWalkToTarget,
          .target_pos = ent.trigger_pos,
          .trigger_id = ent.trigger_id};
}

CorpseRunProgressResult UpdateCorpseRunProgress(CorpseRunProgressState* state,
                                                bool tracking_active,
                                                uint32_t map_id,
                                                float dist_to_target_yd,
                                                uint32_t now_ms,
                                                uint32_t stuck_window_ms) {
  if (!state) {
    return {};
  }
  if (!tracking_active) {
    *state = {};
    return {};
  }
  const bool finite_dist = std::isfinite(dist_to_target_yd);
  if (!state->tracking || state->map_id != map_id) {
    state->tracking = true;
    state->map_id = map_id;
    state->best_dist_yd = finite_dist ? dist_to_target_yd
                                      : std::numeric_limits<float>::infinity();
    state->last_progress_ms = now_ms;
    state->stuck = false;
    return {};
  }
  if (finite_dist &&
      (!std::isfinite(state->best_dist_yd) ||
       dist_to_target_yd + kCorpseRunProgressMinYd <= state->best_dist_yd)) {
    state->best_dist_yd = dist_to_target_yd;
    state->last_progress_ms = now_ms;
    state->stuck = false;
    return {};
  }
  // Unsigned subtraction handles uint32_t getMSTime() wrap-around cleanly.
  const uint32_t elapsed_ms = now_ms - state->last_progress_ms;
  if (elapsed_ms >= stuck_window_ms) {
    const bool newly_stuck = !state->stuck;
    state->stuck = true;
    return {.stuck = true, .newly_stuck = newly_stuck};
  }
  return {.stuck = state->stuck, .newly_stuck = false};
}

bool ShouldSendStuckGhostChatAlert(bool newly_stuck,
                                   bool in_group,
                                   bool group_has_human) {
  return newly_stuck && in_group && group_has_human;
}

bool ShouldHoldPositionForDeadLeader(const LeaderFollowInput& input) {
  if (!input.bot_alive || !input.in_group || input.bot_is_leader ||
      !input.leader_present) {
    return false;
  }
  return !input.leader_alive || input.leader_is_ghost;
}

}  // namespace llm_brain
