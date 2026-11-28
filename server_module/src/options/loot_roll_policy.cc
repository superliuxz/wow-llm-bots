// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/loot_roll_policy.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llm_brain {
namespace {

// WotLK 3.3.5a Classes enum values (SharedDefines.h).
constexpr uint8_t kClassWarrior = 1;
constexpr uint8_t kClassPaladin = 2;
constexpr uint8_t kClassHunter = 3;
constexpr uint8_t kClassRogue = 4;
constexpr uint8_t kClassPriest = 5;
constexpr uint8_t kClassDeathKnight = 6;
constexpr uint8_t kClassShaman = 7;
constexpr uint8_t kClassMage = 8;
constexpr uint8_t kClassWarlock = 9;
constexpr uint8_t kClassDruid = 11;

constexpr uint8_t kWarriorProtectionTab = 2;
constexpr uint8_t kPaladinHolyTab = 0;
constexpr uint8_t kPaladinProtectionTab = 1;
constexpr uint8_t kShamanEnhancementTab = 1;
constexpr uint8_t kDruidFeralTab = 1;

}  // namespace

LootStatProfile LootStatProfileForSpec(uint8_t class_id,
                                       uint8_t spec_tab,
                                       bool has_talents,
                                       bool is_tank,
                                       bool is_healer) {
  switch (class_id) {
    case kClassWarrior:
      if (is_tank || (has_talents && spec_tab == kWarriorProtectionTab)) {
        return LootStatProfile::kTankStrength;
      }
      return LootStatProfile::kMeleeStrength;
    case kClassPaladin:
      if (is_healer || (has_talents && spec_tab == kPaladinHolyTab)) {
        return LootStatProfile::kSpellCaster;
      }
      if (is_tank || (has_talents && spec_tab == kPaladinProtectionTab)) {
        return LootStatProfile::kTankStrength;
      }
      return LootStatProfile::kMeleeStrength;
    case kClassHunter:
    case kClassRogue:
      return LootStatProfile::kPhysicalAgility;
    case kClassPriest:
    case kClassMage:
    case kClassWarlock:
      return LootStatProfile::kSpellCaster;
    case kClassDeathKnight:
      return is_tank ? LootStatProfile::kTankStrength
                     : LootStatProfile::kMeleeStrength;
    case kClassShaman:
      if (!is_healer && has_talents && spec_tab == kShamanEnhancementTab) {
        return LootStatProfile::kPhysicalAgility;
      }
      return LootStatProfile::kSpellCaster;
    case kClassDruid:
      if (is_tank) {
        return LootStatProfile::kTankAgility;
      }
      if (!is_healer && has_talents && spec_tab == kDruidFeralTab) {
        return LootStatProfile::kPhysicalAgility;
      }
      return LootStatProfile::kSpellCaster;
    default:
      return LootStatProfile::kMeleeStrength;
  }
}

bool IsShieldDisallowedForProfile(LootStatProfile profile) {
  return profile == LootStatProfile::kMeleeStrength ||
         profile == LootStatProfile::kPhysicalAgility;
}

bool IsWeaponDisallowedForSpec(LootStatProfile profile,
                               uint8_t class_id,
                               uint32_t weapon_subclass) {
  switch (weapon_subclass) {
    case kWeaponSubclassBow:
    case kWeaponSubclassGun:
    case kWeaponSubclassCrossbow:
      // Primary weapons for Hunters; Warriors/Rogues only use them as minor
      // stat sticks and must not Need them over Hunters.
      return class_id != kClassHunter;
    case kWeaponSubclassThrown:
      return class_id == kClassHunter ||
             profile == LootStatProfile::kSpellCaster;
    case kWeaponSubclassWand:
      return profile != LootStatProfile::kSpellCaster;
    default:
      return false;
  }
}

ItemStatFlags StatFlagsFromGearInfo(const GearItemInfo& item) {
  ItemStatFlags f;
  f.strength = item.strength > 0;
  f.agility = item.agility > 0;
  f.intellect = item.intellect > 0;
  f.spirit = item.spirit > 0;
  f.stamina = item.stamina > 0;
  f.spell_power = item.spell_power > 0;
  f.attack_power = item.attack_power > 0;
  f.ranged_attack_power = item.ranged_attack_power > 0;
  f.mp5 = item.mp5 > 0;
  f.hit = item.hit > 0;
  f.crit = item.crit > 0;
  f.haste = item.haste > 0;
  f.tank_avoidance =
      item.defense > 0 || item.dodge > 0 || item.parry > 0 || item.block > 0;
  f.melee_dps = item.expertise > 0 || item.armor_pen > 0;
  return f;
}

