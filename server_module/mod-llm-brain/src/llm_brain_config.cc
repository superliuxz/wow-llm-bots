// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "llm_brain_config.h"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "Config.h"

namespace llm_brain {
namespace {

std::vector<std::string> SplitNames(const std::string& csv) {
  std::vector<std::string> names;
  std::stringstream ss(csv);
  std::string name;
  while (std::getline(ss, name, ',')) {
    const size_t begin = name.find_first_not_of(" \t");
    const size_t end = name.find_last_not_of(" \t");
    if (begin != std::string::npos) {
      names.push_back(name.substr(begin, end - begin + 1));
    }
  }
  return names;
}

}  // namespace

Config LoadConfig() {
  Config config;
  config.enabled = sConfigMgr->GetOption<bool>("LlmBrain.Enable", false);
  config.socket_path =
      sConfigMgr->GetOption<std::string>("LlmBrain.SocketPath", "brain.sock");
  config.bots =
      SplitNames(sConfigMgr->GetOption<std::string>("LlmBrain.Bots", ""));
  config.snapshot_interval_ms =
      sConfigMgr->GetOption<uint32_t>("LlmBrain.SnapshotIntervalMs", 1000);
  return config;
}

}  // namespace llm_brain
