// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "config/config_parsing.h"
#include <algorithm>

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace llm_brain {

std::vector<std::string> SplitList(const std::string& csv) {
  std::vector<std::string> items;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) {
    const size_t begin = item.find_first_not_of(" \t");
    const size_t end = item.find_last_not_of(" \t");
    if (begin != std::string::npos) {
      items.push_back(item.substr(begin, end - begin + 1));
    }
  }
  return items;
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
    if (!dir.empty() && file.front() != '/') {
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

}  // namespace llm_brain
