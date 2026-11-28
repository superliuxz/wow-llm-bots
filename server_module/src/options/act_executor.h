// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Executes standing-order and plan acts (`Command::Act`) on the world thread,
// resolving targets and relative places (or the party's minimap ping) against
// live world state with ground, LoS, and path checks, recomputing relative
// spots while a movement step runs, and walking a `use` into its spell's range
// and line of sight first.

#ifndef MOD_LLM_BRAIN_OPTIONS_ACT_EXECUTOR_H_
#define MOD_LLM_BRAIN_OPTIONS_ACT_EXECUTOR_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "base/time/time.h"
#include "options/act_geometry.h"
#include "snapshot/snapshot.h"

class Player;
class SpellInfo;
class Unit;

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
  uint32_t out_of_attempt = 0;
  base::TimeTicks hold_deadline;
  InterruptedActivity interrupted;
  base::TimeTicks last_repath_at;
  // For `use` with a cast time: waiting for the cast to finish.
  bool waiting_cast = false;
  uint32_t cast_spell_id = 0;
  uint32_t cast_mana_before = 0;
  bool cast_cd_before = false;
  base::TimeTicks cast_expected_end;
  uint32_t cast_miss_seq_before = 0;
  uint32_t cast_failure_seq_before = 0;
  std::string cast_target_name;
  // For `use <spell>` out of range or sight: walking in (2 yd inside the
  // spell's max range, with line of sight) before the cast, after entering
  // stealth first for a spell usable only in stealth. The target is a unit
  // (`approach_target_guid`) or the ping's spot (`approach_spot`).
  bool approach_cast = false;
  uint32_t approach_spell_id = 0;
  uint64_t approach_target_guid = 0;
  std::optional<wlb::snapshot::Vec3> approach_spot;
  float approach_closer_yd = 0.0f;
  uint32_t stealth_spell_id = 0;
  base::TimeTicks stealth_started_at;
  // Started by a chosen option (crowd control), not an act: its finish is
  // the option's (`done_command_id`), not an act's.
  bool for_option = false;
  // Started by a leader's party command ("los", "stack", "spread"): on
  // arriving, the bot holds there like after `to ping`
  // (BotManager::StartPingStay).
  bool hold_on_arrival = false;
  // Walks to a fixed world coordinate (`dest`) without re-resolving an anchor
  // unit (used by leader `spread`).
  bool fixed_spot = false;
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

// Starts a leader-command spread move around `center` for bot `slot_index` of
// `total_bots`, choosing the first navigable spot clear of unpulled hostiles.
// World thread only.
ActStartResult StartSpreadAct(Player* bot,
                              const AnchorGeometry& center,
                              size_t slot_index,
                              size_t total_bots,
                              base::TimeTicks now,
                              ActiveAct* out_active);

// Uses known `spell` on `target` the way `act: use` does (pure Spell.dbc
// facts): a spell usable only in stealth enters stealth first, and a target
// out of range or sight is walked to 2 yd inside the spell's range before the
// cast; a cast-time spell holds still while it casts. For a chosen option
// (crowd control). World thread only. When `done` is false and `error` is
// empty, populates `*out_active` (to step with StepActiveAct).
ActStartResult StartSpellUseOn(Player* bot,
                               uint32_t command_id,
                               const SpellInfo* spell,
                               Unit* target,
                               base::TimeTicks now,
                               ActiveAct* out_active);

// Advances an active multi-tick act (`to`, `away`, cast-time `use`). World
// thread only.
ActStepResult StepActiveAct(Player* bot,
                            base::TimeTicks now,
                            ActiveAct* active);

// Whether `active` is moving the bot out of the way right now (`out of` /
// `away` before it arrives, or `kite`): playerbots' reflex spells that would
// hold the bot still wait meanwhile (BotSession::moving_out_until_ms).
bool ActMovesOut(const ActiveAct& active);

// Whether `active` is a `use` of a spell usable only in stealth still walking
// in (StartSpellUse entered stealth, or found the bot in it): playerbots'
// `unstealth` and spell casts would break the stealth the act depends on, so
// they wait meanwhile (BotSession::act_stealth_until_ms).
bool ActHoldsStealth(const ActiveAct& active);

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
