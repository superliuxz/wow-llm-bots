// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/vehicle_controls.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

namespace pb = wlb::snapshot;

pb::VehiclePart BuildVehiclePart(const std::vector<VehicleSeatInput>& seats,
                                 const std::vector<VehicleSpellInput>& spells) {
  pb::VehiclePart part;
  part.seats.reserve(seats.size());
  for (const VehicleSeatInput& seat : seats) {
    pb::VehiclePart::Seat out;
    out.index = seat.index;
    out.occupant = seat.occupant;
    out.can_use = seat.can_use;
    part.seats.push_back(std::move(out));
  }
  std::sort(part.seats.begin(), part.seats.end(),
            [](const pb::VehiclePart::Seat& a, const pb::VehiclePart::Seat& b) {
              return a.index < b.index;
            });

  std::vector<uint32_t> seen_ids;
  seen_ids.reserve(kMaxVehicleAbilities);
  for (const VehicleSpellInput& in : spells) {
    if (part.abilities.size() >= kMaxVehicleAbilities) {
      break;
    }
    if (in.spell_id == 0 || in.name.empty() || in.passive) {
      continue;
    }
    if (std::find(seen_ids.begin(), seen_ids.end(), in.spell_id) !=
        seen_ids.end()) {
      continue;
    }
    seen_ids.push_back(in.spell_id);
    pb::Spell sp;
    sp.id = in.spell_id;
    sp.name = in.name;
    sp.cooldown_s = in.cooldown_s > 0.0f ? in.cooldown_s : 0.0f;
    pb::AbilityState& ab = sp.ability.emplace();
    ab.known = true;
    ab.cooldown_remaining_s =
        in.cooldown_remaining_s > 0.0f ? in.cooldown_remaining_s : 0.0f;
    ab.usable = in.usable && ab.cooldown_remaining_s <= 0.0f;
    ab.ground_targeted = in.ground_targeted;
    part.abilities.push_back(std::move(sp));
  }
  return part;
}

bool CanOfferVehicleExit(bool alive,
                         bool in_vehicle,
                         bool can_exit_seat,
                         bool incapacitated) {
  return alive && in_vehicle && can_exit_seat && !incapacitated;
}

std::string ValidateVehicleExitState(bool in_vehicle,
                                     bool can_exit_seat,
                                     bool incapacitated) {
  if (!in_vehicle) {
    return "not in a vehicle";
  }
  if (incapacitated) {
    return "cannot leave vehicle right now";
  }
  if (!can_exit_seat) {
    return "cannot exit this vehicle seat";
  }
  return "";
}

std::vector<VehicleSpellOption> CollectVehicleSpellOptions(
    const pb::Snapshot& snapshot,
    bool can_cast_from_vehicle,
    bool incapacitated) {
  std::vector<VehicleSpellOption> out;
  const pb::Self& self = snapshot.base.self;
  if (self.state == pb::Self::DEAD_BODY || self.state == pb::Self::GHOST ||
      !can_cast_from_vehicle || incapacitated) {
    return out;
  }
  if (!snapshot.ext.vehicle || snapshot.ext.vehicle->dead ||
      !snapshot.ext.vehicle->vehicle) {
    return out;
  }
  for (const pb::Spell& sp : snapshot.ext.vehicle->vehicle->abilities) {
    if (out.size() >= kMaxVehicleAbilities) {
      break;
    }
    if (sp.id == 0 || sp.name.empty()) {
      continue;
    }
    if (sp.ability &&
        (!sp.ability->usable || sp.ability->cooldown_remaining_s > 0.0f)) {
      continue;
    }
    out.push_back({sp.id, "Use " + sp.name + " (vehicle)"});
  }
  return out;
}

std::string ValidateVehicleSpellState(bool in_vehicle,
                                      bool vehicle_alive,
                                      bool incapacitated,
                                      bool has_vehicle_spell,
                                      bool on_cooldown) {
  if (!in_vehicle) {
    return "not in a vehicle";
  }
  if (!vehicle_alive) {
    return "vehicle is destroyed";
  }
  if (incapacitated) {
    return "cannot use vehicle ability right now";
  }
  if (!has_vehicle_spell) {
    return "vehicle does not have that ability";
  }
  if (on_cooldown) {
    return "vehicle ability is on cooldown";
  }
  return "";
}

}  // namespace llm_brain
