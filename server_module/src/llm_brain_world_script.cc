// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Define.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldScript.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "bot_manager.h"
#include "brain_bridge.h"
#include "llm_brain_config.h"
#include "nearby_builder.h"
#include "option_executor.h"
#include "options.h"
#include "proto/snapshot.pb.h"
#include "snapshot_builder.h"
#include "snapshot_schedule.h"
#include "snapshot_stats.h"

#if defined(LLM_BRAIN_TEST_HOOKS)
#include "test/fixture_loader.h"
#endif

namespace llm_brain {
namespace {

constexpr size_t kMaxPendingMessages = 1000;
// How often the snapshot cost summary is logged.
constexpr uint32_t kStatsIntervalMs = 60000;
// How long a bot remembers its snapshots' options, so a choice made on a
// snapshot that is no longer the newest can still be resolved: well above an
// urgent wake-up on a busy GPU (p95 3.8 s, docs/phase2-results.md).
constexpr base::TimeDelta kOptionHistory = base::Seconds(30);

// World-thread entry points for the brain bridge: every snapshot interval,
// send each attached bot's snapshot to the harness.
class LlmBrainWorldScript : public WorldScript {
 public:
  LlmBrainWorldScript()
      : WorldScript("LlmBrainWorldScript",
                    {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP,
                     WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN}) {}

  void OnAfterConfigLoad(bool reload) override {
    if (reload) {
      // The bot list sizes the schedule and the bot manager at startup, and
      // the socket is already open: a running server keeps its settings.
      LOG_WARN("module",
               "mod-llm-brain: settings are read at startup only; restart "
               "the server to apply changes");
      return;
    }
    config_ = LoadConfig();
  }

