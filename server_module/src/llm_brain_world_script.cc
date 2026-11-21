// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Bag.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Playerbots.h"
#include "Position.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "World.h"
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
#include "options/act_executor.h"
#include "options/option_executor.h"
#include "options/option_facts.h"
#include "options/options.h"
#include "position_facts.h"
#include "snapshot/behavior_trace.h"
#include "snapshot/behavior_trace_scripts.h"
#include "snapshot/nearby_builder.h"
#include "snapshot/panels_builder.h"
#include "snapshot/snapshot.h"
#include "snapshot/snapshot_builder.h"
#include "snapshot/snapshot_schedule.h"
#include "snapshot/snapshot_stats.h"
#include "snapshot/ui_state.h"
#include "snapshot/world_util.h"

#if defined(LLM_BRAIN_TEST_HOOKS)
#include "test/facts_dump.h"
#include "test/fixture_loader.h"
#include "test/test_commands.h"
#endif

namespace llm_brain {
namespace {

constexpr size_t kMaxPendingMessages = 1000;
// How often the snapshot cost summary is logged.
constexpr uint32_t kStatsIntervalMs = 60000;
// trace_parity.py rejects traces whose median source rate falls below 3.5 Hz.
constexpr double kMinParityTraceHz = 3.5;
// How long a bot remembers its snapshots' options, so a choice made on a
// snapshot that is no longer the newest can still be resolved: well above an
// urgent wake-up on a busy GPU (p95 3.8 s, docs/results.md).
constexpr base::TimeDelta kOptionHistory = base::Seconds(30);

// How the body is doing with `target` (`used`: the object/NPC was already
// clicked or the destination reached, at once or on arrival): still at it,
// done, or cut short (`interrupted` says why; it is reported as the step's
// error, never as done: review of f40b0b6, finding 10).
struct TargetProgress {
  bool pending = false;
  std::string interrupted;
};

TargetProgress OptionTargetProgress(Player* bot,
                                    const OptionTarget& target,
                                    bool used,
                                    bool* had_food_drink_aura) {
  const auto pending = [] { return TargetProgress{.pending = true}; };
  const auto done = [] { return TargetProgress{}; };
  const auto cut = [](std::string why) {
    return TargetProgress{.interrupted = std::move(why)};
  };
  if (target.kind != OptionTarget::Kind::kKeep && !bot->IsAlive()) {
    return cut("you died");
  }
  switch (target.kind) {
    case OptionTarget::Kind::kAttack: {
      Unit* unit =
          ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(target.guid));
      if (unit == nullptr || !unit->IsInWorld()) {
        return cut(target.name + " is gone");
      }
      return unit->isDead() ? done() : pending();
    }
    case OptionTarget::Kind::kLoot: {
      if (used) {
        return done();
      }
      Creature* creature =
          ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(target.guid));
      if (creature == nullptr || !creature->IsInWorld() ||
          !creature->isDead() ||
          !creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)) {
        return cut("nothing left to loot on " + target.name);
      }
      return pending();
    }
    case OptionTarget::Kind::kGather:
    case OptionTarget::Kind::kUse: {
      GameObject* go =
          ObjectAccessor::GetGameObject(*bot, ParseSnapshotGuid(target.guid));
      if (go == nullptr || !go->IsInWorld() || !go->isSpawned()) {
        return used ? done() : cut(target.name + " is gone");
      }
      // A chest, herb or vein is done once opened (walking there until then);
      // anything else once clicked.
      const bool still =
          IsLootableObject(go) ? go->getLootState() == GO_READY : !used;
      if (still && !used && bot->IsInCombat()) {
        return cut("you are in combat");
      }
      return still ? pending() : done();
    }
    case OptionTarget::Kind::kTalk: {
      if (used) {
        return done();
      }
      if (bot->IsInCombat()) {
        return cut("you are in combat");
      }
      Creature* npc =
          ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(target.guid));
      if (npc == nullptr || !npc->IsInWorld() || !npc->IsAlive()) {
        return cut(target.name + " is gone");
      }
      return pending();
    }
    case OptionTarget::Kind::kRest:
      if (IsRestInProgress(bot, had_food_drink_aura)) {
        return pending();
      }
      return bot->IsInCombat() ? cut("you are in combat") : done();
    case OptionTarget::Kind::kExplore:
    case OptionTarget::Kind::kGoTo:
      if (used) {
        return done();
      }
      return bot->IsInCombat() ? cut("you are in combat") : pending();
    case OptionTarget::Kind::kKeep:
    case OptionTarget::Kind::kInvite:
    case OptionTarget::Kind::kAcceptInvite:
    case OptionTarget::Kind::kDeclineInvite:
    case OptionTarget::Kind::kLeaveGroup:
      return done();
  }
  return done();
}

