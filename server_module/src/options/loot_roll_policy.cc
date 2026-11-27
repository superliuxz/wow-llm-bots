// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/loot_roll_policy.h"

#include <algorithm>
#include <cstdint>
#include <optional>
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

StatFit EvaluateStatFit(LootStatProfile profile,
                        uint8_t class_id,
                        const ItemStatFlags& stats) {
  bool good = false;
  bool bad = false;

  switch (profile) {
    case LootStatProfile::kSpellCaster:
      good = stats.intellect || stats.spell_power || stats.spirit || stats.mp5;
      bad = stats.strength || stats.agility || stats.attack_power ||
            stats.tank_avoidance || stats.melee_dps;
      break;
    case LootStatProfile::kMeleeStrength:
      good = stats.strength || stats.attack_power || stats.melee_dps;
      bad = stats.agility || stats.intellect || stats.spirit ||
            stats.spell_power || stats.mp5 || stats.tank_avoidance;
      break;
    case LootStatProfile::kTankStrength:
      good = stats.strength || stats.tank_avoidance || stats.attack_power ||
             stats.melee_dps;
      bad = stats.intellect || stats.spirit || stats.spell_power || stats.mp5;
      break;
    case LootStatProfile::kPhysicalAgility: {
      const bool is_feral = class_id == kClassDruid;
      const bool wants_int =
          class_id == kClassHunter || class_id == kClassShaman;
      good = stats.agility || stats.attack_power || stats.melee_dps ||
             (is_feral && stats.strength);
      bad = stats.spell_power || stats.spirit || stats.mp5 ||
            stats.tank_avoidance || (!is_feral && stats.strength) ||
            (!wants_int && stats.intellect);
      break;
    }
    case LootStatProfile::kTankAgility:
      good = stats.agility || stats.strength || stats.tank_avoidance ||
             stats.attack_power || stats.melee_dps;
      bad = stats.intellect || stats.spirit || stats.spell_power || stats.mp5;
      break;
  }

  if (good) {
    return StatFit::kMatch;
  }
  if (bad) {
    return StatFit::kMismatch;
  }
  return StatFit::kNeutral;
}

std::optional<LootRollChoice> DecidePolicyRollVote(
    const LootRollPolicyInput& in) {
  if (in.human_pending_vote && in.roll_elapsed_ms < in.human_defer_ms) {
    return std::nullopt;
  }

  const bool want_need =
      in.usable && !in.bind_on_equip && in.armor_type_matches &&
      !in.shield_disallowed_for_spec && in.has_upgrade_slot &&
      in.stat_fit != StatFit::kMismatch && !in.human_already_needed;
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
