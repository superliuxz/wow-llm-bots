// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CONFIG_LLM_BRAIN_CONFIG_H_
#define MOD_LLM_BRAIN_CONFIG_LLM_BRAIN_CONFIG_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace llm_brain {

struct Config {
  bool enabled = false;
  std::string socket_path = "brain.sock";
  // Characters our bot manager keeps online and the brain controls.
  std::vector<std::string> bots;
  uint32_t snapshot_interval_ms = 1000;
  uint32_t snapshot_interval_combat_ms = 250;
  // How far names / nameplate fields show in `nearby` (later per bot, from its
  // UI settings), and the line-of-sight ray-cast budget per snapshot.
  float nameplate_range_yd = 20;
  uint32_t los_raycasts_per_snapshot = 32;
  // Playerbots strategy changes the bot manager keeps applied (see
  // docs/playerbots-random-bot-audit.md).
  std::string combat_strategies;
  std::string non_combat_strategies;
  std::string dead_strategies;
  // Test only: characters reset from fixture files at startup, as
  // (character, fixture file path) pairs, on `test_account`.
  std::string test_account = "WLBTEST";
  std::vector<std::pair<std::string, std::string>> test_fixtures;
  // Test only: a query file for test/facts_dump.cc; the eval fixtures'
  // OptionFacts are written at startup (tools/snapshot/add_fixture_options.py
  // --capture).
  std::string test_facts_queries;
};

// Reads LlmBrain.* options from the loaded worldserver config.
Config LoadConfig();

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CONFIG_LLM_BRAIN_CONFIG_H_
