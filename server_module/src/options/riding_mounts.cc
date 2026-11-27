// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/riding_mounts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llm_brain {

std::string_view RidingSpellName(uint32_t spell_id) {
  switch (spell_id) {
    case kSpellApprenticeRiding:
      return "Apprentice Riding";
    case kSpellJourneymanRiding:
      return "Journeyman Riding";
    case kSpellExpertRiding:
      return "Expert Riding";
    case kSpellArtisanRiding:
      return "Artisan Riding";
    case kSpellColdWeatherFlying:
      return "Cold Weather Flying";
    default:
      return "";
  }
}

std::string FormatCopperCost(uint32_t copper) {
  if (copper == 0) {
    return "free";
  }
  const uint32_t gold = copper / 10000;
  const uint32_t silver = (copper % 10000) / 100;
  const uint32_t rem_copper = copper % 100;
  std::string out;
  if (gold > 0) {
    out += std::to_string(gold) + "g";
  }
  if (silver > 0) {
    if (!out.empty()) {
      out += " ";
    }
    out += std::to_string(silver) + "s";
  }
  if (rem_copper > 0) {
    if (!out.empty()) {
      out += " ";
    }
    out += std::to_string(rem_copper) + "c";
  }
  return out;
}

uint32_t DiscountedCopperCost(uint32_t base_copper, float discount) {
  if (base_copper == 0 || !std::isfinite(discount) || discount <= 0.0f) {
    return 0;
  }
  if (discount >= 1.0f) {
    return base_copper;
  }
  return static_cast<uint32_t>(static_cast<float>(base_copper) * discount);
}

std::vector<RidingTrainingOption> SelectAffordableRidingTraining(
    const std::vector<RidingTrainingCandidate>& candidates,
    uint32_t bot_money_copper,
    std::string_view npc_name) {
  std::vector<const RidingTrainingCandidate*> eligible;
  for (const RidingTrainingCandidate& c : candidates) {
    if (c.spell_id == 0 || !c.can_teach || c.cost_copper > bot_money_copper) {
      continue;
    }
    eligible.push_back(&c);
  }
  std::sort(
      eligible.begin(), eligible.end(),
      [](const RidingTrainingCandidate* a, const RidingTrainingCandidate* b) {
        if (a->req_level != b->req_level) {
          return a->req_level < b->req_level;
        }
        return a->spell_id < b->spell_id;
      });

  std::vector<RidingTrainingOption> out;
  out.reserve(eligible.size());
  for (const RidingTrainingCandidate* c : eligible) {
    std::string name =
        !c->name.empty() ? c->name : std::string(RidingSpellName(c->spell_id));
    if (name.empty()) {
      name = "Riding";
    }
    std::string label = "Train " + name + " (" +
                        FormatCopperCost(c->cost_copper) + ") at " +
                        std::string(npc_name);
    out.push_back({c->spell_id, c->cost_copper, std::move(label)});
  }
  return out;
}

std::string ValidateRidingTrain(bool in_combat,
                                bool valid_trainer,
                                bool spell_found,
                                bool can_teach,
                                uint32_t cost_copper,
                                uint32_t bot_money_copper) {
  if (in_combat) {
    return "you are in combat";
  }
  if (!valid_trainer) {
    return "cannot train riding here";
  }
  if (!spell_found) {
    return "riding skill not taught here";
  }
  if (!can_teach) {
    return "cannot learn that riding skill";
  }
  if (cost_copper > bot_money_copper) {
    return "not enough money";
  }
  return {};
}

