// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure C++ corpse-run and ghost instance re-entry logic (Milestone 4a #6).
//
// When a bot is brain-led, playerbots' own corpse-run actions (`find corpse`,
// `revive from corpse`, `spirit healer`, `repop`) are gated because they ignore
// the corpse's map, revive at spirit healers without resurrection sickness, and
// cheat-teleport. This file decides what a ghost should do each tick using only
// client-legal mechanics:
//  - Same map: walk to the corpse and send CMSG_RECLAIM_CORPSE once within
//    CORPSE_RECLAIM_RADIUS and past the reclaim delay.
//  - Different map (instance wipe): walk to the exterior instance entrance
//    AreaTrigger matching the corpse's map and send CMSG_AREATRIGGER once
//    inside the trigger radius (WotLK 3.3.5a automatically resurrects the ghost
//    inside the entrance).
//  - Never teleports directly to the corpse or revives at a spirit healer
//    without sickness.
//  - Yields cleanly when `tick_owner != TickOwner::kBrain` (harness disconnect
//    or battleground fallback to playerbots).

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_CORPSE_RUN_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_CORPSE_RUN_H_

#include <cstdint>
#include <optional>
#include <span>

#include "bot_manager/tick_owner.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

// Client-enforced corpse reclaim radius in AzerothCore (Corpse.h).
inline constexpr float kCorpseReclaimRadiusYd = 39.0f;

// One exterior AreaTrigger + AreaTriggerTeleport pair that enters an instance.
struct InstanceEntranceCandidate {
  uint32_t trigger_id = 0;
  uint32_t trigger_map_id = 0;
  wlb::snapshot::Vec3 trigger_pos;
  uint32_t target_map_id = 0;
  wlb::snapshot::Vec3 target_pos;
  // True when Player::IsInAreaTriggerRadius(atEntry, 0.f) holds.
  bool bot_inside_trigger = false;

  bool operator==(const InstanceEntranceCandidate&) const = default;
};

enum class CorpseRunAction : uint8_t {
  // No action: alive, not a ghost, in BG/arena, playerbots-led (disconnect
  // fallback), or no valid corpse/entrance on this map.
  kNone,
  // Walk the ghost toward `target_pos` on its current map (either the open-
  // world corpse or the exterior instance entrance AreaTrigger).
  kWalkToTarget,
  // Already within reclaim range on the corpse's map, waiting for the server's
  // reclaim delay timer to elapse.
  kWaitReclaimDelay,
  // Within reclaim range on the corpse's map and reclaim delay elapsed: send
  // CMSG_RECLAIM_CORPSE.
  kReclaimCorpse,
  // Inside the exterior instance entrance AreaTrigger matching the corpse's
  // map: send CMSG_AREATRIGGER with `trigger_id`.
  kEnterInstanceTrigger,
};

struct CorpseRunInput {
  TickOwner tick_owner = TickOwner::kPlayerbots;
  bool alive = true;
  bool is_ghost = false;
  bool in_battleground = false;
  bool in_arena = false;
  bool has_corpse = false;
  bool being_teleported = false;
  bool can_move = true;

  uint32_t bot_map_id = 0;
  wlb::snapshot::Vec3 bot_pos;

  uint32_t corpse_map_id = 0;
  wlb::snapshot::Vec3 corpse_pos;
  bool within_reclaim_range = false;
  bool reclaim_ready = false;

  // Best exterior instance entrance AreaTrigger on `bot_map_id` leading to
  // `corpse_map_id`, if any.
  std::optional<InstanceEntranceCandidate> instance_entrance;
};

struct CorpseRunDecision {
  CorpseRunAction action = CorpseRunAction::kNone;
  wlb::snapshot::Vec3 target_pos;
  uint32_t trigger_id = 0;

  bool operator==(const CorpseRunDecision&) const = default;
};

// Selects the best exterior instance entrance AreaTrigger on `bot_map_id` that
// teleports into `corpse_map_id`. Prefers any trigger the ghost is already
// inside; otherwise picks the trigger whose interior landing point is closest
// to `corpse_pos` (selecting the right wing in multi-wing instances such as
// Scarlet Monastery or Maraudon), breaking ties by closest exterior trigger to
// `bot_pos` and then lowest `trigger_id`.
std::optional<InstanceEntranceCandidate> SelectInstanceEntrance(
    uint32_t bot_map_id,
    wlb::snapshot::Vec3 bot_pos,
    uint32_t corpse_map_id,
    wlb::snapshot::Vec3 corpse_pos,
    std::span<const InstanceEntranceCandidate> candidates);

// Evaluates the next client-legal step for a dead/ghost bot.
CorpseRunDecision EvaluateCorpseRun(const CorpseRunInput& input);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_CORPSE_RUN_H_