StatFit EvaluateStatFit(LootStatProfile profile,
                        uint8_t class_id,
                        const ItemStatFlags& stats) {
  const bool secondary_dps = stats.hit || stats.crit || stats.haste;

  switch (profile) {
    case LootStatProfile::kSpellCaster: {
      const bool bad = stats.strength || stats.agility || stats.attack_power ||
                       stats.ranged_attack_power || stats.tank_avoidance ||
                       stats.melee_dps;
      if (bad) {
        return StatFit::kMismatch;
      }
      if (stats.intellect || stats.spell_power || stats.spirit || stats.mp5 ||
          secondary_dps) {
        return StatFit::kMatch;
      }
      return StatFit::kNeutral;
    }
    case LootStatProfile::kMeleeStrength: {
      const bool hard_bad = stats.intellect || stats.spirit ||
                            stats.spell_power || stats.mp5 ||
                            stats.ranged_attack_power || stats.tank_avoidance;
      // Agility without Strength (e.g. Agi + AP Hunter/Rogue gear) is off-spec
      // for Strength melee; Attack Power does NOT override Agility.
      const bool wrong_primary = stats.agility && !stats.strength;
      if (hard_bad || wrong_primary) {
        return StatFit::kMismatch;
      }
      if (stats.strength || stats.melee_dps || secondary_dps) {
        return StatFit::kMatch;
      }
      return StatFit::kNeutral;
    }
    case LootStatProfile::kTankStrength: {
      const bool hard_bad = stats.intellect || stats.spirit ||
                            stats.spell_power || stats.mp5 ||
                            stats.ranged_attack_power;
      const bool wrong_primary =
          stats.agility && !stats.strength && !stats.tank_avoidance;
      if (hard_bad || wrong_primary) {
        return StatFit::kMismatch;
      }
      if (stats.strength || stats.tank_avoidance || stats.melee_dps ||
          secondary_dps) {
        return StatFit::kMatch;
      }
      return StatFit::kNeutral;
    }
    case LootStatProfile::kPhysicalAgility: {
      const bool is_feral = class_id == kClassDruid;
      const bool is_hunter = class_id == kClassHunter;
      const bool wants_int =
          class_id == kClassHunter || class_id == kClassShaman;
      const bool hard_bad = stats.spell_power || stats.spirit || stats.mp5 ||
                            stats.tank_avoidance ||
                            (!wants_int && stats.intellect) ||
                            (!is_hunter && stats.ranged_attack_power);
      // Strength without Agility (e.g. Str + AP plate/ring) is off-spec for
      // non-Feral agility classes; Attack Power does NOT override Strength.
      const bool wrong_primary = !is_feral && stats.strength && !stats.agility;
      if (hard_bad || wrong_primary) {
        return StatFit::kMismatch;
      }
      if (stats.agility || (is_feral && stats.strength) ||
          (is_hunter && stats.ranged_attack_power) || stats.melee_dps ||
          secondary_dps) {
        return StatFit::kMatch;
      }
      return StatFit::kNeutral;
    }
    case LootStatProfile::kTankAgility: {
      const bool hard_bad = stats.intellect || stats.spirit ||
                            stats.spell_power || stats.mp5 ||
                            stats.ranged_attack_power;
      if (hard_bad) {
        return StatFit::kMismatch;
      }
      if (stats.agility || stats.strength || stats.tank_avoidance ||
          stats.melee_dps || secondary_dps) {
        return StatFit::kMatch;
      }
      return StatFit::kNeutral;
    }
  }
  return StatFit::kNeutral;
}