std::vector<MountPurchaseOption> SelectMountPurchases(
    const std::vector<MountVendorCandidate>& candidates,
    uint32_t bot_money_copper,
    uint32_t best_known_ground_speed_pct,
    uint32_t best_known_flight_speed_pct,
    std::string_view npc_name,
    size_t max_picks) {
  if (max_picks == 0) {
    return {};
  }
  std::vector<const MountVendorCandidate*> eligible;
  for (const MountVendorCandidate& c : candidates) {
    if (c.item_id == 0 || c.name.empty() || !c.meets_requirements ||
        c.already_known || c.in_bags || c.price_copper > bot_money_copper) {
      continue;
    }
    const uint32_t known_best =
        c.is_flying ? best_known_flight_speed_pct : best_known_ground_speed_pct;
    // Offer only mounts that provide a first mount in that category or a speed
    // upgrade over what the bot already knows.
    if (known_best > 0 && c.speed_pct <= known_best) {
      continue;
    }
    eligible.push_back(&c);
  }
  // Prefer higher speed first, then non-flying before flying at equal speed,
  // then cheaper price, then deterministic lowest item_id.
  std::sort(eligible.begin(), eligible.end(),
            [](const MountVendorCandidate* a, const MountVendorCandidate* b) {
              if (a->is_flying != b->is_flying) {
                return !a->is_flying;
              }
              if (a->speed_pct != b->speed_pct) {
                return a->speed_pct > b->speed_pct;
              }
              if (a->price_copper != b->price_copper) {
                return a->price_copper < b->price_copper;
              }
              return a->item_id < b->item_id;
            });

  std::vector<MountPurchaseOption> out;
  for (const MountVendorCandidate* c : eligible) {
    if (out.size() >= max_picks) {
      break;
    }
    bool dup_tier = false;
    for (const MountPurchaseOption& picked : out) {
      if (picked.is_flying == c->is_flying &&
          picked.speed_pct == c->speed_pct) {
        dup_tier = true;
        break;
      }
    }
    if (dup_tier) {
      continue;
    }
    std::string detail;
    if (c->speed_pct > 0) {
      detail = std::to_string(c->speed_pct) + "% ";
    }
    detail += c->is_flying ? "flying mount, " : "mount, ";
    detail += FormatCopperCost(c->price_copper);
    out.push_back(
        {c->item_id, c->price_copper, c->speed_pct, c->is_flying,
         "Buy " + c->name + " (" + detail + ") from " + std::string(npc_name)});
  }
  return out;
}

bool CanOfferMount(const MountState& state) {
  return state.alive && !state.in_combat && !state.mounted &&
         !state.in_flight && !state.in_vehicle && !state.in_dungeon &&
         state.outdoors && !state.swimming;
}

std::string ValidateMountState(const MountState& state, bool has_spell) {
  if (!state.alive) {
    return "you are dead";
  }
  if (state.in_combat) {
    return "cannot mount in combat";
  }
  if (state.mounted) {
    return "already mounted";
  }
  if (state.in_flight) {
    return "cannot mount while on a flight path";
  }
  if (state.in_vehicle) {
    return "cannot mount while in a vehicle";
  }
  if (state.in_dungeon) {
    return "cannot mount inside a dungeon";
  }
  if (!state.outdoors) {
    return "cannot mount indoors";
  }
  if (state.swimming) {
    return "cannot mount while swimming";
  }
  if (!has_spell) {
    return "you don't know that mount";
  }
  return {};
}

std::optional<KnownMountSpell> PickBestMount(
    const std::vector<KnownMountSpell>& mounts,
    bool can_fly) {
  const KnownMountSpell* best_ground = nullptr;
  const KnownMountSpell* best_flying = nullptr;
  for (const KnownMountSpell& m : mounts) {
    if (m.spell_id == 0 || m.name.empty()) {
      continue;
    }
    if (!m.is_flying) {
      if (!best_ground || m.ground_speed_pct > best_ground->ground_speed_pct ||
          (m.ground_speed_pct == best_ground->ground_speed_pct &&
           m.spell_id < best_ground->spell_id)) {
        best_ground = &m;
      }
    } else if (can_fly) {
      if (!best_flying || m.flight_speed_pct > best_flying->flight_speed_pct ||
          (m.flight_speed_pct == best_flying->flight_speed_pct &&
           m.spell_id < best_flying->spell_id)) {
        best_flying = &m;
      }
    }
  }
  if (best_ground) {
    return *best_ground;
  }
  if (best_flying) {
    return *best_flying;
  }
  return std::nullopt;
}

bool CanOfferDismount(bool alive,
                      bool mounted,
                      bool in_flight,
                      bool incapacitated) {
  return alive && mounted && !in_flight && !incapacitated;
}

std::string ValidateDismountState(bool mounted,
                                  bool in_flight,
                                  bool incapacitated) {
  if (in_flight) {
    return "cannot dismount while on a flight path";
  }
  if (incapacitated) {
    return "cannot dismount right now";
  }
  if (!mounted) {
    return "not mounted";
  }
  return {};
}

}  // namespace llm_brain
