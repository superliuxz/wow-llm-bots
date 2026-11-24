// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CONFIG_LLM_BRAIN_CONFIG_H_
#define MOD_LLM_BRAIN_CONFIG_LLM_BRAIN_CONFIG_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "options/position_facts.h"

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
  // Unified human + bot behavior trace directory (empty disables tracing).
  std::string trace_dir;
  // Minimum interval between periodic behavior trace samples (ms). Defaults to
  // 200 ms (5 Hz). Observer-parity scoring (tools/bench/trace_parity.py)
  // requires a measured median rate >= 3.5 Hz in the trace; world-tick rounding
  // adds up to one tick (~50 ms) on top of this interval, and
  // LlmBrainWorldScript warns when the measured rate drops below 3.5 Hz, so
  // keep the default 200 ms whenever traces are scored. Use TraceMaxFileMb /
  // TraceMaxRotatedFiles to bound disk volume.
  uint32_t trace_sample_interval_ms = 200;
  // Per-character, per-day trace volume cap: each day file rotates at
  // `trace_max_file_mb` and keeps `trace_max_rotated_files` archives.
  uint32_t trace_max_file_mb = 64;
  uint32_t trace_max_rotated_files = 8;
  // When true (and `trace_dir` is non-empty), also logs online human players
  // using the same trace schema so bots can be compared against a human
  // baseline.
  bool observe_humans = false;
  // Multi-room dungeon navigation geometry thresholds (LlmBrain.Dungeon*).
  DungeonNavConfig dungeon_nav;
};

// Reads LlmBrain.* options from the loaded worldserver config.
Config LoadConfig();

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CONFIG_LLM_BRAIN_CONFIG_H_