float ScoreGearItem(LootStatProfile profile,
                    uint8_t class_id,
                    uint32_t best_armor_subclass,
                    const GearItemInfo& item) {
  const auto pos = [](int32_t v) -> float {
    return v > 0 ? static_cast<float>(v) : 0.0f;
  };
  const bool disallowed_shield =
      item.is_shield && IsShieldDisallowedForProfile(profile);
  const bool disallowed_weapon =
      item.is_weapon &&
      IsWeaponDisallowedForSpec(profile, class_id, item.weapon_subclass);
  const StatFit fit =
      EvaluateStatFit(profile, class_id, StatFlagsFromGearInfo(item));

  float score = static_cast<float>(item.item_level) * 0.25f;
  switch (profile) {
    case LootStatProfile::kSpellCaster:
      score += pos(item.spell_power) * 2.2f + pos(item.intellect) * 1.5f +
               pos(item.mp5) * 1.4f + pos(item.hit) * 1.2f +
               pos(item.crit) * 1.2f + pos(item.haste) * 1.2f +
               pos(item.spirit) * 0.9f + pos(item.stamina) * 0.35f +
               static_cast<float>(item.armor) * 0.005f;
      break;
    case LootStatProfile::kMeleeStrength:
      score += pos(item.strength) * 2.2f + pos(item.expertise) * 1.6f +
               pos(item.hit) * 1.5f + pos(item.armor_pen) * 1.4f +
               pos(item.crit) * 1.3f + pos(item.haste) * 1.1f +
               pos(item.attack_power) * 0.9f + pos(item.stamina) * 0.4f +
               static_cast<float>(item.armor) * 0.01f;
      if (item.is_weapon && !disallowed_weapon) {
        score += item.weapon_dps * 4.0f;
      }
      break;
    case LootStatProfile::kTankStrength:
      score += pos(item.defense) * 2.0f + pos(item.stamina) * 1.8f +
               pos(item.dodge) * 1.6f + pos(item.parry) * 1.6f +
               pos(item.strength) * 1.3f + pos(item.expertise) * 1.2f +
               pos(item.hit) * 1.1f + pos(item.block) * 1.0f +
               pos(item.crit) * 0.6f + pos(item.haste) * 0.5f +
               pos(item.armor_pen) * 0.5f + pos(item.attack_power) * 0.4f +
               static_cast<float>(item.armor) * 0.04f;
      if (item.is_weapon && !disallowed_weapon) {
        score += item.weapon_dps * 2.5f;
      }
      break;
    case LootStatProfile::kPhysicalAgility: {
      const bool is_feral = class_id == kClassDruid;
      const bool is_hunter = class_id == kClassHunter;
      const bool wants_int =
          class_id == kClassHunter || class_id == kClassShaman;
      score += pos(item.agility) * 2.2f + pos(item.hit) * 1.5f +
               pos(item.crit) * 1.3f + pos(item.armor_pen) * 1.3f +
               pos(item.haste) * 1.1f + pos(item.attack_power) * 0.9f +
               pos(item.stamina) * 0.4f +
               (is_hunter ? pos(item.ranged_attack_power) * 1.0f : 0.0f) +
               (is_feral ? pos(item.strength) * 1.8f : 0.0f) +
               (!is_hunter ? pos(item.expertise) * 1.4f : 0.0f) +
               (wants_int ? pos(item.intellect) * 0.5f : 0.0f) +
               static_cast<float>(item.armor) * 0.01f;
      if (item.is_weapon && !disallowed_weapon) {
        score += item.weapon_dps * 4.0f;
      }
      break;
    }
    case LootStatProfile::kTankAgility:
      score += pos(item.agility) * 2.0f + pos(item.stamina) * 1.9f +
               pos(item.dodge) * 1.7f + pos(item.defense) * 1.5f +
               pos(item.strength) * 1.2f + pos(item.expertise) * 1.2f +
               pos(item.hit) * 1.1f + pos(item.crit) * 0.7f +
               pos(item.haste) * 0.6f + pos(item.armor_pen) * 0.5f +
               pos(item.attack_power) * 0.5f +
               static_cast<float>(item.armor) * 0.05f;
      if (item.is_weapon && !disallowed_weapon) {
        score += item.weapon_dps * 2.5f;
      }
      break;
  }

  // Penalize body armor below the bot's class armor proficiency (e.g. Warrior
  // wearing cloth/leather/mail instead of plate).
  if (item.is_armor && !item.is_cloak && !item.is_shield &&
      item.armor_subclass >= kArmorSubclassCloth &&
      item.armor_subclass <= kArmorSubclassPlate &&
      best_armor_subclass >= kArmorSubclassCloth &&
      best_armor_subclass <= kArmorSubclassPlate &&
      item.armor_subclass < best_armor_subclass) {
    const uint32_t tier_gap = best_armor_subclass - item.armor_subclass;
    const float armor_mult =
        tier_gap == 1 ? 0.50f : (tier_gap == 2 ? 0.35f : 0.25f);
    score *= armor_mult;
  }

  // Strongly penalize off-spec stats, disallowed shields, or off-role weapons.
  if (fit == StatFit::kMismatch || disallowed_shield || disallowed_weapon) {
    score *= 0.15f;
  }
  return score;
}

