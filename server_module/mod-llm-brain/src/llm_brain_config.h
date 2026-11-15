// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_LLM_BRAIN_CONFIG_H_
#define MOD_LLM_BRAIN_LLM_BRAIN_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

namespace llm_brain {

struct Config {
  bool enabled = false;
  std::string socket_path = "brain.sock";
  std::vector<std::string> bots;
  uint32_t snapshot_interval_ms = 1000;
};

// Reads LlmBrain.* options from the loaded worldserver config.
Config LoadConfig();

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_LLM_BRAIN_CONFIG_H_