// World-thread entry points for the brain bridge: every snapshot interval,
// send each attached bot's snapshot to the harness.
class LlmBrainWorldScript : public WorldScript {
 public:
  LlmBrainWorldScript(ChatInbox* inbox,
                      std::atomic<BehaviorTraceWriter*>* human_trace,
                      TraceHookState* trace_hooks)
      : WorldScript("LlmBrainWorldScript",
                    {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP,
                     WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN}),
        inbox_(inbox),
        human_trace_(human_trace),
        trace_hooks_(trace_hooks) {}

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
    // Before any bot logs in: a zone lookup per creature spawn takes seconds,
    // which must never happen on a snapshot.
    item_spell_texts_ = ItemSpellTexts::Load();
    LOG_INFO("module",
             "mod-llm-brain: {} item spell descriptions from Spell.dbc",
             item_spell_texts_ ? item_spell_texts_->size() : 0);
    const base::TimeTicks places_start = base::TimeTicks::Now();
    world_places_ = WorldPlaces::Build();
    LOG_INFO(
        "module",
        "mod-llm-brain: indexed {} grind spots and {} service hubs in {} ms",
        world_places_->buckets().size(), world_places_->hubs().size(),
        (base::TimeTicks::Now() - places_start).InMilliseconds());
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
    if (!config_.test_fixtures.empty() || !config_.test_facts_queries.empty()) {
      LOG_ERROR("module",
                "mod-llm-brain: LlmBrain.TestFixtures / TestFactsQueries "
                "ignored: production build (test builds: tools/wlb build "
                "server --test)");
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
    if (!config_.trace_dir.empty()) {
      trace_writer_ = std::make_unique<BehaviorTraceWriter>(
          config_.trace_dir,
          static_cast<uint64_t>(config_.trace_max_file_mb) * 1024 * 1024,
          static_cast<size_t>(config_.trace_max_rotated_files));
      if (human_trace_ && config_.observe_humans) {
        human_trace_->store(trace_writer_.get(), std::memory_order_release);
      }
      if (trace_hooks_) {
        trace_hooks_->bot_names = config_.bots;
        std::sort(trace_hooks_->bot_names.begin(),
                  trace_hooks_->bot_names.end());
        trace_hooks_->observe_humans = config_.observe_humans;
        trace_hooks_->writer.store(trace_writer_.get(),
                                   std::memory_order_release);
      }
      LOG_INFO("module",
               "mod-llm-brain: behavior tracing enabled (dir={}, "
               "observe_humans={})",
               config_.trace_dir, config_.observe_humans);
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
    // After startup (playerbots' travel caches are built by then).
    if (world_places_ && !config_.test_facts_queries.empty()) {
      std::string error;
      const int written =
          DumpFixtureFacts(*world_places_, config_.test_facts_queries, &error);
      if (written < 0) {
        LOG_ERROR("module", "mod-llm-brain: facts dump failed: {}", error);
      } else {
        LOG_INFO("module", "mod-llm-brain: facts dumped n={}", written);
      }
      config_.test_facts_queries.clear();
    }
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
    const base::TimeTicks now = base::TimeTicks::Now();
    if (trace_writer_ && trace_writer_->enabled()) {
      SampleObservedEntities(now);
    }
    if (!bridge_->connected()) {
      return;  // Nobody to send to: don't spend world-thread time building.
    }
    // Each bot has its own moment in the interval (bots are spread over it, so
    // a tick builds one or two snapshots, not all of them), and a bot whose
    // combat or life state changed is due at once.
    size_t built = 0;
    for (size_t i = 0; i < config_.bots.size(); ++i) {
      Player* bot = ObjectAccessor::FindPlayerByName(config_.bots[i]);
      if (!bot || !bot->IsInWorld()) {
        continue;  // Offline (or not in the world yet).
      }
      StepActiveOption(bot, i, config_.bots[i], now);
      CheckPendingGroupInvite(bot, i, config_.bots[i]);
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
    if (human_trace_) {
      human_trace_->store(nullptr, std::memory_order_release);
    }
    if (trace_hooks_) {
      trace_hooks_->writer.store(nullptr, std::memory_order_release);
    }
    CheckMeasuredTraceRate(/*min_intervals=*/5);
    trace_writer_.reset();
  }

 private:
  void CheckMeasuredTraceRate(size_t min_intervals) {
    if (!trace_writer_ || !trace_writer_->enabled()) {
      return;
    }
    if (const uint64_t untracked = trace_writer_->TakeUntrackedSampleChars();
        untracked > 0) {
      LOG_WARN("module",
               "mod-llm-brain: trace sample-rate tracker hit per-role active "
               "cap ({} samples from extra concurrent characters not "
               "rate-tracked)",
               untracked);
    }
    for (std::string_view role : {"bot", "human"}) {
      const std::optional<double> hz =
          trace_writer_->TakeMeasuredSampleHz(role, min_intervals);
      if (hz && *hz < kMinParityTraceHz) {
        LOG_WARN("module",
                 "mod-llm-brain: measured {} trace sample rate {:.2f} Hz is "
                 "below the 3.5 Hz that trace_parity.py requires "
                 "(TraceSampleIntervalMs = {} ms plus world-tick rounding)",
                 role, *hz, config_.trace_sample_interval_ms);
      }
    }
  }

  // The minute's summary: what snapshots cost to build, how many one tick
  // built at most (the spread of the bots), and how long they took to reach the
  // socket.
  void LogStats() {
    CheckMeasuredTraceRate(/*min_intervals=*/10);
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
    if (!path_times_.empty()) {
      std::sort(path_times_.begin(), path_times_.end());
      LOG_INFO("module",
               "mod-llm-brain: walk paths n={} (path_us p50={} max={})",
               path_times_.size(),
               path_times_[path_times_.size() / 2].InMicroseconds(),
               path_times_.back().InMicroseconds());
      path_times_.clear();
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
    wrong_version_logged_ = false;
    for (size_t i = 0; i < bots_.size(); ++i) {
      if (bots_[i].active_act) {
        if (Player* bot = ObjectAccessor::FindPlayerByName(config_.bots[i])) {
          CancelActiveAct(bot, &*bots_[i].active_act);
        }
        bots_[i].active_act.reset();
      }
      if (connected) {
        bots_[i].active_option.reset();
        bots_[i].active_command_id = 0;
        bots_[i].done_command_id = 0;
        bots_[i].done_act_command_id = 0;
        bots_[i].object_used = false;
        bots_[i].had_food_drink_aura = false;
        bots_[i].attack_idle_since = base::TimeTicks();
        bots_[i].walk = PendingWalk{};
        bots_[i].ui.Reset();
        schedule_->SetActiveTargetPending(i, false);
      }
    }
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

  static std::string TraceChannelName(wlb::snapshot::ChatChannel ch) {
    switch (ch) {
      case wlb::snapshot::WHISPER:
        return "whisper";
      case wlb::snapshot::SAY:
        return "say";
      case wlb::snapshot::YELL:
        return "yell";
      case wlb::snapshot::PARTY:
        return "party";
      case wlb::snapshot::RAID:
        return "raid";
      case wlb::snapshot::GENERAL:
        return "general";
      case wlb::snapshot::TRADE:
        return "trade";
      case wlb::snapshot::LFG:
        return "lfg";
      default:
        return "chat";
    }
  }

  // Chat lines (whisper, say, party, general) the bots heard since the last
  // update, sent to the harness as events.
  void DeliverChat() {
    if (const size_t dropped = inbox_->TakeDropped()) {
      LOG_WARN("module", "mod-llm-brain: {} chat lines dropped (inbox full)",
               dropped);
    }
    const base::TimeTicks now = base::TimeTicks::Now();
    for (ChatInbox::Line& line : inbox_->TakeAll()) {
      const size_t index = BotIndex(line.to);
      Player* bot = ObjectAccessor::FindPlayerByName(line.to);
      if (index == config_.bots.size() || !bot || !bot->IsInWorld() ||
          !bridge_->connected()) {
        continue;  // Nobody to tell: like a whisper while away.
      }
      const std::string ch_name = TraceChannelName(line.chat.channel);
      LOG_INFO("module", "mod-llm-brain: {} heard {} from {} (len={})", line.to,
               ch_name, line.chat.from, line.chat.text.size());
      if (!line.chat.from.empty() && line.chat.from != line.to) {
        auto& chatters = bots_[index].recent_chatters;
        std::erase_if(chatters, [&](const RecentChatter& rc) {
          return rc.name == line.chat.from;
        });
        chatters.push_front({line.chat.from, now});
        while (chatters.size() > 5) {
          chatters.pop_back();
        }
      }
      if (trace_writer_ && trace_writer_->enabled()) {
        TraceChat tc;
        tc.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
        tc.monotonic_us = (now - base::TimeTicks()).InMicroseconds();
        tc.role = "bot";
        tc.name = line.to;
        tc.direction = "in";
        tc.channel = ch_name;
        tc.peer = line.chat.from;
        tc.text_len = line.chat.text.size();
        trace_writer_->WriteChat(tc);
      }
      const bool alerting =
          line.chat.channel == wlb::snapshot::WHISPER ||
          line.chat.channel == wlb::snapshot::MONSTER_WHISPER ||
          line.chat.channel == wlb::snapshot::RAID_WARNING ||
          line.chat.channel == wlb::snapshot::BOSS_EMOTE || line.chat.at_me;
      wlb::snapshot::Event payload;
      payload.chat = std::move(line.chat);
      if (alerting) {
        EmitAlertingEvent(index, line.to, bot, std::move(payload));
      } else {
        SendImmediateEvent(index, line.to, std::move(payload));
      }
    }
  }

  void CheckPendingGroupInvite(Player* bot,
                               size_t index,
                               const std::string& name) {
    Group* invite = bot->GetGroupInvite();
    BotState& state = bots_[index];
    if (!invite) {
      state.seen_group_invite_leader.clear();
      return;
    }
    const std::string leader = invite->GetLeaderName();
    if (leader.empty() || leader == state.seen_group_invite_leader) {
      return;
    }
    state.seen_group_invite_leader = leader;
    LOG_INFO("module", "mod-llm-brain: {} received group invite from {}", name,
             leader);
    wlb::snapshot::Event payload;
    wlb::snapshot::PromptEvent& prompt = payload.prompt.emplace();
    prompt.kind = wlb::snapshot::PromptEvent::GROUP_INVITE;
    prompt.from = leader;
    EmitAlertingEvent(index, name, bot, std::move(payload));
  }

  // A command from the harness (docs/specs/snapshot.md §9): Choose and Say.
  void OnHarnessMessage(std::string bytes) {
    wlb::snapshot::Command command;
    if (!wlb::snapshot::ReadBeve(bytes, &command)) {
      LOG_ERROR("module", "mod-llm-brain: unreadable command ({} bytes)",
                bytes.size());
      return;
    }
    if (command.version != wlb::snapshot::kWireVersion) {
      // Once per connection: every command of that harness would say the same.
      if (!wrong_version_logged_) {
        wrong_version_logged_ = true;
        LOG_ERROR("module",
                  "mod-llm-brain: dropping commands from a harness of wire "
                  "version {} (this module: {}): rebuild and restart the one "
                  "that is out of date",
                  command.version, wlb::snapshot::kWireVersion);
      }
      return;
    }
    const std::string& name = command.bot;
    if (!command.choose && !command.say && !command.open && !command.close &&
        !command.hover && !command.act) {
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
    if (command.open || command.close || command.hover) {
      OnUiCommand(bot_index, name, command);
      return;
    }
    if (command.act) {
      OnAct(bot_index, name, bot, command.command_id, *command.act);
      return;
    }
    BotState& state = bots_[bot_index];
    const wlb::snapshot::Command::Choose& choose = *command.choose;
    const OptionHistory::Lookup found =
        state.history.Find(choose.seq, choose.option);
    // Copy the target by value before emitting any alerting event: emitting
    // builds a new snapshot and calls `state.history.Add(...)`, which can evict
    // the entry that `found.target` pointed into.
    const std::optional<OptionTarget> target =
        found.target ? std::optional<OptionTarget>(*found.target)
                     : std::nullopt;
    std::string error = found.error;
    bool used = false;
    std::string info;
    // Keep carries on with what the body is doing: its walk and its rest stay
    // as they are (a reset walk would send the bot towards (0, 0, 0)).
    const bool keep = target && target->kind == OptionTarget::Kind::kKeep;
    const bool attack = target && target->kind == OptionTarget::Kind::kAttack;
    if (target && !keep) {
      // Cancel an active movement act on a new valid non-Keep, non-Attack
      // choice (an expired/invalid choice must not cancel a boss dodge; a
      // replan that still fights keeps an in-flight `away` running).
      if (!attack && state.active_act) {
        const uint32_t cancelled_cmd_id = state.active_act->command_id;
        CancelActiveAct(bot, &*state.active_act);
        state.active_act.reset();
        wlb::snapshot::Event payload;
        wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
        failed.text = "act cancelled by new choice";
        failed.command_id = cancelled_cmd_id;
        EmitAlertingEvent(bot_index, name, bot, std::move(payload));
      }
      state.had_food_drink_aura = false;
      state.attack_idle_since = base::TimeTicks();
      state.walk = PendingWalk{};
    }
    if (target) {
      ExecuteResult result = ExecuteOption(bot, *target, &state.walk);
      error = std::move(result.error);
      used = result.used;
      info = std::move(result.info);
      if (!result.path_time.is_zero()) {
        path_times_.push_back(result.path_time);
      }
      if (target->kind == OptionTarget::Kind::kRest) {
        state.had_food_drink_aura = HasFoodOrDrinkAura(bot);
      }
    }
    // How old the picture was when the bot acted on it: the number that
    // matters (docs/specs/harness.md, "Age").
    const int64_t age_ms =
        target
            ? (base::TimeTicks::Now() - found.snapshot_built).InMilliseconds()
            : -1;
    if (target) {
      apply_ages_.push_back(base::Milliseconds(age_ms));
    }
    LOG_INFO("module",
             "mod-llm-brain: {} chose {} of snapshot {} ({} {}): {}, "
             "age_at_apply={} ms",
             name, choose.option, choose.seq,
             target ? OptionKindName(target->kind) : "-",
             target ? target->name : "", error.empty() ? "ok" : error, age_ms);
    if (!error.empty()) {
      state.active_option.reset();
      schedule_->SetActiveTargetPending(bot_index, false);
      // The bot sees it like the client's red error line; attach a fresh
      // snapshot first so the harness acts on current world state rather than a
      // snapshot up to 999 ms stale.
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = error;
      failed.command_id = command.command_id;
      EmitAlertingEvent(bot_index, name, bot, std::move(payload));
    } else if (keep) {
      // Keep: the body carries on, including a walk to an object it hasn't
      // clicked yet.
      state.done_command_id = command.command_id;
    } else if (target) {
      if (!info.empty()) {
        wlb::snapshot::Event payload;
        payload.info.emplace().text = std::move(info);
        SendImmediateEvent(bot_index, name, std::move(payload));
      }
      state.object_used = used;
      state.active_command_id = command.command_id;
      const TargetProgress progress =
          OptionTargetProgress(bot, *target, used, &state.had_food_drink_aura);
      if (progress.pending) {
        state.active_option = *target;
        schedule_->SetActiveTargetPending(bot_index, true);
      } else if (!progress.interrupted.empty()) {
        state.active_option = *target;
        FailActiveOption(bot_index, name, bot, progress.interrupted);
      } else {
        state.active_option.reset();
        state.done_command_id = command.command_id;
        schedule_->SetActiveTargetPending(bot_index, false);
      }
    }
  }

  void OnAct(size_t bot_index,
             const std::string& name,
             Player* bot,
             uint32_t command_id,
             const wlb::snapshot::Command::Act& act) {
    BotState& state = bots_[bot_index];
    if (state.active_act) {
      CancelActiveAct(bot, &*state.active_act);
      state.active_act.reset();
    }
    ActiveAct pending;
    ActStartResult res =
        StartAct(bot, command_id, act, base::TimeTicks::Now(), &pending);
    LOG_INFO("module", "mod-llm-brain: {} act {} ({}): {}", name,
             wlb::snapshot::EnumName(act.verb), act.name,
             res.error.empty() ? (res.done ? "done" : "running") : res.error);
    if (trace_writer_ && trace_writer_->enabled()) {
      const base::TimeTicks now = base::TimeTicks::Now();
      TraceEvent ev;
      ev.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
      ev.monotonic_us = (now - base::TimeTicks()).InMicroseconds();
      ev.role = "bot";
      ev.name = name;
      ev.event = "act_sent";
      ev.detail = std::string(wlb::snapshot::EnumName(act.verb));
      if (!act.name.empty()) {
        ev.detail += " " + act.name;
      }
      if (!res.error.empty()) {
        ev.detail += " [err: " + res.error + "]";
      }
      ev.value = static_cast<int64_t>(act.yd);
      trace_writer_->WriteEvent(ev);
    }
    if (!res.error.empty()) {
      schedule_->SetActiveTargetPending(bot_index,
                                        state.active_option.has_value());
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = std::move(res.error);
      failed.command_id = command_id;
      EmitAlertingEvent(bot_index, name, bot, std::move(payload));
    } else if (res.done) {
      state.done_act_command_id = command_id;
      schedule_->SetActiveTargetPending(bot_index,
                                        state.active_option.has_value());
    } else {
      state.active_act = std::move(pending);
      schedule_->SetActiveTargetPending(bot_index, true);
    }
  }

  void OnSay(size_t bot_index,
             const std::string& name,
             Player* bot,
             uint32_t /*command_id*/,
             const wlb::snapshot::Command::Say& say) {
    // Rate-limit outbound bot chat (max 5 lines per 10 s and min 1 s spacing)
    // to prevent spam loops while matching human chat cadence.
    const base::TimeTicks now = base::TimeTicks::Now();
    std::deque<base::TimeTicks>& sent_times = bots_[bot_index].recent_chat_sent;
    while (!sent_times.empty() &&
           now - sent_times.front() > base::Seconds(10)) {
      sent_times.pop_front();
    }
    std::string error;
    if (!sent_times.empty() && (sent_times.size() >= 5 ||
                                now - sent_times.back() < base::Seconds(1))) {
      error = "chat rate limited";
    } else {
      error = Speak(bot, say);
      if (error.empty()) {
        sent_times.push_back(now);
      }
    }
    LOG_INFO("module", "mod-llm-brain: {} says ({}{}{}, len={}): {}", name,
             wlb::snapshot::EnumName(say.channel), say.to.empty() ? "" : " to ",
             say.to, say.text.size(), error.empty() ? "ok" : error);
    if (error.empty() && trace_writer_ && trace_writer_->enabled()) {
      TraceChat tc;
      tc.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
      tc.monotonic_us = (now - base::TimeTicks()).InMicroseconds();
      tc.role = "bot";
      tc.name = name;
      tc.direction = "out";
      tc.channel = TraceChannelName(say.channel);
      tc.peer = say.to;
      tc.text = say.text;
      trace_writer_->WriteChat(tc);
    }
    if (!error.empty()) {
      // Chat error (command_id = 0): recorded in recent_events without
      // cancelling the active step or building an extra snapshot.
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = error;
      SendImmediateEvent(bot_index, name, std::move(payload));
    }
  }

  // open / close / hover (docs/specs/options.md): the next snapshot
  // shows the panel or tooltip. A refusal comes back as a non-step error event.
  void OnUiCommand(size_t bot_index,
                   const std::string& name,
                   const wlb::snapshot::Command& command) {
    UiState& ui = bots_[bot_index].ui;
    std::string error;
    if (command.open) {
      error = ui.Open(command.open->panel);
    } else if (command.close) {
      ui.Close(command.close->panel);
    } else if (command.hover) {
      error = command.hover->ref
                  ? ui.Hover(*command.hover->ref, base::TimeTicks::Now())
                  : "nothing to point at";
    }
    if (error.empty()) {
      return;
    }
    LOG_INFO("module", "mod-llm-brain: {}: {}", name, error);
    wlb::snapshot::Event payload;
    payload.error.emplace().text = std::move(error);
    SendImmediateEvent(bot_index, name, std::move(payload));
  }

  // Advances an active multi-tick option:
  // - Gather / Use: clicks the object once in range;
  // - Talk: talks to the NPC once in range;
  // - Explore / GoTo: steps along the mmap path ~70 yd at a time.
  void StepActiveOption(Player* bot,
                        size_t index,
                        const std::string& name,
                        base::TimeTicks now) {
    BotState& state = bots_[index];
    ClearStaleCombatTarget(bot);
    if (state.active_act) {
      ActStepResult res = StepActiveAct(bot, now, &*state.active_act);
      if (!res.error.empty()) {
        const uint32_t cmd_id = state.active_act->command_id;
        state.active_act.reset();
        schedule_->SetActiveTargetPending(index,
                                          state.active_option.has_value());
        wlb::snapshot::Event payload;
        wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
        failed.text = std::move(res.error);
        failed.command_id = cmd_id;
        EmitAlertingEvent(index, name, bot, std::move(payload));
      } else if (res.done) {
        state.done_act_command_id = state.active_act->command_id;
        state.active_act.reset();
      } else {
        // An active standing-order act owns movement/casting until it finishes
        // or fails: pause any overlapping plan-option walk/approach so the two
        // don't fight over MovePoint.
        return;
      }
    }
    if (!state.active_option || state.object_used) {
      return;
    }
    const OptionTarget::Kind kind = state.active_option->kind;
    if (kind == OptionTarget::Kind::kAttack) {
      Unit* unit = ObjectAccessor::GetUnit(
          *bot, ParseSnapshotGuid(state.active_option->guid));
      if (!unit || !unit->IsInWorld() || unit->isDead()) {
        return;
      }
      const AttackStepAction action = CheckAttackStep(
          bot->IsInCombat(), bot->IsNonMeleeSpellCast(false), bot->isMoving(),
          bot->GetDistance(unit), bot->IsWithinLOSInMap(unit), now,
          &state.attack_idle_since);
      switch (action) {
        case AttackStepAction::kActive:
          return;
        case AttackStepAction::kEngageWhileMoving:
          if (now >= state.next_rest_try) {
            state.next_rest_try = now + base::Seconds(1);
            ExecuteOption(bot, *state.active_option);
          }
          return;
        case AttackStepAction::kStuck:
          FailActiveOption(index, name, bot,
                           "can't reach " + state.active_option->name);
          return;
        case AttackStepAction::kRetryWalk:
          if (now >= state.next_rest_try) {
            state.next_rest_try = now + base::Seconds(2);
            WalkTowards(bot, unit->GetPositionX(), unit->GetPositionY(),
                        unit->GetPositionZ());
          }
          return;
        case AttackStepAction::kRetryExecute:
          if (now >= state.next_rest_try) {
            state.next_rest_try = now + base::Seconds(2);
            ExecuteOption(bot, *state.active_option);
          }
          return;
      }
    }
    if (kind == OptionTarget::Kind::kRest) {
      if (now >= state.next_rest_try) {
        state.next_rest_try = now + base::Seconds(1);
        ContinueRest(bot);
      }
      return;
    }
    if (kind == OptionTarget::Kind::kLoot) {
      Creature* creature = ObjectAccessor::GetCreature(
          *bot, ParseSnapshotGuid(state.active_option->guid));
      if (creature == nullptr) {
        return;
      }
      if (!IsInLootRange(bot, creature)) {
        if (!bot->isMoving() && now >= state.next_rest_try) {
          state.next_rest_try = now + base::Seconds(1);
          if (!WalkTowards(bot, creature->GetPositionX(),
                           creature->GetPositionY(),
                           creature->GetPositionZ())) {
            FailActiveOption(index, name, bot,
                             "no path to " + state.active_option->name);
          }
        }
        return;
      }
      state.object_used = true;
      if (std::string text = LootCorpseNow(bot, creature); !text.empty()) {
        LOG_INFO("module", "mod-llm-brain: {} can't loot {} on arrival: {}",
                 name, state.active_option->name, text);
        FailActiveOption(index, name, bot, std::move(text));
      }
      return;
    }
    if (kind == OptionTarget::Kind::kUse ||
        kind == OptionTarget::Kind::kGather) {
      GameObject* go = ObjectAccessor::GetGameObject(
          *bot, ParseSnapshotGuid(state.active_option->guid));
      if (go == nullptr) {
        return;
      }
      if (!IsInUseRange(bot, go)) {
        if (!bot->isMoving() && now >= state.next_rest_try) {
          state.next_rest_try = now + base::Seconds(1);
          if (!WalkTowards(bot, go->GetPositionX(), go->GetPositionY(),
                           go->GetPositionZ())) {
            FailActiveOption(index, name, bot,
                             "no path to " + state.active_option->name);
          }
        }
        return;
      }
      state.object_used = true;
      if (std::string text = UseObjectNow(bot, go); !text.empty()) {
        LOG_INFO("module", "mod-llm-brain: {} can't use {} on arrival: {}",
                 name, state.active_option->name, text);
        FailActiveOption(index, name, bot, std::move(text));
      }
      return;
    }
    if (kind == OptionTarget::Kind::kTalk) {
      Creature* npc = ObjectAccessor::GetCreature(
          *bot, ParseSnapshotGuid(state.active_option->guid));
      if (npc == nullptr) {
        return;
      }
      if (!IsInTalkRange(bot, npc)) {
        if (!bot->isMoving() && now >= state.next_rest_try) {
          state.next_rest_try = now + base::Seconds(1);
          if (!WalkTowards(bot, npc->GetPositionX(), npc->GetPositionY(),
                           npc->GetPositionZ())) {
            FailActiveOption(index, name, bot,
                             "no path to " + state.active_option->name);
          }
        }
        return;
      }
      state.object_used = true;
      TalkResult res = TalkToNpcNow(bot, npc);
      if (!res.error.empty()) {
        LOG_INFO("module", "mod-llm-brain: {} can't talk to {} on arrival: {}",
                 name, state.active_option->name, res.error);
        FailActiveOption(index, name, bot, std::move(res.error));
      } else if (!res.info.empty()) {
        LOG_INFO("module", "mod-llm-brain: {}: {}", name, res.info);
        wlb::snapshot::Event payload;
        payload.info.emplace().text = std::move(res.info);
        SendImmediateEvent(index, name, std::move(payload));
      }
      return;
    }
    if (kind == OptionTarget::Kind::kExplore ||
        kind == OptionTarget::Kind::kGoTo) {
      const WalkStepResult step = TickLongWalk(bot, now, &state.walk);
      if (!step.path_time.is_zero()) {
        path_times_.push_back(step.path_time);
      }
      if (!step.error.empty()) {
        LOG_INFO("module", "mod-llm-brain: {} walk to {} failed: {}", name,
                 state.active_option->name, step.error);
        FailActiveOption(index, name, bot, std::move(step.error));
        return;
      }
      if (step.arrived) {
        state.object_used = true;
      }
    }
  }

  void FailActiveOption(size_t index,
                        const std::string& name,
                        Player* bot,
                        std::string error) {
    BotState& state = bots_[index];
    const uint32_t command_id = state.active_command_id;
    state.active_option.reset();
    schedule_->SetActiveTargetPending(index, false);
    wlb::snapshot::Event payload;
    wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
    failed.text = std::move(error);
    failed.command_id = command_id;
    EmitAlertingEvent(index, name, bot, std::move(payload));
  }

  SnapshotSchedule::Flags CurrentFlags(Player* bot, size_t index) {
    BotState* state = &bots_[index];
    bool pending = false;
    if (state->active_option) {
      const TargetProgress progress =
          OptionTargetProgress(bot, *state->active_option, state->object_used,
                               &state->had_food_drink_aura);
      pending = progress.pending;
      if (!progress.interrupted.empty()) {
        LOG_INFO("module", "mod-llm-brain: {}: {} interrupted: {}",
                 bot->GetName(), state->active_option->name,
                 progress.interrupted);
        FailActiveOption(index, bot->GetName(), bot, progress.interrupted);
      } else if (!pending) {
        state->active_option.reset();
        state->done_command_id = state->active_command_id;
      }
    }
    const bool reactive =
        state->seq > 0 && HasNewCastOrAura(bot, state->last_reactive);
    return SnapshotSchedule::Flags{
        .in_combat = bot->IsInCombat(),
        .alive = bot->IsAlive(),
        .control_mask = ControlMask(bot),
        .attacker_count = static_cast<uint32_t>(bot->getAttackers().size()),
        .active_target_pending = pending || state->active_act.has_value(),
        .reactive_event = reactive,
    };
  }

  void SendImmediateEvent(size_t index,
                          const std::string& name,
                          wlb::snapshot::Event payload) {
    wlb::snapshot::ModuleMessage message;
    message.version = wlb::snapshot::kWireVersion;
    wlb::snapshot::ImmediateEvent& event = message.event.emplace();
    event.bot = name;
    event.seq = bots_[index].seq;
    event.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
    event.monotonic_us =
        (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
    event.event = std::move(payload);
    // Flush any pending snapshot for `name` first so an alerting event never
    // overtakes the snapshot sent right before it when the socket backs up.
    bridge_->Send(wlb::snapshot::WriteBeve(message), name);
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
    SendImmediateEvent(index, name, std::move(payload));
  }

  void SendSnapshot(size_t index, const std::string& name, Player* bot) {
    BotState& state = bots_[index];
    if (!state.builder) {
      state.builder = std::make_unique<SnapshotBuilder>(NearbyBuilder::Options{
          config_.nameplate_range_yd, config_.los_raycasts_per_snapshot});
    }
    wlb::snapshot::ModuleMessage message;
    message.version = wlb::snapshot::kWireVersion;
    wlb::snapshot::Snapshot& snapshot = message.snapshot.emplace();
    snapshot.bot = name;
    snapshot.seq = ++state.seq;
    snapshot.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
    const base::TimeTicks start = base::TimeTicks::Now();
    snapshot.monotonic_us = (start - base::TimeTicks()).InMicroseconds();
    snapshot.done_command_id = state.done_command_id;
    snapshot.done_act_command_id = state.done_act_command_id;
    OptionFacts facts;
    const NearbyBuilder::Stats nearby =
        state.builder->Build(bot, &snapshot, &facts, world_places_.get(),
                             &state.ui, item_spell_texts_.get(), start);
    if (Group* invite = bot->GetGroupInvite()) {
      facts.pending_group_invite_from = invite->GetLeaderName();
    }
    Group* cur_group = bot->GetGroup();
    facts.can_leave_group = (cur_group != nullptr && !bot->IsInCombat());
    const bool can_invite =
        !bot->IsInCombat() &&
        (!cur_group || ((cur_group->IsLeader(bot->GetGUID()) ||
                         cur_group->IsAssistant(bot->GetGUID())) &&
                        !cur_group->IsFull()));
    if (can_invite) {
      static constexpr base::TimeDelta kChatterMemory = base::Minutes(5);
      for (const RecentChatter& rc : state.recent_chatters) {
        if (start - rc.at > kChatterMemory ||
            facts.invite_candidates.size() >= 3) {
          break;
        }
        Player* other = ObjectAccessor::FindPlayerByName(rc.name);
        if (!other || !other->IsInWorld() || other == bot ||
            other->GetTeamId() != bot->GetTeamId() ||
            other->GetGroup() != nullptr) {
          continue;
        }
        facts.invite_candidates.push_back(rc.name);
      }
    }
    state.history.Add(snapshot.seq, start, AddOptions(&snapshot, facts));
    std::string bytes = wlb::snapshot::WriteBeve(message);
    stats_.Add({base::TimeTicks::Now() - start, bytes.size(), nearby.units,
                nearby.objects, nearby.hidden, nearby.raycasts});
    // The newest snapshot of a bot wins if the harness is behind.
    bridge_->SendLatest(name, std::move(bytes));
    state.last_reactive = CollectReactiveState(bot);
  }

  // Samples both observed humans and tracked bots through the exact same
  // observer-view path at 5 Hz (200 ms) plus movement-packet / combat edges so
  // downstream trajectory reconstruction sees identical timing and schema
  // (docs/plans/self-improving-loop.md, section F).
  static TraceSample BuildObservedSample(Player* player,
                                         std::string_view role,
                                         int64_t time_ms,
                                         int64_t mono_us) {
    TraceSample s;
    s.time_ms = time_ms;
    s.monotonic_us = mono_us;
    s.role = std::string(role);
    s.name = player->GetName();
    s.map_id = player->GetMapId();
    s.instance_id = player->GetInstanceId();
    if (const AreaTableEntry* area =
            sAreaTableStore.LookupEntry(player->GetZoneId())) {
      const char* zname = area->area_name[sWorld->GetDefaultDbcLocale()];
      if (!zname || zname[0] == '\0') {
        zname = area->area_name[0];
      }
      if (zname) {
        s.zone = zname;
      }
    }
    s.in_town = player->HasPlayerFlag(PLAYER_FLAGS_RESTING);
    s.in_group = player->GetGroup() != nullptr;
    s.in_dungeon = player->GetMap() != nullptr && player->GetMap()->IsDungeon();
    s.x = player->GetPositionX();
    s.y = player->GetPositionY();
    s.z = player->GetPositionZ();
    s.orientation = player->GetOrientation();
    s.moving = player->isMoving();
    s.jumping = player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING);
    s.mounted = player->IsMounted();
    s.swimming = player->IsInWater();
    s.sitting = player->getStandState() != UNIT_STAND_STATE_STAND;
    s.on_taxi = player->IsInFlight();
    s.in_combat = player->IsInCombat();
    s.dead = !player->IsAlive();
    s.cc_mask = ControlMask(player);
    uint32_t free_slots = 0;
    for (uint8_t slot = INVENTORY_SLOT_ITEM_START;
         slot < INVENTORY_SLOT_ITEM_END; ++slot) {
      if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) {
        ++free_slots;
      }
    }
    for (uint8_t i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END;
         ++i) {
      if (const Bag* bag = player->GetBagByPos(i); IsGeneralBag(bag)) {
        free_slots += bag->GetFreeSlots();
      }
    }
    s.bag_free_slots = free_slots;
    if (Pet* pet = player->GetPet();
        pet && pet->IsAlive() && pet->GetMaxHealth() > 0) {
      s.pet_hp_pct = 100.0f * static_cast<float>(pet->GetHealth()) /
                     static_cast<float>(pet->GetMaxHealth());
    }
    s.level = player->GetLevel();
    s.xp = player->GetUInt32Value(PLAYER_XP);
    if (player->GetMaxHealth() > 0) {
      s.hp_pct = 100.0f * static_cast<float>(player->GetHealth()) /
                 static_cast<float>(player->GetMaxHealth());
    }
    if (player->GetMaxPower(POWER_MANA) > 0) {
      s.mana_pct = 100.0f * static_cast<float>(player->GetPower(POWER_MANA)) /
                   static_cast<float>(player->GetMaxPower(POWER_MANA));
    }
    Spell* sp = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!sp || sp->getState() != SPELL_STATE_PREPARING) {
      sp = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    }
    if (sp && sp->GetSpellInfo()) {
      const char* sn =
          sp->GetSpellInfo()->SpellName[sWorld->GetDefaultDbcLocale()];
      if (!sn || sn[0] == '\0') {
        sn = sp->GetSpellInfo()->SpellName[0];
      }
      if (sn) {
        s.casting = sn;
      }
    }
    if (Unit* target = player->GetSelectedUnit()) {
      s.target = target->GetName();
      s.target_hostile = !player->IsFriendlyTo(target) && target->IsAlive();
      if (s.target_hostile) {
        s.target_dist = player->GetDistance(target);
        s.target_x = target->GetPositionX();
        s.target_y = target->GetPositionY();
        if (target->GetMaxHealth() > 0) {
          s.target_hp_pct = 100.0f * static_cast<float>(target->GetHealth()) /
                            static_cast<float>(target->GetMaxHealth());
        }
        s.target_level = target->GetLevel();
        Spell* tsp = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!tsp || tsp->getState() != SPELL_STATE_PREPARING) {
          tsp = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        }
        if (tsp && tsp->GetSpellInfo()) {
          const SpellInfo* tinfo = tsp->GetSpellInfo();
          const char* tsn = tinfo->SpellName[sWorld->GetDefaultDbcLocale()];
          if (!tsn || tsn[0] == '\0') {
            tsn = tinfo->SpellName[0];
          }
          if (tsn) {
            s.target_casting = tsn;
          }
          if (tsp->m_targets.HasDst()) {
            if (const WorldLocation* dst = tsp->m_targets.GetDstPos()) {
              s.target_x = dst->GetPositionX();
              s.target_y = dst->GetPositionY();
            }
          }
          float max_r = 0.0f;
          for (uint8 eff = 0; eff < MAX_SPELL_EFFECTS; ++eff) {
            const float r = tinfo->Effects[eff].CalcRadius(target, tsp);
            if (r > max_r) {
              max_r = r;
            }
          }
          if (max_r > 0.0f) {
            s.target_cast_radius = max_r;
          }
        }
        s.target_moving = target->isMoving();
        s.target_targeting_me = target->GetTarget() == player->GetGUID() ||
                                target->GetVictim() == player;
        s.target_in_combat = target->IsInCombat();
      }
    }
    s.attackers = static_cast<int>(player->getAttackers().size());
    return s;
  }

  void SampleOneObserved(Player* player,
                         std::string_view role,
                         base::TimeTicks now,
                         int64_t time_ms,
                         int64_t mono_us) {
    const base::TimeDelta trace_interval =
        base::Milliseconds(config_.trace_sample_interval_ms);
    const uint64_t key = player->GetGUID().GetRawValue();
    ObservedState& obs = observed_state_[key];
    const bool in_combat = player->IsInCombat();
    const bool jumping = player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING);

    if (obs.initialized && in_combat != obs.in_combat) {
      TraceEvent ev;
      ev.time_ms = time_ms;
      ev.monotonic_us = mono_us;
      ev.role = std::string(role);
      ev.name = player->GetName();
      ev.event = in_combat ? "combat_start" : "combat_end";
      ev.value = static_cast<int64_t>(player->getAttackers().size());
      trace_writer_->WriteEvent(ev);
    }

    // Use the exact same cadence + combat/jump edge triggers for both humans
    // and bots — never emit extra samples on human-only movement packets, which
    // would leak packet detail into downstream parity checks.
    const bool state_edge = !obs.initialized || in_combat != obs.in_combat ||
                            jumping != obs.jumping;
    const bool interval_due =
        !obs.initialized || (now - obs.last_sample) >= trace_interval;

    if (!state_edge && !interval_due) {
      return;
    }

    obs.initialized = true;
    obs.last_sample = now;
    obs.in_combat = in_combat;
    obs.jumping = jumping;
    trace_writer_->WriteSample(
        BuildObservedSample(player, role, time_ms, mono_us));
  }

  void SampleObservedEntities(base::TimeTicks now) {
    const int64_t time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
    const int64_t mono_us = (now - base::TimeTicks()).InMicroseconds();
    for (const std::string& bot_name : config_.bots) {
      Player* bot = ObjectAccessor::FindPlayerByName(bot_name);
      if (bot && bot->IsInWorld()) {
        SampleOneObserved(bot, "bot", now, time_ms, mono_us);
      }
    }
    if (!config_.observe_humans) {
      return;
    }
    for (const auto& [guid, player] : ObjectAccessor::GetPlayers()) {
      if (!player || !player->IsInWorld() ||
          GET_PLAYERBOT_AI(player) != nullptr ||
          BotIndex(player->GetName()) != config_.bots.size()) {
        continue;
      }
      SampleOneObserved(player, "human", now, time_ms, mono_us);
    }
  }

  Config config_;
  const base::raw_ptr<ChatInbox> inbox_;
  const base::raw_ptr<std::atomic<BehaviorTraceWriter*>> human_trace_;
  const base::raw_ptr<TraceHookState> trace_hooks_;
#if defined(LLM_BRAIN_TEST_HOOKS)
  std::unique_ptr<FixtureLoader> fixture_loader_;
#endif
  // The client's text of item spells (tooltips), read from Spell.dbc.
  std::unique_ptr<ItemSpellTexts> item_spell_texts_;
  // Grind spots and service hubs, indexed at startup (Explore / Go to).
  std::unique_ptr<WorldPlaces> world_places_;
  std::unique_ptr<BotManager> bot_manager_;
  std::unique_ptr<BrainBridge> bridge_;
  std::unique_ptr<SnapshotSchedule> schedule_;
  std::unique_ptr<BehaviorTraceWriter> trace_writer_;
  struct ObservedState {
    bool initialized = false;
    base::TimeTicks last_sample;
    bool in_combat = false;
    bool jumping = false;
  };
  std::unordered_map<uint64_t, ObservedState> observed_state_;
  uint32 since_stats_ms_ = 0;
  size_t max_update_snapshots_ = 0;
  base::TimeDelta max_update_time_;
  SnapshotStats stats_;
  struct RecentChatter {
    std::string name;
    base::TimeTicks at;
  };
  struct BotState {
    // Snapshot counter (gaps tell the harness something was dropped).
    uint32_t seq = 0;
    std::unique_ptr<SnapshotBuilder> builder;
    OptionHistory history{kOptionHistory};
    std::optional<OptionTarget> active_option;
    uint32_t active_command_id = 0;
    // The last command the body finished (Snapshot::done_command_id /
    // done_act_command_id), kept separate so option and act completions never
    // overwrite each other.
    uint32_t done_command_id = 0;
    uint32_t done_act_command_id = 0;
    // Gather / Use / Talk / Explore / GoTo: finished at once or on arrival.
    bool object_used = false;
    // Rest: whether a food or drink aura was observed while resting.
    bool had_food_drink_aura = false;
    // What the bot has open and points at (open / close / hover).
    UiState ui;
    // Rest: when ContinueRest may next try the other of eat / drink.
    base::TimeTicks next_rest_try;
    // Attack: when the bot first stood idle (not in combat/moving/casting)
    // while trying to engage an active kAttack target.
    base::TimeTicks attack_idle_since;
    // Explore / GoTo: active stepped mmap walk state.
    PendingWalk walk;
    // Active standing-order act (to / away / cast-time use).
    std::optional<ActiveAct> active_act;
    // Casts and auras on bot/target/attackers at the last sent snapshot.
    ReactiveSnapshotState last_reactive;
    // Players who recently chatted to/near this bot (for group invite options).
    std::deque<RecentChatter> recent_chatters;
    // Timestamps of recent outbound bot chat messages (for rate limiting).
    std::deque<base::TimeTicks> recent_chat_sent;
    // Leader name of the currently surfaced pending group invite.
    std::string seen_group_invite_leader;
  };
  std::vector<BotState> bots_;
  // A wrong-version command was logged on this connection.
  bool wrong_version_logged_ = false;
  // How old each applied choice's snapshot was, since the last summary.
  std::vector<base::TimeDelta> apply_ages_;
  // Time spent in PathGenerator per ~70 yd walk step, since the last summary.
  std::vector<base::TimeDelta> path_times_;
  base::WeakPtrFactory<LlmBrainWorldScript> weak_factory_{this};
};

}  // namespace
}  // namespace llm_brain

void AddLlmBrainScripts() {
  // Shared by the scripts, which AzerothCore deletes at shutdown in no set
  // order: never freed, so it outlives them all.
  auto* inbox = new llm_brain::ChatInbox();
  auto* human_trace = new std::atomic<llm_brain::BehaviorTraceWriter*>(nullptr);
  auto* trace_hooks = new llm_brain::TraceHookState();
  new llm_brain::LlmBrainWorldScript(inbox, human_trace, trace_hooks);
  llm_brain::AddLlmBrainChatScripts(inbox, human_trace);
  llm_brain::AddLlmBrainTraceScripts(trace_hooks);
#if defined(LLM_BRAIN_TEST_HOOKS)
  llm_brain::AddLlmBrainTestCommandScripts();
#endif
}