bool ShouldDeferRollForHuman(bool human_pending_vote,
                             uint64_t roll_elapsed_ms,
                             uint64_t human_defer_ms) {
  return human_pending_vote && roll_elapsed_ms < human_defer_ms;
}

std::optional<LootRollChoice> DecidePolicyRollVote(
    const LootRollPolicyInput& in) {
  if (ShouldDeferRollForHuman(in.human_pending_vote, in.roll_elapsed_ms,
                              in.human_defer_ms)) {
    return std::nullopt;
  }

  const bool want_need =
      in.usable && !in.bind_on_equip && in.armor_type_matches &&
      !in.shield_disallowed_for_spec && !in.weapon_disallowed_for_spec &&
      in.has_upgrade_slot && in.stat_fit != StatFit::kMismatch &&
      !in.human_already_needed;
  if (want_need && (in.roll_vote_mask & kRollMaskNeed)) {
    return LootRollChoice::kNeed;
  }
  if (in.roll_vote_mask & kRollMaskDisenchant) {
    return LootRollChoice::kDisenchant;
  }
  if (in.roll_vote_mask & kRollMaskGreed) {
    return LootRollChoice::kGreed;
  }
  return LootRollChoice::kPass;
}

std::vector<TradeOfferOption> SelectTradeItemOffers(
    bool has_free_trade_slot,
    const std::vector<TradeBagItem>& bag_items) {
  std::vector<TradeOfferOption> out;
  if (!has_free_trade_slot) {
    return out;
  }
  std::vector<const TradeBagItem*> unique;
  std::vector<uint32_t> seen_ids;
  unique.reserve(bag_items.size());
  seen_ids.reserve(bag_items.size());
  for (const TradeBagItem& b : bag_items) {
    if (b.item_id == 0 || b.name.empty() || b.already_in_trade ||
        !b.can_be_traded || b.bound_not_with_trader) {
      continue;
    }
    if (std::find(seen_ids.begin(), seen_ids.end(), b.item_id) !=
        seen_ids.end()) {
      continue;
    }
    seen_ids.push_back(b.item_id);
    unique.push_back(&b);
  }
  // Prioritize non-grey items ahead of grey junk while preserving bag order
  // within each tier; expose every distinct tradeable kind (no 8-item cap).
  std::stable_partition(unique.begin(), unique.end(),
                        [](const TradeBagItem* b) { return b->quality > 0; });
  out.reserve(unique.size());
  for (const TradeBagItem* b : unique) {
    TradeOfferOption opt;
    opt.guid = "trade:put:" + std::to_string(b->bag) + ":" +
               std::to_string(b->slot) + ":" + std::to_string(b->item_id);
    opt.label =
        "Put " + b->name +
        (b->count > 1 ? " x" + std::to_string(b->count) : std::string()) +
        " in the trade";
    out.push_back(std::move(opt));
  }
  return out;
}

const char* LootRollChoiceVerb(LootRollChoice choice) {
  switch (choice) {
    case LootRollChoice::kNeed:
      return "Need";
    case LootRollChoice::kGreed:
      return "Greed";
    case LootRollChoice::kDisenchant:
      return "Disenchant";
    case LootRollChoice::kPass:
      return "Pass";
  }
  return "Pass";
}

void PrunePendingRolls(std::vector<PendingRollSeen>* seen,
                       const std::vector<RollKey>& active_rolls) {
  if (!seen) {
    return;
  }
  std::erase_if(*seen, [&](const PendingRollSeen& entry) {
    return std::find(active_rolls.begin(), active_rolls.end(),
                     RollKey{entry.item_guid, entry.item_slot}) ==
           active_rolls.end();
  });
}

uint64_t RecordAndGetRollElapsedMs(std::vector<PendingRollSeen>* seen,
                                   RollKey key,
                                   uint64_t now_ms) {
  if (!seen) {
    return 0;
  }
  for (const PendingRollSeen& entry : *seen) {
    if (entry.item_guid == key.item_guid && entry.item_slot == key.item_slot) {
      return now_ms >= entry.first_seen_ms ? now_ms - entry.first_seen_ms : 0;
    }
  }
  seen->push_back(PendingRollSeen{
      .item_guid = key.item_guid,
      .item_slot = key.item_slot,
      .first_seen_ms = now_ms,
  });
  return 0;
}

}  // namespace llm_brain
