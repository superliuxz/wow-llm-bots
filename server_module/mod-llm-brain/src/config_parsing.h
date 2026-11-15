// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CONFIG_PARSING_H_
#define MOD_LLM_BRAIN_CONFIG_PARSING_H_

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

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CONFIG_PARSING_H_
