// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Creature.h"
#include "Define.h"
#include "GameObject.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "WorldScript.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "bot_manager/bot_manager.h"
#include "bridge/brain_bridge.h"
#include "chat/chat_inbox.h"
#include "chat/chat_scripts.h"
#include "chat/speak.h"
#include "config/llm_brain_config.h"
#include "options/option_executor.h"
#include "options/options.h"
#include "proto/snapshot.h"
#include "snapshot/nearby_builder.h"
#include "snapshot/snapshot_builder.h"
#include "snapshot/snapshot_schedule.h"
#include "snapshot/snapshot_stats.h"

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

uint8_t ControlMask(const Player* bot) {
  uint8_t mask = 0;
  if (bot->HasUnitState(UNIT_STATE_STUNNED) ||
      bot->HasUnitFlag(UNIT_FLAG_STUNNED)) {
    mask |= 1u << 0;
  }
  if (bot->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) ||
      bot->HasUnitFlag(UNIT_FLAG_FLEEING | UNIT_FLAG_CONFUSED)) {
    mask |= 1u << 1;
  }
  if (bot->HasUnitState(UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED) ||
      !bot->GetCharmerGUID().IsEmpty()) {
    mask |= 1u << 2;
  }
  if (bot->HasUnitFlag(UNIT_FLAG_SILENCED)) {
    mask |= 1u << 3;
  }
  if (bot->HasUnitState(UNIT_STATE_ROOT)) {
    mask |= 1u << 4;
  }
  if (bot->HasUnitFlag(UNIT_FLAG_PACIFIED)) {
    mask |= 1u << 5;
  }
  if (bot->HasUnitFlag(UNIT_FLAG_DISARMED)) {
    mask |= 1u << 6;
  }
  return mask;
}

// Whether the body is still carrying out `target` (`used`: a non-loot object
// was already clicked on arrival).
bool IsOptionTargetPending(Player* bot, const OptionTarget& target, bool used) {
  switch (target.kind) {
    case OptionTarget::Kind::kAttack: {
      Unit* unit =
          ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(target.guid));
      return unit != nullptr && unit->IsInWorld() && !unit->isDead();
    }
    case OptionTarget::Kind::kLoot: {
      Creature* creature =
          ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(target.guid));
      return creature != nullptr && creature->IsInWorld() &&
             creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
    }
    case OptionTarget::Kind::kGather:
    case OptionTarget::Kind::kUse: {
      GameObject* go =
          ObjectAccessor::GetGameObject(*bot, ParseSnapshotGuid(target.guid));
      if (go == nullptr || !go->IsInWorld() || !go->isSpawned()) {
        return false;
      }
      // A chest, herb or vein is done once opened; anything else once clicked.
      return IsLootableObject(go) ? go->getLootState() == GO_READY : !used;
    }
    case OptionTarget::Kind::kKeep:
      return false;
  }
  return false;
}

