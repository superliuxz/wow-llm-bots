// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "config/config_parsing.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/string_split.h"

namespace llm_brain {
namespace {

std::string FormatStrategyChecks(const std::vector<StrategyCheck>& checks) {
  std::string out;
  for (size_t i = 0; i < checks.size(); ++i) {
    if (i > 0) {
      out.push_back(',');
    }
    out.push_back(checks[i].present ? '+' : '-');
    out += checks[i].name;
  }
  return out;
}

void SetStrategyCheck(std::vector<StrategyCheck>& checks,
                      std::string_view name,
                      bool present) {
  for (StrategyCheck& c : checks) {
    if (c.name == name) {
      c.present = present;
      return;
    }
  }
  checks.push_back(StrategyCheck{std::string(name), present});
}

}  // namespace

std::vector<std::string> SplitList(const std::string& csv) {
  return base::SplitString(csv, ",", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY);
}

std::vector<std::pair<std::string, std::string>> ParseFixtures(
    const std::string& csv,
    const std::string& dir) {
  std::vector<std::pair<std::string, std::string>> fixtures;
  for (const std::string& entry : SplitList(csv)) {
    const size_t eq = entry.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == entry.size()) {
      continue;
    }
    std::string file = entry.substr(eq + 1);
    if (!dir.empty() && file.front() != '/' && !file.starts_with("new:")) {
      file = dir + "/" + file;
    }
    fixtures.emplace_back(entry.substr(0, eq), std::move(file));
  }
  return fixtures;
}

std::vector<StrategyCheck> StrategyChecks(const std::string& changes) {
  std::vector<StrategyCheck> checks;
  for (const std::string& change : SplitList(changes)) {
    if (change.size() < 2 || (change.front() != '+' && change.front() != '-')) {
      continue;
    }
    StrategyCheck check{change.substr(1), change.front() == '+'};
    const auto same = std::find_if(
        checks.begin(), checks.end(),
        [&](const StrategyCheck& c) { return c.name == check.name; });
    if (same != checks.end()) {
      *same = std::move(check);  // The last change to a name wins.
    } else {
      checks.push_back(std::move(check));
    }
  }
  return checks;
}

std::string AdjustNonCombatStrategies(const std::string& base_non_combat,
                                      bool in_group,
                                      bool is_leader,
                                      bool in_dungeon,
                                      bool group_has_human,
                                      bool brain_led) {
  std::vector<StrategyCheck> checks = StrategyChecks(base_non_combat);
  bool has_autonomous_strategy = false;
  for (const StrategyCheck& c : checks) {
    if ((c.name == "grind" || c.name == "new rpg") && c.present) {
      has_autonomous_strategy = true;
      break;
    }
  }
  if (!has_autonomous_strategy) {
    return base_non_combat;
  }

  if (in_group && !is_leader) {
    // Followers follow and never solo-grind or RPG-travel.
    SetStrategyCheck(checks, "follow", true);
    SetStrategyCheck(checks, "grind", false);
    SetStrategyCheck(checks, "new rpg", false);
  } else if (brain_led || (in_group && group_has_human)) {
    // While brain-led, the brain owns quests, grinding, travel, and flight
    // paths for solo bots and group leaders alike. Even when playerbots-led, a
    // bot leading a human never solo-grinds or RPG-travels.
    SetStrategyCheck(checks, "grind", false);
    SetStrategyCheck(checks, "new rpg", false);
  } else if (in_dungeon) {
    // Playerbots-led dungeon leaders/solo bots strip `+new rpg` so open-world
    // RPG travel does not Hearthstone them out of the instance.
    SetStrategyCheck(checks, "new rpg", false);
  } else {
    return base_non_combat;
  }

  return FormatStrategyChecks(checks);
}

std::string_view ScriptedRaidStrategyForMap(uint32_t map_id) {
  switch (map_id) {
    case 249:
      return "onyxia";  // Onyxia's Lair
    case 309:
      return "zulgurub";  // Zul'Gurub
    case 409:
      return "moltencore";  // Molten Core
    case 469:
      return "bwl";  // Blackwing Lair
    case 509:
      return "aq20";  // Ruins of Ahn'Qiraj
    case 531:
      return "aq40";  // Temple of Ahn'Qiraj
    case 532:
      return "karazhan";  // Karazhan
    case 533:
      return "naxx";  // Naxxramas
    case 534:
      return "hyjal";  // Hyjal Summit
    case 544:
      return "magtheridon";  // Magtheridon's Lair
    case 548:
      return "ssc";  // Serpentshrine Cavern
    case 550:
      return "tempestkeep";  // Tempest Keep: The Eye
    case 564:
      return "blacktemple";  // Black Temple
    case 565:
      return "gruulslair";  // Gruul's Lair
    case 568:
      return "zulaman";  // Zul'Aman
    case 603:
      return "ulduar";  // Ulduar
    case 615:
      return "wotlk-os";  // Obsidian Sanctum
    case 616:
      return "wotlk-eoe";  // Eye of Eternity
    case 624:
      return "voa";  // Vault of Archavon
    case 631:
      return "icc";  // Icecrown Citadel
    case 724:
      return "rs";  // Ruby Sanctum
    default:
      return "";
  }
}

std::string AdjustRaidStrategies(const std::string& base_strategies,
                                 uint32_t map_id,
                                 bool brain_led,
                                 bool apply_instance_strategies) {
  const std::string_view raid_strat = ScriptedRaidStrategyForMap(map_id);
  if (raid_strat.empty()) {
    return base_strategies;
  }
  std::vector<StrategyCheck> checks = StrategyChecks(base_strategies);
  bool changed = false;
  if (brain_led) {
    // Strip only autonomous open-world sub-behaviors that clash with the brain
    // (`grind`, `new rpg`), while preserving playerbots' raid strategy as the
    // body layer (vehicles, harpoons, cannons, Malygos drakes, positioning).
    for (StrategyCheck& c : checks) {
      if ((c.name == "grind" || c.name == "new rpg") && c.present) {
        c.present = false;
        changed = true;
      }
    }
  }
  if (apply_instance_strategies) {
    const auto existing = std::find_if(
        checks.begin(), checks.end(),
        [&](const StrategyCheck& c) { return c.name == raid_strat; });
    if (existing == checks.end()) {
      SetStrategyCheck(checks, raid_strat, true);
      changed = true;
    }
  }
  return changed ? FormatStrategyChecks(checks) : base_strategies;
}

std::string GroupedCombatStrategies(const std::string& combat, bool is_healer) {
  std::string out;
  for (const StrategyCheck& check : StrategyChecks(combat)) {
    bool present = check.present;
    if (check.name == "threat") {
      present = !is_healer;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += (present ? "+" : "-") + check.name;
  }
  return out;
}

GroupReflexLimits GroupReflexLimitsFor(bool in_group,
                                       bool in_dungeon,
                                       bool group_has_human) {
  return {.block_flee = in_group && (in_dungeon || group_has_human),
          .pet_defensive = in_group};
}

}  // namespace llm_brain
