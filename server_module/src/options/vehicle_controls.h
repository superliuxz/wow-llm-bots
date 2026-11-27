// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure vehicle snapshot construction, option offering, and action validation
// helpers (no AzerothCore headers, unit-tested directly).

#ifndef MOD_LLM_BRAIN_OPTIONS_VEHICLE_CONTROLS_H_
#define MOD_LLM_BRAIN_OPTIONS_VEHICLE_CONTROLS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

// Maximum vehicle action-bar spells exposed to the brain (WoW 3.3.5a
// MAX_SPELL_VEHICLE = 6).
inline constexpr size_t kMaxVehicleAbilities = 6;

// Pure server-side seat input mapped into `pb::VehiclePart_Seat`.
struct VehicleSeatInput {
  uint32_t index = 0;
  std::string occupant;
  bool can_use = false;
};

// Pure server-side vehicle ability input mapped into `pb::Spell`.
struct VehicleSpellInput {
  uint32_t spell_id = 0;
  std::string name;
  float cooldown_s = 0.0f;
  float cooldown_remaining_s = 0.0f;
  bool usable = true;
  bool ground_targeted = false;
  bool passive = false;
};

// Builds a `VehiclePart` from raw seat and spell inputs: sorts seats by index,
// skips passive/empty/duplicate-id spells, and caps abilities at
// `kMaxVehicleAbilities`.
wlb::snapshot::VehiclePart BuildVehiclePart(
    const std::vector<VehicleSeatInput>& seats,
    const std::vector<VehicleSpellInput>& spells);

// Pure predicate: whether `Leave vehicle` should be offered as an option.
bool CanOfferVehicleExit(bool alive,
                         bool in_vehicle,
                         bool can_exit_seat,
                         bool incapacitated);

// Pure validation before exiting a vehicle. Returns empty string if valid.
std::string ValidateVehicleExitState(bool in_vehicle,
                                     bool can_exit_seat,
                                     bool incapacitated);

struct VehicleSpellOption {
  uint32_t spell_id = 0;
  std::string label;
};

// Collects mechanically valid vehicle ability options from
// `snapshot.ext.vehicle` when the bot is alive, seated in a casting-capable
// vehicle seat, and not incapacitated. Skips abilities that are unusable or on
// cooldown.
std::vector<VehicleSpellOption> CollectVehicleSpellOptions(
    const wlb::snapshot::Snapshot& snapshot,
    bool can_cast_from_vehicle,
    bool incapacitated);

// Pure validation before casting a vehicle ability. Returns empty string if
// valid.
std::string ValidateVehicleSpellState(bool in_vehicle,
                                      bool vehicle_alive,
                                      bool incapacitated,
                                      bool has_vehicle_spell,
                                      bool on_cooldown);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_VEHICLE_CONTROLS_H_