// World-thread entry points for the brain bridge: every snapshot interval,
// send each attached bot's snapshot to the harness.
class LlmBrainWorldScript : public WorldScript {
 public:
  explicit LlmBrainWorldScript(ChatInbox* inbox)
      : WorldScript("LlmBrainWorldScript",
                    {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP,
                     WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN}),
        inbox_(inbox) {}

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
    bots_.resize(config_.bots.size());
    if (!config_.enabled) {
      return;
    }
    inbox_->SetBots(config_.bots);
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
                "build (test builds: tools/wlb build server --test)");
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
    DeliverChat();
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
      if (!bot || !bot->IsInWorld()) {
        continue;  // Offline (or not in the world yet).
      }
      UseOnArrival(bot, i, config_.bots[i]);
      const SnapshotSchedule::Flags flags = CurrentFlags(bot, i);
      if (schedule_->IsDue(i, now, flags)) {
        SendSnapshot(i, config_.bots[i], bot);
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
    if (!sent.delays.empty() || sent.replaced > 0 || sent.dropped > 0) {
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
               "max={}us), {} replaced by a newer snapshot, {} dropped "
               "(socket refused)",
               sent.delays.size(), at(0.5), at(0.99), at(1.0), sent.replaced,
               sent.dropped);
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

  // The index of `name` in LlmBrain.Bots, or config_.bots.size().
  size_t BotIndex(const std::string& name) const {
    for (size_t i = 0; i < config_.bots.size(); ++i) {
      if (config_.bots[i] == name) {
        return i;
      }
    }
    return config_.bots.size();
  }

  // Whispers the bots heard since the last update, each sent to the harness
  // as an event (with a fresh snapshot first, like any alerting event).
  void DeliverChat() {
    if (const size_t dropped = inbox_->TakeDropped()) {
      LOG_WARN("module", "mod-llm-brain: {} chat lines dropped (inbox full)",
               dropped);
    }
    for (ChatInbox::Line& line : inbox_->TakeAll()) {
      const size_t index = BotIndex(line.to);
      Player* bot = ObjectAccessor::FindPlayerByName(line.to);
      if (index == config_.bots.size() || !bot || !bot->IsInWorld() ||
          !bridge_->connected()) {
        continue;  // Nobody to tell: like a whisper while away.
      }
      LOG_INFO("module", "mod-llm-brain: {} heard a whisper from {}: {}",
               line.to, line.chat.from, line.chat.text);
      wlb::snapshot::Event payload;
      payload.chat = std::move(line.chat);
      EmitAlertingEvent(index, line.to, bot, std::move(payload));
    }
  }

  // A command from the harness (docs/specs/snapshot.md §9): Choose and Say.
  void OnHarnessMessage(std::string bytes) {
    wlb::snapshot::Command command;
    if (!wlb::snapshot::ReadBeve(bytes, &command)) {
      LOG_ERROR("module", "mod-llm-brain: unreadable command ({} bytes)",
                bytes.size());
      return;
    }
    const std::string& name = command.bot;
    if (!command.choose && !command.say) {
      LOG_INFO("module", "mod-llm-brain: {}: command not supported yet", name);
      return;
    }
    const size_t bot_index = BotIndex(name);
    Player* bot = ObjectAccessor::FindPlayerByName(name);
    if (bot_index == config_.bots.size() || !bot || bots_[bot_index].seq == 0) {
      LOG_INFO("module", "mod-llm-brain: command for {}: no such bot online",
               name);
      return;
    }
    if (command.say) {
      OnSay(bot_index, name, bot, command.command_id, *command.say);
      return;
    }
    const wlb::snapshot::Command::Choose& choose = *command.choose;
    const OptionHistory::Lookup found =
        bots_[bot_index].history.Find(choose.seq, choose.option);
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
             name, choose.option, choose.seq,
             found.target ? OptionKindName(found.target->kind) : "-",
             found.target ? found.target->name : "",
             error.empty() ? "ok" : error, age_ms);
    if (!error.empty()) {
      bots_[bot_index].active_option.reset();
      schedule_->SetActiveTargetPending(bot_index, false);
      // The bot sees it like the client's red error line; attach a fresh
      // snapshot first so the harness acts on current world state rather than a
      // snapshot up to 999 ms stale.
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = error;
      failed.command_id = command.command_id;
      EmitAlertingEvent(bot_index, name, bot, std::move(payload));
    } else if (found.target) {
      bots_[bot_index].object_used = false;
      bots_[bot_index].active_command_id = command.command_id;
      const bool pending = IsOptionTargetPending(bot, *found.target, false);
      if (pending) {
        bots_[bot_index].active_option = *found.target;
      } else {
        bots_[bot_index].active_option.reset();
      }
      schedule_->SetActiveTargetPending(bot_index, pending);
    }
  }

  void OnSay(size_t bot_index,
             const std::string& name,
             Player* bot,
             uint32_t command_id,
             const wlb::snapshot::Command::Say& say) {
    const std::string error = Speak(bot, say);
    LOG_INFO("module", "mod-llm-brain: {} says ({}{}{}): {}: {}", name,
             wlb::snapshot::EnumName(say.channel), say.to.empty() ? "" : " to ",
             say.to, say.text, error.empty() ? "ok" : error);
    if (!error.empty()) {
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = error;
      failed.command_id = command_id;
      EmitAlertingEvent(bot_index, name, bot, std::move(payload));
    }
  }

  // A Use step whose object playerbots can't walk to itself: the executor
  // started the walk; click it once in reach, as a player would on arrival.
  void UseOnArrival(Player* bot, size_t index, const std::string& name) {
    BotState& state = bots_[index];
    if (!state.active_option || state.object_used ||
        state.active_option->kind != OptionTarget::Kind::kUse) {
      return;
    }
    GameObject* go = ObjectAccessor::GetGameObject(
        *bot, ParseSnapshotGuid(state.active_option->guid));
    if (go == nullptr || IsLootableObject(go) ||
        !go->IsAtInteractDistance(bot)) {
      return;
    }
    state.object_used = true;
    if (!UseObjectNow(bot, go)) {
      const std::string text =
          "can't use " + state.active_option->name + " now";
      state.active_option.reset();
      schedule_->SetActiveTargetPending(index, false);
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = text;
      failed.command_id = state.active_command_id;
      EmitAlertingEvent(index, name, bot, std::move(payload));
    }
  }

  SnapshotSchedule::Flags CurrentFlags(Player* bot, size_t index) {
    BotState* state = &bots_[index];
    bool pending = false;
    if (state->active_option) {
      pending =
          IsOptionTargetPending(bot, *state->active_option, state->object_used);
      if (!pending) {
        state->active_option.reset();
      }
    }
    return SnapshotSchedule::Flags{
        .in_combat = bot->IsInCombat(),
        .alive = bot->IsAlive(),
        .control_mask = ControlMask(bot),
        .attacker_count = static_cast<uint32_t>(bot->getAttackers().size()),
        .active_target_pending = pending,
    };
  }

  void EmitAlertingEvent(size_t index,
                         const std::string& name,
                         Player* bot,
                         wlb::snapshot::Event payload) {
    if (bot && bot->IsInWorld()) {
      const base::TimeTicks now = base::TimeTicks::Now();
      const SnapshotSchedule::Flags flags = CurrentFlags(bot, index);
      SendSnapshot(index, name, bot);
      schedule_->Sent(index, now, flags);
    }
    wlb::snapshot::ModuleMessage message;
    wlb::snapshot::ImmediateEvent& event = message.event.emplace();
    event.bot = name;
    event.seq = bots_[index].seq;
    event.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
    event.monotonic_us =
        (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
    event.event = std::move(payload);
    bridge_->Send(wlb::snapshot::WriteBeve(message));
  }

  void SendSnapshot(size_t index, const std::string& name, Player* bot) {
    BotState& state = bots_[index];
    if (!state.builder) {
      state.builder = std::make_unique<SnapshotBuilder>(NearbyBuilder::Options{
          config_.nameplate_range_yd, config_.los_raycasts_per_snapshot});
    }
    wlb::snapshot::ModuleMessage message;
    wlb::snapshot::Snapshot& snapshot = message.snapshot.emplace();
    snapshot.bot = name;
    snapshot.seq = ++state.seq;
    snapshot.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
    const base::TimeTicks start = base::TimeTicks::Now();
    snapshot.monotonic_us = (start - base::TimeTicks()).InMicroseconds();
    const NearbyBuilder::Stats nearby = state.builder->Build(bot, &snapshot);
    state.history.Add(snapshot.seq, start, AddOptions(&snapshot));
    std::string bytes = wlb::snapshot::WriteBeve(message);
    stats_.Add({base::TimeTicks::Now() - start, bytes.size(), nearby.units,
                nearby.objects, nearby.hidden, nearby.raycasts});
    // The newest snapshot of a bot wins if the harness is behind.
    bridge_->SendLatest(name, std::move(bytes));
  }

  Config config_;
  const base::raw_ptr<ChatInbox> inbox_;
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
    std::optional<OptionTarget> active_option;
    uint32_t active_command_id = 0;
    // A Use step's object was clicked on arrival (UseOnArrival).
    bool object_used = false;
  };
  std::vector<BotState> bots_;
  // How old each applied choice's snapshot was, since the last summary.
  std::vector<base::TimeDelta> apply_ages_;
  base::WeakPtrFactory<LlmBrainWorldScript> weak_factory_{this};
};

}  // namespace
}  // namespace llm_brain

void AddLlmBrainScripts() {
  // Shared by the scripts, which AzerothCore deletes at shutdown in no set
  // order: never freed, so it outlives them all.
  auto* inbox = new llm_brain::ChatInbox();
  new llm_brain::LlmBrainWorldScript(inbox);
  llm_brain::AddLlmBrainChatScripts(inbox);
}
