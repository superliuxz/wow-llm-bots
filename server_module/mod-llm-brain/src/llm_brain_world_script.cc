// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <string>

#include "Log.h"
#include "WorldScript.h"
#include "llm_brain_config.h"

namespace llm_brain {
namespace {

// World-thread entry points for the brain bridge. A1: load config and log it.
class LlmBrainWorldScript : public WorldScript {
 public:
  LlmBrainWorldScript()
      : WorldScript("LlmBrainWorldScript",
                    {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP}) {}

  void OnAfterConfigLoad(bool /*reload*/) override { config_ = LoadConfig(); }

  void OnStartup() override {
    std::string bots;
    for (const std::string& name : config_.bots) {
      bots += bots.empty() ? name : ", " + name;
    }
    LOG_INFO("module",
             "mod-llm-brain: {} (socket {}, bots [{}], snapshot every {} ms)",
             config_.enabled ? "enabled" : "disabled", config_.socket_path,
             bots, config_.snapshot_interval_ms);
  }

 private:
  Config config_;
};

}  // namespace
}  // namespace llm_brain

void AddLlmBrainScripts() {
  new llm_brain::LlmBrainWorldScript();
}
