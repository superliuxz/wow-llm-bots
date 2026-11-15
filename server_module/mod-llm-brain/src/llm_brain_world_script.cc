// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <cstddef>
#include <memory>
#include <string>

#include "Define.h"
#include "GameTime.h"
#include "Log.h"
#include "WorldScript.h"
#include "brain_bridge.h"
#include "llm_brain_config.h"

namespace llm_brain {
namespace {

constexpr size_t kMaxPendingMessages = 1000;

// World-thread entry points for the brain bridge. A2: run the socket bridge,
// send a heartbeat every snapshot interval, log what the harness sends.
class LlmBrainWorldScript : public WorldScript {
 public:
  LlmBrainWorldScript()
      : WorldScript("LlmBrainWorldScript",
                    {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP,
                     WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN}) {}

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
    if (!config_.enabled) {
      return;
    }
    bridge_ = std::make_unique<BrainBridge>(
        config_.socket_path, kMaxPendingMessages,
        BrainBridge::Callbacks{
            [](std::string message) {
              LOG_INFO("module", "mod-llm-brain: from harness: {}", message);
            },
            [](bool connected) {
              LOG_INFO("module", "mod-llm-brain: harness {}",
                       connected ? "connected" : "disconnected");
            }});
    if (!bridge_->Start()) {
      LOG_ERROR("module", "mod-llm-brain: cannot listen on {}",
                config_.socket_path);
      bridge_.reset();
      return;
    }
    LOG_INFO("module", "mod-llm-brain: listening on {}", config_.socket_path);
  }

  void OnUpdate(uint32 diff) override {
    if (!bridge_) {
      return;
    }
    bridge_->RunWorldTasks();
    since_snapshot_ms_ += diff;
    if (since_snapshot_ms_ < config_.snapshot_interval_ms) {
      return;
    }
    since_snapshot_ms_ = 0;
    bridge_->Send("{\"type\":\"heartbeat\",\"t\":" +
                  std::to_string(GameTime::GetGameTimeMS().count()) + "}");
  }

  void OnShutdown() override {
    if (bridge_) {
      bridge_->Stop();
      bridge_.reset();
    }
  }

 private:
  Config config_;
  std::unique_ptr<BrainBridge> bridge_;
  uint32 since_snapshot_ms_ = 0;
};

}  // namespace
}  // namespace llm_brain

void AddLlmBrainScripts() {
  new llm_brain::LlmBrainWorldScript();
}
