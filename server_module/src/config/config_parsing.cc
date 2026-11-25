// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "config/config_parsing.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "base/strings/string_split.h"

namespace llm_brain {

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
                                      bool group_has_human) {
  std::vector<StrategyCheck> checks = StrategyChecks(base_non_combat);
  bool has_grind = false;
  for (const StrategyCheck& c : checks) {
    if (c.name == "grind" && c.present) {
      has_grind = true;
      break;
    }
  }
  if (!has_grind) {
    return base_non_combat;
  }

  auto set_strategy = [&](const char* name, bool present) {
    for (StrategyCheck& c : checks) {
      if (c.name == name) {
        c.present = present;
        return;
      }
    }
    checks.push_back(StrategyCheck{name, present});
  };

  if (in_group && (!is_leader || group_has_human)) {
    // Followers follow. A bot leading a human never solo-grinds or RPG-travels
    // either: it would wander, hearth or fly off, and pull trash alone in a
    // dungeon. (An all-bot party's leader still grinds its way through.)
    if (!is_leader) {
      set_strategy("follow", true);
    }
    set_strategy("grind", false);
    set_strategy("new rpg", false);
  } else if (in_dungeon) {
    set_strategy("new rpg", false);
  } else {
    return base_non_combat;
  }

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

}  // namespace llm_brain
