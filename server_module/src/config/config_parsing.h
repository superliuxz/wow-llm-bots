// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CONFIG_CONFIG_PARSING_H_
#define MOD_LLM_BRAIN_CONFIG_CONFIG_PARSING_H_

#include <string>
#include <utility>
#include <vector>

namespace llm_brain {

// Splits "a, b,,c " into {"a", "b", "c"} (trims spaces and tabs, drops empty
// entries).
std::vector<std::string> SplitList(const std::string& csv);

// Parses "Name=file,Name2=/abs/file" into (name, path) pairs. Relative files
// are joined to `dir` when it is set. Malformed entries are skipped.
std::vector<std::pair<std::string, std::string>> ParseFixtures(
    const std::string& csv,
    const std::string& dir);

// What must hold while the strategy changes `changes` ("-follow,+grind,+new
// rpg") are in effect: every strategy added present, every one removed absent
// (the last change to a name wins). If any no longer holds, playerbots has
// reset the strategies since we applied them.
struct StrategyCheck {
  std::string name;
  bool present = true;
  bool operator==(const StrategyCheck&) const = default;
};
std::vector<StrategyCheck> StrategyChecks(const std::string& changes);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CONFIG_CONFIG_PARSING_H_
