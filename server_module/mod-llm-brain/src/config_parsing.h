// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CONFIG_PARSING_H_
#define MOD_LLM_BRAIN_CONFIG_PARSING_H_

#include <optional>
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

// How to tell whether playerbots has reset a bot's strategies since we applied
// `changes` ("-follow,+grind,+new rpg"): the first strategy we add must be
// present, or else the first one we remove must be absent. nullopt if
// `changes` names no strategy.
struct StrategyCheck {
  std::string name;
  bool present = true;
  bool operator==(const StrategyCheck&) const = default;
};
std::optional<StrategyCheck> ResetCheckFor(const std::string& changes);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CONFIG_PARSING_H_
