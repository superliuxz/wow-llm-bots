// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "config/llm_brain_config.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Config.h"
#include "config/config_parsing.h"
#include "options/position_facts.h"

namespace llm_brain {
namespace {

float ClampFloat(float value, float fallback, float min_val, float max_val) {
  if (!std::isfinite(value)) {
    return fallback;
  }
  return std::clamp(value, min_val, max_val);
}

}  // namespace

Config LoadConfig() {
  Config config;
  config.enabled = sConfigMgr->GetOption<bool>("LlmBrain.Enable", false);
  config.socket_path =
      sConfigMgr->GetOption<std::string>("LlmBrain.SocketPath", "brain.sock");
  config.bots =
      SplitList(sConfigMgr->GetOption<std::string>("LlmBrain.Bots", ""));
  config.snapshot_interval_ms =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.SnapshotIntervalMs", 1000);
  config.snapshot_interval_combat_ms =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.SnapshotIntervalCombatMs", 250);
  config.nameplate_range_yd =
      sConfigMgr->GetOption<float>("LlmBrain.NameplateRangeYd", 20);
  config.los_raycasts_per_snapshot =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.LosRaycastsPerSnapshot", 32);
  config.combat_strategies = sConfigMgr->GetOption<std::string>(
      "LlmBrain.CombatStrategies", "+boost,+dps assist,-threat");
  config.non_combat_strategies = sConfigMgr->GetOption<std::string>(
      "LlmBrain.NonCombatStrategies", "-follow,+grind,+new rpg");
  config.dead_strategies = sConfigMgr->GetOption<std::string>(
      "LlmBrain.DeadStrategies", "-follow,-stay");
  config.test_account =
      sConfigMgr->GetOption<std::string>("LlmBrain.TestAccount", "WLBTEST");
  config.test_fixtures = ParseFixtures(
      sConfigMgr->GetOption<std::string>("LlmBrain.TestFixtures", ""),
      sConfigMgr->GetOption<std::string>("LlmBrain.FixtureDir", ""));
  config.test_facts_queries =
      sConfigMgr->GetOption<std::string>("LlmBrain.TestFactsQueries", "");
  config.trace_dir =
      sConfigMgr->GetOption<std::string>("LlmBrain.TraceDir", "");
  config.trace_sample_interval_ms =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.TraceSampleIntervalMs", 200);
  if (config.trace_sample_interval_ms < 50) {
    config.trace_sample_interval_ms = 50;
  }
  config.trace_max_file_mb =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.TraceMaxFileMb", 64);
  if (config.trace_max_file_mb < 1) {
    config.trace_max_file_mb = 1;
  }
  config.trace_max_rotated_files =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.TraceMaxRotatedFiles", 8);
  if (config.trace_max_rotated_files > 64) {
    config.trace_max_rotated_files = 64;
  }
  config.observe_humans =
      sConfigMgr->GetOption<bool>("LlmBrain.ObserveHumans", false);
  config.dungeon_nav.cluster_radius_yd = ClampFloat(
      sConfigMgr->GetOption<float>("LlmBrain.DungeonClusterRadiusYd", 30.0f),
      30.0f, 1.0f, 200.0f);
  config.dungeon_nav.max_cluster_candidates =
      std::clamp<uint32_t>(sConfigMgr->GetOption<uint32_t>(
                               "LlmBrain.DungeonMaxClusterCandidates", 12),
                           1u, 32u);
  config.dungeon_nav.unloaded_probe_yd = ClampFloat(
      sConfigMgr->GetOption<float>("LlmBrain.DungeonUnloadedProbeYd", 60.0f),
      60.0f, kMinTravelDistYd, kMaxTravelDistYd);
  config.dungeon_nav.incomplete_progress_slack_yd =
      ClampFloat(sConfigMgr->GetOption<float>(
                     "LlmBrain.DungeonIncompleteProgressSlackYd", 15.0f),
                 15.0f, 0.0f, 500.0f);
  config.dungeon_nav.seam_bridge_enable =
      sConfigMgr->GetOption<bool>("LlmBrain.DungeonSeamBridgeEnable", true);
  config.dungeon_nav.seam_bridge_max_drop_yd = ClampFloat(
      sConfigMgr->GetOption<float>("LlmBrain.DungeonSeamBridgeMaxDropYd", 6.0f),
      6.0f, 0.0f, 20.0f);
  config.dungeon_nav.seam_bridge_max_climb_yd =
      ClampFloat(sConfigMgr->GetOption<float>(
                     "LlmBrain.DungeonSeamBridgeMaxClimbYd", 2.5f),
                 2.5f, 0.0f, 10.0f);
  return config;
}

}  // namespace llm_brain
