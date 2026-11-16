// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "llm_brain_config.h"

#include <cstdint>
#include <string>
#include <vector>

#include "Config.h"
#include "config_parsing.h"

namespace llm_brain {

Config LoadConfig() {
  Config config;
  config.enabled = sConfigMgr->GetOption<bool>("LlmBrain.Enable", false);
  config.socket_path =
      sConfigMgr->GetOption<std::string>("LlmBrain.SocketPath", "brain.sock");
  config.bots =
      SplitList(sConfigMgr->GetOption<std::string>("LlmBrain.Bots", ""));
  config.snapshot_interval_ms =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.SnapshotIntervalMs", 1000);
  config.nameplate_range_yd =
      sConfigMgr->GetOption<float>("LlmBrain.NameplateRangeYd", 20);
  config.los_raycasts_per_snapshot =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.LosRaycastsPerSnapshot", 32);
  config.combat_strategies = sConfigMgr->GetOption<std::string>(
      "LlmBrain.CombatStrategies", "+boost,+dps assist,-threat");
  config.non_combat_strategies = sConfigMgr->GetOption<std::string>(
      "LlmBrain.NonCombatStrategies", "-follow,+grind,+new rpg");
  config.dead_strategies =
      sConfigMgr->GetOption<std::string>("LlmBrain.DeadStrategies", "-follow");
  config.test_account =
      sConfigMgr->GetOption<std::string>("LlmBrain.TestAccount", "WLBTEST");
  config.test_fixtures = ParseFixtures(
      sConfigMgr->GetOption<std::string>("LlmBrain.TestFixtures", ""),
      sConfigMgr->GetOption<std::string>("LlmBrain.FixtureDir", ""));
  return config;
}

}  // namespace llm_brain
