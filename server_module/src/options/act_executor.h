// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Executes standing-order acts (`Command::Act`) on the world thread for the
// first verbs (`to`, `away`, `target`, `use`, `stop`, `wake`), resolving
// targets and relative places against live world state with ground, LoS, and
// path checks, and recomputing relative spots while a movement step runs.

#ifndef MOD_LLM_BRAIN_OPTIONS_ACT_EXECUTOR_H_
#define MOD_LLM_BRAIN_OPTIONS_ACT_EXECUTOR_H_

#include <cstdint>
#include <string>

#include "base/time/time.h"
#include "options/act_geometry.h"
#include "snapshot/snapshot.h"

class Player;

namespace llm_brain {

struct ActiveAct {
  enum class DodgePhase : uint8_t {
    kMovingOut = 0,
    kHolding = 1,
    kReturning = 2,
  };

  struct InterruptedActivity {
    bool active = false;
    uint32_t map_id = 0;
    uint32_t instance_id = 0;
    uint64_t target_guid = 0;
    wlb::snapshot::Vec3 prev_pos;
    float prev_target_dist_yd = 0.0f;
    bool was_attacking = false;
  };

  uint32_t command_id = 0;
  uint32_t map_id = 0;
  uint32_t instance_id = 0;
  wlb::snapshot::Command_Act act;
  // For `to` / `away` / `out of` / `back`: current destination, last anchor
  // geometry, and last path recompute timestamp.
  wlb::snapshot::Vec3 dest;
  AnchorGeometry last_anchor;
  float away_target_edge_yd = 0.0f;
  float hazard_radius_yd = 0.0f;
  bool hazard_is_ground_area = false;
  uint64_t hazard_caster_guid = 0;
  uint32_t hazard_spell_id = 0;
  DodgePhase dodge_phase = DodgePhase::kMovingOut;
  base::TimeTicks hold_deadline;
  InterruptedActivity interrupted;
  base::TimeTicks last_repath_at;
  // For `use` with a cast time: waiting for the cast to finish.
  bool waiting_cast = false;
  uint32_t cast_spell_id = 0;
  uint32_t cast_mana_before = 0;
  bool cast_cd_before = false;
  base::TimeTicks cast_expected_end;
};

struct ActStartResult {
  // True when the act finished immediately (no multi-tick tracking needed).
  bool done = false;
  // Non-empty when the act could not be started (reported as an ErrorEvent).
  std::string error;
};

struct ActStepResult {
  // True when the active act completed on this tick.
  bool done = false;
  // Non-empty when the active act failed/was interrupted (reported as an
  // ErrorEvent).
  std::string error;
};

// Starts carrying out `act` for `bot`. World thread only.
// When `done` is false and `error` is empty, populates `*out_active`.
ActStartResult StartAct(Player* bot,
                        uint32_t command_id,
                        const wlb::snapshot::Command_Act& act,
                        base::TimeTicks now,
                        ActiveAct* out_active);

// Advances an active multi-tick act (`to`, `away`, cast-time `use`). World
// thread only.
ActStepResult StepActiveAct(Player* bot,
                            base::TimeTicks now,
                            ActiveAct* active);

// Cancels an active act on `bot` (releasing `MOVEMENT_FORCED` and stopping
// movement if it was a movement act). World thread only.
void CancelActiveAct(Player* bot, ActiveAct* active);

// Clears avoided-target memory for `bot_guid` (all targets on combat exit /
// disconnect, or one specific `target_guid` on explicit Attack/TARGET).
void ClearAvoidedTargetsForBot(uint64_t bot_guid);
void ClearAvoidedTargetForBot(uint64_t bot_guid, uint64_t target_guid);

// Clears saved pre-dodge `back` position for `bot_guid` (e.g. on map/instance
// change or disconnect so old-map coordinates are never reused, A-L1).
void ClearLastInterruptedForBot(uint64_t bot_guid);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_ACT_EXECUTOR_H_
