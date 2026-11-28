// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/upkeep_helpers.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "options/riding_mounts.h"

namespace llm_brain {

StackBuyPlan ComputeStackBuyPlan(const StackBuyRequest& req) {
  const uint32_t buy_count = std::max<uint32_t>(1u, req.buy_count);
  const uint32_t max_stack = std::max<uint32_t>(buy_count, req.max_stack);
  if (req.current_count >= req.target_stock) {
    return {};
  }
  const uint32_t deficit = req.target_stock - req.current_count;
  const uint32_t desired_items = std::min<uint32_t>(deficit, max_stack);
  uint32_t units = (desired_items + buy_count - 1u) / buy_count;
  const uint32_t max_stack_units =
      std::max<uint32_t>(1u, max_stack / buy_count);
  units = std::min<uint32_t>(units, max_stack_units);
  if (units == 0) {
    return {};
  }

  if (req.unit_price_copper > 0) {
    const uint32_t affordable_units =
        req.bot_money_copper / req.unit_price_copper;
    if (affordable_units == 0) {
      return {};
    }
    units = std::min<uint32_t>(units, affordable_units);
  }
  if (req.vendor_stock > 0) {
    const uint32_t stock_units = req.vendor_stock / buy_count;
    if (stock_units == 0) {
      return {};
    }
    units = std::min<uint32_t>(units, stock_units);
  }
  if (req.max_storable_items > 0) {
    const uint32_t bag_units = req.max_storable_items / buy_count;
    if (bag_units == 0) {
      return {};
    }
    units = std::min<uint32_t>(units, bag_units);
  }

  units = std::min<uint32_t>(units, 255u);
  if (units == 0) {
    return {};
  }
  const uint64_t total_copper = static_cast<uint64_t>(units) *
                                static_cast<uint64_t>(req.unit_price_copper);
  return StackBuyPlan{
      .purchase_units = static_cast<uint8_t>(units),
      .total_items = units * buy_count,
      .total_copper = static_cast<uint32_t>(std::min<uint64_t>(
          total_copper, std::numeric_limits<uint32_t>::max())),
  };
}

std::string FormatStackBuyLabel(std::string_view item_name,
                                std::string_view npc_name,
                                const StackBuyPlan& plan) {
  std::string label = "Buy ";
  label.append(item_name);
  if (plan.total_items > 1) {
    label += " x" + std::to_string(plan.total_items);
  }
  if (plan.total_copper > 0) {
    label += " (" + FormatCopperCost(plan.total_copper) + ")";
  }
  label += " from ";
  label.append(npc_name);
  return label;
}

bool NeedsFoodRestock(uint32_t food_count) {
  return food_count < kFoodRestockThreshold;
}

bool NeedsDrinkRestock(uint32_t drink_count, bool uses_mana) {
  return uses_mana && drink_count < kDrinkRestockThreshold;
}

std::vector<ClassReagentSpec> ClassReagentSpecsFor(uint8_t class_id,
                                                   uint32_t level) {
  std::vector<ClassReagentSpec> specs;
  switch (class_id) {
    case kClassDruid:
      // Rebirth seeds.
      if (level >= 20 && level < 30) {
        specs.push_back({17034, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 30 && level < 40) {
        specs.push_back({17035, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 40 && level < 50) {
        specs.push_back({17036, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 50 && level < 60) {
        specs.push_back({17037, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 60 && level < 69) {
        specs.push_back({17038, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 69 && level < 79) {
        specs.push_back({22147, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 79) {
        specs.push_back({44614, kReagentRestockThreshold, kReagentTargetStock});
      }
      // Gift of the Wild reagents.
      if (level >= 50 && level < 60) {
        specs.push_back({17021, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 60 && level < 70) {
        specs.push_back({17026, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 70 && level < 80) {
        specs.push_back({22148, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 80) {
        specs.push_back({44605, kReagentRestockThreshold, kReagentTargetStock});
      }
      break;
    case kClassShaman:
      if (level >= 30) {
        specs.push_back({17030, kReagentRestockThreshold, kReagentTargetStock});
      }
      break;
    case kClassMage:
      if (level >= 20) {
        specs.push_back({17031, kReagentRestockThreshold, kReagentTargetStock});
      }
      if (level >= 40) {
        specs.push_back({17032, kReagentRestockThreshold, kReagentTargetStock});
      }
      if (level >= 56) {
        specs.push_back({17020, kReagentRestockThreshold, kReagentTargetStock});
      }
      break;
    case kClassPaladin:
      if (level >= 30) {
        specs.push_back({17033, kReagentRestockThreshold, kReagentTargetStock});
      }
      if (level >= 52) {
        specs.push_back(
            {21177, kSymbolOfKingsRestockThreshold, kSymbolOfKingsTargetStock});
      }
      break;
    case kClassPriest:
      if (level >= 48 && level < 56) {
        specs.push_back({17028, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 56 && level < 77) {
        specs.push_back({17029, kReagentRestockThreshold, kReagentTargetStock});
      } else if (level >= 77) {
        specs.push_back({44615, kReagentRestockThreshold, kReagentTargetStock});
      }
      break;
    case kClassDeathKnight:
      if (level >= 56) {
        specs.push_back({37201, kReagentRestockThreshold, kReagentTargetStock});
      }
      break;
    default:
      break;
  }
  return specs;
}

RoguePoisonFamily ClassifyRoguePoison(std::string_view item_name) {
  if (item_name.starts_with("Instant Poison")) {
    return RoguePoisonFamily::kInstant;
  }
  if (item_name.starts_with("Deadly Poison")) {
    return RoguePoisonFamily::kDeadly;
  }
  if (item_name.starts_with("Wound Poison")) {
    return RoguePoisonFamily::kWound;
  }
  if (item_name.starts_with("Crippling Poison")) {
    return RoguePoisonFamily::kCrippling;
  }
  if (item_name.starts_with("Mind-Numbing Poison")) {
    return RoguePoisonFamily::kMindNumbing;
  }
  if (item_name.starts_with("Anesthetic Poison")) {
    return RoguePoisonFamily::kAnesthetic;
  }
  return RoguePoisonFamily::kNone;
}

uint32_t PickHunterAmmoToEquip(
    bool is_hunter,
    bool alive,
    uint32_t current_ammo_id,
    uint32_t current_ammo_count,
    uint32_t current_ammo_ilvl,
    bool current_ammo_matches_ranged,
    const std::vector<HunterAmmoCandidate>& bag_ammo) {
  if (!is_hunter || !alive) {
    return 0;
  }
  const HunterAmmoCandidate* best = nullptr;
  for (const HunterAmmoCandidate& c : bag_ammo) {
    if (c.item_id == 0 || c.count == 0 || !c.matches_ranged_weapon ||
        !c.usable) {
      continue;
    }
    if (!best || c.item_level > best->item_level ||
        (c.item_level == best->item_level && c.count > best->count) ||
        (c.item_level == best->item_level && c.count == best->count &&
         c.item_id < best->item_id)) {
      best = &c;
    }
  }
  if (!best) {
    return 0;
  }
  const bool current_valid = current_ammo_id != 0 && current_ammo_count > 0 &&
                             current_ammo_matches_ranged;
  if (current_valid && best->item_level <= current_ammo_ilvl) {
    return 0;
  }
  return best->item_id;
}

std::string ValidateLeaderErrandCommand(bool alive,
                                        bool in_combat,
                                        bool party_in_combat,
                                        bool in_battleground) {
  if (!alive) {
    return "you are dead";
  }
  if (in_combat || party_in_combat) {
    return "in combat";
  }
  if (in_battleground) {
    return "in a battleground";
  }
  return {};
}

}  // namespace llm_brain