  void OnStartup() override {
    std::string bots;
    for (const std::string& name : config_.bots) {
      bots += bots.empty() ? name : ", " + name;
    }
    LOG_INFO("module",
             "mod-llm-brain: {} (socket {}, bots [{}], snapshot every {} ms, "
             "{} ms in combat)",
             config_.enabled ? "enabled" : "disabled", config_.socket_path,
             bots, config_.snapshot_interval_ms,
             config_.snapshot_interval_combat_ms);
    schedule_ = std::make_unique<SnapshotSchedule>(
        config_.bots.size(), base::Milliseconds(config_.snapshot_interval_ms),
        base::Milliseconds(config_.snapshot_interval_combat_ms),
        base::TimeTicks::Now());
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
            base::BindRepeating(&LlmBrainWorldScript::OnHarnessMessage,
                                weak_factory_.GetWeakPtr()),
            base::BindRepeating(&LlmBrainWorldScript::OnHarnessConnection,
                                weak_factory_.GetWeakPtr())});
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
    since_stats_ms_ += diff;
    if (since_stats_ms_ >= kStatsIntervalMs) {
      since_stats_ms_ = 0;
      LogStats();
    }
    if (!bridge_->connected()) {
      return;  // Nobody to send to: don't spend world-thread time building.
    }
    // Each bot has its own moment in the interval (bots are spread over it, so
    // a tick builds one or two snapshots, not all of them), and a bot whose
    // combat or life state changed is due at once.
    const base::TimeTicks now = base::TimeTicks::Now();
    size_t built = 0;
    for (size_t i = 0; i < config_.bots.size(); ++i) {
      Player* bot = ObjectAccessor::FindPlayerByName(config_.bots[i]);
      if (!bot) {
        continue;  // Offline (or not in the world yet).
      }
      const SnapshotSchedule::Flags flags{bot->IsInCombat(), bot->IsAlive()};
      if (schedule_->IsDue(i, now, flags)) {
        SendSnapshot(config_.bots[i], bot);
        schedule_->Sent(i, now, flags);
        ++built;
      }
    }
    if (built > 0) {
      max_update_snapshots_ = std::max(max_update_snapshots_, built);
      max_update_time_ =
          std::max(max_update_time_, base::TimeTicks::Now() - now);
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
  // The minute's summary: what snapshots cost to build, how many one tick
  // built at most (the spread of the bots), and how long they took to reach the
  // socket.
  void LogStats() {
    if (stats_.count()) {
      LOG_INFO("module", "mod-llm-brain: snapshots in the last minute: {}",
               stats_.Summary());
      LOG_INFO("module",
               "mod-llm-brain: one tick built at most {} snapshots, in at most "
               "{} us",
               max_update_snapshots_, max_update_time_.InMicroseconds());
      stats_.Reset();
    }
    max_update_snapshots_ = 0;
    max_update_time_ = base::TimeDelta();
    if (!apply_ages_.empty()) {
      std::sort(apply_ages_.begin(), apply_ages_.end());
      LOG_INFO("module",
               "mod-llm-brain: choices applied n={} (age_at_apply p50={} ms "
               "max={} ms)",
               apply_ages_.size(),
               apply_ages_[apply_ages_.size() / 2].InMilliseconds(),
               apply_ages_.back().InMilliseconds());
      apply_ages_.clear();
    }
    BrainBridge::SendStats sent = bridge_->TakeSendStats();
    if (!sent.delays.empty() || sent.replaced > 0) {
      std::sort(sent.delays.begin(), sent.delays.end());
      const auto at = [&](double q) {
        return sent.delays.empty()
                   ? int64_t{0}
                   : sent
                         .delays[std::min(
                             sent.delays.size() - 1,
                             static_cast<size_t>(
                                 q * static_cast<double>(sent.delays.size())))]
                         .InMicroseconds();
      };
      LOG_INFO("module",
               "mod-llm-brain: sent n={} (built to socket p50={}us p99={}us "
               "max={}us), {} replaced by a newer snapshot",
               sent.delays.size(), at(0.5), at(0.99), at(1.0), sent.replaced);
    }
  }

  void OnHarnessConnection(bool connected) {
    LOG_INFO("module", "mod-llm-brain: harness {}",
             connected ? "connected" : "disconnected");
    if (connected) {
      // Every bot is overdue by now: spread them again instead of building
      // them all on the next tick.
      schedule_->Restart(base::TimeTicks::Now());
    }
  }

  // A command from the harness (docs/specs/snapshot.md §9). So far: Choose.
  void OnHarnessMessage(std::string bytes) {
    wlb::snapshot::Command command;
    if (!command.ParseFromString(bytes)) {
      LOG_ERROR("module", "mod-llm-brain: unreadable command ({} bytes)",
                bytes.size());
      return;
    }
    const std::string name(command.bot());
    if (!command.has_choose()) {
      LOG_INFO("module", "mod-llm-brain: {}: command not supported yet", name);
      return;
    }
    const auto state = bots_.find(name);
    Player* bot = ObjectAccessor::FindPlayerByName(name);
    if (state == bots_.end() || !bot) {
      LOG_INFO("module", "mod-llm-brain: choose for {}: no such bot online",
               name);
      return;
    }
    const wlb::snapshot::Command::Choose& choose = command.choose();
    const OptionHistory::Lookup found =
        state->second.history.Find(choose.seq(), choose.option());
    std::string error = found.error;
    if (found.target) {
      error = ExecuteOption(bot, *found.target);
    }
    // How old the picture was when the bot acted on it: the number that
    // matters (docs/plans/harness.md, "Age").
    const int64_t age_ms =
        found.target
            ? (base::TimeTicks::Now() - found.snapshot_built).InMilliseconds()
            : -1;
    if (found.target) {
      apply_ages_.push_back(base::Milliseconds(age_ms));
    }
    LOG_INFO("module",
             "mod-llm-brain: {} chose {} of snapshot {} ({} {}): {}, "
             "age_at_apply={} ms",
             name, choose.option(), choose.seq(),
             found.target ? OptionKindName(found.target->kind) : "-",
             found.target ? found.target->name : "",
             error.empty() ? "ok" : error, age_ms);
    if (!error.empty()) {
      // The bot sees it like the client's red error line.
      wlb::snapshot::ModuleMessage message;
      wlb::snapshot::ImmediateEvent* event = message.mutable_event();
      event->set_bot(name);
      event->set_seq(state->second.seq);
      event->set_time_ms(base::Time::Now().InMillisecondsSinceUnixEpoch());
      event->set_monotonic_us(
          (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds());
      wlb::snapshot::ErrorEvent* failed =
          event->mutable_event()->mutable_error();
      failed->set_text(error);
      failed->set_command_id(command.command_id());
      bridge_->Send(message.SerializeAsString());
    }
  }

  void SendSnapshot(const std::string& name, Player* bot) {
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
    snapshot->set_monotonic_us((start - base::TimeTicks()).InMicroseconds());
    const NearbyBuilder::Stats nearby = state.builder->Build(bot, snapshot);
    state.history.Add(snapshot->seq(), start, AddOptions(snapshot));
    std::string bytes = message.SerializeAsString();
    stats_.Add({base::TimeTicks::Now() - start, bytes.size(), nearby.units,
                nearby.objects, nearby.hidden, nearby.raycasts});
    // The newest snapshot of a bot wins if the harness is behind.
    bridge_->SendLatest(name, std::move(bytes));
  }

  Config config_;
#if defined(LLM_BRAIN_TEST_HOOKS)
  std::unique_ptr<FixtureLoader> fixture_loader_;
#endif
  std::unique_ptr<BotManager> bot_manager_;
  std::unique_ptr<BrainBridge> bridge_;
  std::unique_ptr<SnapshotSchedule> schedule_;
  uint32 since_stats_ms_ = 0;
  size_t max_update_snapshots_ = 0;
  base::TimeDelta max_update_time_;
  SnapshotStats stats_;
  struct BotState {
    // Snapshot counter (gaps tell the harness something was dropped).
    uint32_t seq = 0;
    std::unique_ptr<SnapshotBuilder> builder;
    OptionHistory history{kOptionHistory};
  };
  std::map<std::string, BotState> bots_;
  // How old each applied choice's snapshot was, since the last summary.
  std::vector<base::TimeDelta> apply_ages_;
  base::WeakPtrFactory<LlmBrainWorldScript> weak_factory_{this};
};

}  // namespace
}  // namespace llm_brain

void AddLlmBrainScripts() {
  new llm_brain::LlmBrainWorldScript();
}
