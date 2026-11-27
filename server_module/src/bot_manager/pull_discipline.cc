// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/pull_discipline.h"

#include <cstdint>
#include <string_view>

namespace llm_brain {

void RecordRtiOverride(RtiOverrideState& state,
                       uint64_t target_guid,
                       bool in_combat) {
  state.active = true;
  state.target_guid = target_guid;
  state.saw_combat = in_combat;
}

bool ShouldResetRtiOverride(RtiOverrideState& state,
                            const RtiOverrideTickInput& input) {
  if (!state.active) {
    return false;
  }
  if (input.bot_in_combat || input.party_in_combat) {
    state.saw_combat = true;
    return false;
  }
  if (state.saw_combat || !input.target_alive_and_in_world) {
    state = {};
    return true;
  }
  return false;
}

bool ClearRtiOverride(RtiOverrideState& state) {
  if (!state.active) {
    return false;
  }
  state = {};
  return true;
}

bool IsPetTauntSpell(uint32_t spell_id) {
  switch (spell_id) {
    // Hunter pet Growl (Ranks 1-9, WotLK 3.3.5a spell_ranks.sql).
    case 2649:
    case 14916:
    case 14917:
    case 14918:
    case 14919:
    case 14920:
    case 14921:
    case 27047:
    case 61676:
    // Warlock Voidwalker Torment (Ranks 1-8).
    case 3716:
    case 7809:
    case 7810:
    case 7811:
    case 11774:
    case 11775:
    case 27270:
    case 47984:
    // Warlock Voidwalker Suffering (Ranks 1-8).
    case 17735:
    case 17750:
    case 17751:
    case 17752:
    case 27271:
    case 33701:
    case 47989:
    case 47990:
    // Warlock Felguard Anguish (Ranks 1-4).
    case 33698:
    case 33699:
    case 33700:
    case 47993:
      return true;
    default:
      return false;
  }
}

bool ShouldSuppressPetTaunt(bool in_group, bool has_non_pet_tank) {
  return in_group && has_non_pet_tank;
}

void RecordStopDps(StopDpsState& state, bool in_combat) {
  state.active = true;
  state.saw_combat = in_combat;
}

bool ShouldResetStopDps(StopDpsState& state,
                        bool bot_in_combat,
                        bool party_in_combat) {
  if (!state.active) {
    return false;
  }
  if (bot_in_combat || party_in_combat) {
    state.saw_combat = true;
    return false;
  }
  if (state.saw_combat) {
    state = {};
    return true;
  }
  return false;
}

bool ClearStopDps(StopDpsState& state) {
  if (!state.active) {
    return false;
  }
  state = {};
  return true;
}

bool IsStopDpsBlockedActionName(std::string_view action_name) {
  return action_name == "attack" || action_name.starts_with("attack ") ||
         action_name == "melee" || action_name == "shoot" ||
         action_name == "auto shot" || action_name == "wand" ||
         action_name == "reach melee" || action_name == "reach spell" ||
         action_name == "pull my target" || action_name == "pull rti target" ||
         action_name == "pet attack";
}

bool IsEligibleSkullMarkTarget(const SkullMarkCandidate& candidate) {
  return candidate.alive && candidate.is_npc && candidate.hostile &&
         candidate.has_guid && candidate.dist_yd >= 0.0f &&
         candidate.dist_yd <= kSkullMarkMaxDistYd &&
         candidate.in_combat_with_group && !candidate.is_crowd_controlled &&
         !candidate.has_raid_marker;
}

bool ShouldSuppressWarlockCcStrategy(bool is_warlock,
                                     bool in_group,
                                     bool in_dungeon,
                                     bool in_battleground) {
  if (!is_warlock || in_battleground) {
    return false;
  }
  return in_group || in_dungeon;
}

std::string_view WarlockCcStrategyDelta(bool is_warlock,
                                        bool in_group,
                                        bool in_dungeon,
                                        bool in_battleground) {
  if (!is_warlock || in_battleground) {
    return "";
  }
  return (in_group || in_dungeon) ? ",-cc" : ",+cc";
}

bool ShouldBlockFearCcAction(bool is_cc_spell_action,
                             std::string_view spell_name,
                             bool in_group,
                             bool in_dungeon,
                             bool in_battleground) {
  return is_cc_spell_action && spell_name == "fear" && !in_battleground &&
         (in_group || in_dungeon);
}

}  // namespace llm_brain
