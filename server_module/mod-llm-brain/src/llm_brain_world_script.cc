// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "Define.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldScript.h"
#include "base/time/time.h"
#include "bot_manager.h"
#include "brain_bridge.h"
#include "llm_brain_config.h"
#include "nearby_builder.h"
#include "proto/snapshot.pb.h"
#include "snapshot_builder.h"
#include "snapshot_stats.h"

#if defined(LLM_BRAIN_TEST_HOOKS)
#include <vector>

#include "test/fixture_loader.h"
#endif

namespace llm_brain {
namespace {

constexpr size_t kMaxPendingMessages = 1000;
// How often the snapshot cost summary is logged.
constexpr uint32_t kStatsIntervalMs = 60000;

// World-thread entry points for the brain bridge: every snapshot interval,
// send each attached bot's snapshot to the harness.
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
#if defined(LLM_BRAIN_TEST_HOOKS)
    if (!config_.test_fixtures.empty()) {
      std::vector<FixtureLoader::Fixture> fixtures;
      for (const auto& [character, file] : config_.test_fixtures) {
        fixtures.push_back({character, file});
      }
      fixture_loader_ = std::make_unique<FixtureLoader>(config_.test_account,
                                                        std::move(fixtures));
    }
#else
    if (!config_.test_fixtures.empty()) {
      LOG_ERROR("module",
                "mod-llm-brain: LlmBrain.TestFixtures ignored: production "
                "build (test builds: tools/build-server.sh --test)");
    }
#endif
    bot_manager_ = std::make_unique<BotManager>(
        config_.bots, BotManager::Strategies{config_.combat_strategies,
                                             config_.non_combat_strategies,
                                             config_.dead_strategies});
    bridge_ = std::make_unique<BrainBridge>(
        config_.socket_path, kMaxPendingMessages,
        BrainBridge::Callbacks{
            [](std::string message) {
              // Commands are parsed and applied in a later step.
              LOG_INFO("module", "mod-llm-brain: from harness: {} bytes",
                       message.size());
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
#if defined(LLM_BRAIN_TEST_HOOKS)
    // Test fixtures are reset before the bot manager logs anyone in.
    if (fixture_loader_ && fixture_loader_->Update()) {
      fixture_loader_.reset();
    }
    const bool fixtures_pending = fixture_loader_ != nullptr;
#else
    const bool fixtures_pending = false;
#endif
    if (bot_manager_ && !fixtures_pending) {
      bot_manager_->Update(diff);
    }
    if (!bridge_) {
      return;
    }
    bridge_->RunWorldTasks();
    since_snapshot_ms_ += diff;
    since_stats_ms_ += diff;
    if (since_stats_ms_ >= kStatsIntervalMs) {
      since_stats_ms_ = 0;
      if (stats_.count()) {
        LOG_INFO("module", "mod-llm-brain: snapshots in the last minute: {}",
                 stats_.Summary());
        stats_.Reset();
      }
    }
    if (since_snapshot_ms_ < config_.snapshot_interval_ms) {
      return;
    }
    since_snapshot_ms_ = 0;
    if (!bridge_->connected()) {
      return;  // Nobody to send to: don't spend world-thread time building.
    }
    for (const std::string& name : config_.bots) {
      SendSnapshot(name);
    }
  }

  void OnShutdown() override {
    if (bot_manager_) {
      bot_manager_->Stop();
      bot_manager_.reset();
    }
    if (bridge_) {
      bridge_->Stop();
      bridge_.reset();
    }
  }

 private:
  void SendSnapshot(const std::string& name) {
    Player* bot = ObjectAccessor::FindPlayerByName(name);
    if (!bot) {
      return;  // Offline (or not in the world yet).
    }
    BotState& state = bots_[name];
    if (!state.builder) {
      state.builder = std::make_unique<SnapshotBuilder>(NearbyBuilder::Options{
          config_.nameplate_range_yd, config_.los_raycasts_per_snapshot});
    }
    wlb::snapshot::ModuleMessage message;
    wlb::snapshot::Snapshot* snapshot = message.mutable_snapshot();
    snapshot->set_bot(name);
    snapshot->set_seq(++state.seq);
    snapshot->set_time_ms(base::Time::Now().InMillisecondsSinceUnixEpoch());
    const base::TimeTicks start = base::TimeTicks::Now();
    const NearbyBuilder::Stats nearby = state.builder->Build(bot, snapshot);
    std::string bytes = message.SerializeAsString();
    stats_.Add({base::TimeTicks::Now() - start, bytes.size(), nearby.units,
                nearby.objects, nearby.hidden, nearby.raycasts});
    bridge_->Send(std::move(bytes));
  }

  Config config_;
#if defined(LLM_BRAIN_TEST_HOOKS)
  std::unique_ptr<FixtureLoader> fixture_loader_;
#endif
  std::unique_ptr<BotManager> bot_manager_;
  std::unique_ptr<BrainBridge> bridge_;
  uint32 since_snapshot_ms_ = 0;
  uint32 since_stats_ms_ = 0;
  SnapshotStats stats_;
  struct BotState {
    // Snapshot counter (gaps tell the harness something was dropped).
    uint32_t seq = 0;
    std::unique_ptr<SnapshotBuilder> builder;
  };
  std::map<std::string, BotState> bots_;
};

}  // namespace
}  // namespace llm_brain

void AddLlmBrainScripts() {
  new llm_brain::LlmBrainWorldScript();
}
