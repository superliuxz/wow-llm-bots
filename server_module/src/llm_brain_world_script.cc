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

#include "Creature.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "Event.h"
#include "GameObject.h"
#include "Group.h"
#include "LFG.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Position.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "bot_manager/bot_manager.h"
#include "bot_manager/character_loader.h"
#include "bot_manager/errand_hold.h"
#include "bot_manager/popup_watch.h"
#include "bot_manager/pull_discipline.h"
#include "bot_session.h"
#include "bridge/brain_bridge.h"
#include "chat/chat_inbox.h"
#include "chat/chat_scripts.h"
#include "chat/chat_text.h"
#include "chat/leader_command.h"
#include "chat/speak.h"
#include "config/llm_brain_config.h"
#include "net/frame_codec.h"
#include "options/act_executor.h"
#include "options/act_geometry.h"
#include "options/action_types.h"
#include "options/exec_items.h"
#include "options/exec_movement.h"
#include "options/exec_npc.h"
#include "options/exec_shared.h"
#include "options/exec_social.h"
#include "options/group_roles.h"
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
#include "test/test_commands.h"
#endif

namespace llm_brain {
namespace {

constexpr size_t kMaxPendingMessages = 1000;
// How often the snapshot cost summary is logged.
constexpr uint32_t kStatsIntervalMs = 60000;
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
                                    bool* had_food_drink_aura,
                                    uint32_t pending_item_spell_id = 0) {
  const auto pending = [] { return TargetProgress{.pending = true}; };
  const auto done = [] { return TargetProgress{}; };
  const auto cut = [](std::string why) {
    return TargetProgress{.interrupted = std::move(why)};
  };
  if (IsDeadAllowedAction(target.action) && !IsKeepAction(target.action)) {
    return done();
  }
  if (!IsKeepAction(target.action) && !bot->IsAlive()) {
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
      if (IsPendingCastAction(target.action)) {
        if (used) {
          return done();
        }
        return IsSpellCastPending(bot, pending_item_spell_id) ? pending()
                                                              : done();
      }
      if (!IsGameObjectAction(target.action)) {
        return done();
      }
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
    std::vector<CharacterLoader::Fixture> characters;
#if defined(LLM_BRAIN_TEST_HOOKS)
    for (const auto& [character, file] : config_.test_fixtures) {
      characters.push_back({character, file});
    }
#else
    if (!config_.test_fixtures.empty() || !config_.test_facts_queries.empty()) {
      LOG_ERROR("module",
                "mod-llm-brain: LlmBrain.TestFixtures / TestFactsQueries "
                "ignored: production build (test builds: tools/wlb build "
                "server --test)");
    }
#endif
    for (const auto& [character, spec] : config_.create_bots) {
      characters.push_back({character, spec, /*create_only=*/true});
    }
    if (!characters.empty()) {
      character_loader_ = std::make_unique<CharacterLoader>(
          config_.test_account, std::move(characters));
    }
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
                                weak_factory_.GetWeakPtr())},
        ServerHelloFrame());
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
    // Bot characters are created (and test fixtures reset) before the bot
    // manager logs anyone in.
    if (character_loader_ && character_loader_->Update()) {
      character_loader_.reset();
    }
    const bool characters_pending = character_loader_ != nullptr;
#if defined(LLM_BRAIN_TEST_HOOKS)
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
#endif
    if (bot_manager_ && !characters_pending) {
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
    if (!bridge_->connected() || harness_refused_) {
      return;  // Nobody to send to: don't spend world-thread time building.
    }
    // Each bot has its own moment in the interval (bots are spread over it, so
    // a tick builds one or two snapshots, not all of them), and a bot whose
    // combat or life state changed is due at once.
    size_t built = 0;
    for (size_t i = 0; i < config_.bots.size(); ++i) {
      Player* bot = ObjectAccessor::FindPlayerByName(config_.bots[i]);
      if (!IsControlledBot(bot)) {
        ClearRtiOverride(bots_[i].rti_override);
        ClearStopDps(bots_[i].stop_dps);
        ClearHealerManaWait(bots_[i].healer_mana_wait);
        ClearLeaderWait(bots_[i].leader_wait);
        continue;  // Offline, not in world, or logged in by a real human.
      }
      // Clear any active act or saved `back` coordinates if the bot changed
      // maps/instances so old-map coordinates are never reused (A-L1).
      const uint32_t cur_map = bot->GetMapId();
      const uint32_t cur_inst = bot->GetInstanceId();
      if (bots_[i].has_last_map && (bots_[i].last_map_id != cur_map ||
                                    bots_[i].last_instance_id != cur_inst)) {
        if (bots_[i].active_act) {
          CancelActiveAct(bot, &*bots_[i].active_act);
          bots_[i].active_act.reset();
        }
        ResetRtiOverride(bot, bots_[i].rti_override);
        ResetStopDps(bot, bots_[i].stop_dps);
        ClearHealerManaWait(bots_[i].healer_mana_wait);
        if (ClearLeaderWait(bots_[i].leader_wait)) {
          BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
          if (policy.stay) {
            policy.stay = false;
            BotManager::SetGroupPolicy(bot, policy);
          }
        }
        bots_[i].errand_hold_active = false;
        BotSessionRegistry::Instance().ResetOnMapChange(
            bot->GetGUID().GetRawValue());
      }
      bots_[i].has_last_map = true;
      bots_[i].last_map_id = cur_map;
      bots_[i].last_instance_id = cur_inst;
      StepActiveOption(bot, i, config_.bots[i], now);
      SyncMovingOut(bot, i);
      SyncActStealth(bot, i);
      if (!bot->IsInCombat()) {
        ClearAvoidedTargetsForBot(bot->GetGUID().GetRawValue());
      }
      if (bots_[i].rti_override.active) {
        Unit* marked =
            bots_[i].rti_override.target_guid != 0
                ? ObjectAccessor::GetUnit(
                      *bot, ObjectGuid(bots_[i].rti_override.target_guid))
                : nullptr;
        const RtiOverrideTickInput rti_in{
            .bot_in_combat = bot->IsInCombat(),
            .party_in_combat = IsPartyInCombat(bot),
            .target_alive_and_in_world =
                marked != nullptr && marked->IsInWorld() && marked->IsAlive(),
        };
        if (ShouldResetRtiOverride(bots_[i].rti_override, rti_in)) {
          ApplyDefaultRti(bot);
          LOG_INFO("module", "mod-llm-brain: {}: combat ended, reset rti to {}",
                   bot->GetName(), kDefaultRti);
        }
      }
      if (bots_[i].stop_dps.active &&
          ShouldResetStopDps(bots_[i].stop_dps, bot->IsInCombat(),
                             IsPartyInCombat(bot))) {
        BotManager::SetStopDps(bot, false);
        LOG_INFO("module", "mod-llm-brain: {}: combat ended, cleared stop dps",
                 bot->GetName());
      }
      if (bots_[i].leader_wait.active) {
        BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
        if (ShouldExpireLeaderWait(
                bots_[i].leader_wait, bot->GetGroup() != nullptr, policy.stay,
                bot->IsInCombat(), IsPartyInCombat(bot), now)) {
          policy.stay = false;
          BotManager::SetGroupPolicy(bot, policy);
          LOG_INFO("module",
                   "mod-llm-brain: {}: leader wait timed out, resuming follow",
                   bot->GetName());
        }
      }
      CheckPendingGroupInvite(bot, i, config_.bots[i], now);
      // The brain's popup policy answers them first, within a tick (Now #3);
      // the brain hears what was answered.
      for (std::string& answered : AnswerPopupsByPolicy(bot)) {
        wlb::snapshot::Event info;
        info.info.emplace().text = "popup policy: " + std::move(answered);
        SendImmediateEvent(i, config_.bots[i], std::move(info));
      }
      // A trade someone opened with the bot opens its window, as a client's
      // does; its prompt below wakes the brain to fill, accept or cancel it.
      BotManager::OpenRequestedTrade(bot);
      // Timed popups (ready check, LFG, loot roll, quest share, rez, trade)
      // wake the brain while they are still open (RH-H1).
      PopupState popup_state = BotManager::ReadPopupState(bot);
      if (BotSessionRegistry::Instance().InspectSession(
              bot->GetGUID().GetRawValue(),
              [](const BotSession* s) { return s && s->auto_popups; })) {
        // Deferred auto-popup loot rolls are answered by C++ once the human
        // votes or the deferral window expires; don't wake the LLM for them.
        popup_state.rolls.clear();
      }
      for (wlb::snapshot::Event& ev : bots_[i].popups.Update(popup_state)) {
        EmitAlertingEvent(i, config_.bots[i], bot, std::move(ev));
      }
      DrainPendingChat(bot, i, config_.bots[i], now);
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
    BotManager::SetBrainConnected(false);
    BotManager::SetLlmHealthy(true);
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
    trace_writer_.reset();
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
    if (sent.dropped_inbound > 0) {
      LOG_WARN("module",
               "mod-llm-brain: {} harness command(s) dropped: the world thread "
               "fell behind (LlmBrain max pending messages reached)",
               sent.dropped_inbound);
    }
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

  // The first frame of every connection (BrainBridge sends it): this build's
  // wire version and schema hash, so a harness or tool of another build
  // refuses at once (docs/specs/snapshot.md, "Handshake").
  static std::string ServerHelloFrame() {
    wlb::snapshot::ModuleMessage hello;
    hello.version = wlb::snapshot::kWireVersion;
    hello.hello = wlb::snapshot::MakeHello("module");
    return wlb::snapshot::WriteBeve(hello);
  }

  // A client's optional hello: one of another build is refused (its commands
  // dropped, the bots left to playerbots) until it disconnects.
  void OnClientHello(const wlb::snapshot::Hello& hello) {
    const std::string mismatch = wlb::snapshot::HelloMismatch(hello);
    if (mismatch.empty()) {
      LOG_INFO("module", "mod-llm-brain: {} hello: wire version {}, schema {}",
               hello.role.empty() ? "client" : hello.role, hello.wire_version,
               hello.schema_hash);
      return;
    }
    if (!harness_refused_) {
      harness_refused_ = true;
      LOG_ERROR("module", "mod-llm-brain: refusing the connection: {}",
                mismatch);
      // Nothing it sends is applied, so playerbots keeps the bots alive.
      BotManager::SetBrainConnected(false);
      BotManager::SetLlmHealthy(true);
    }
  }

  void OnClientHealth(const wlb::snapshot::Command::Health& health) {
    const bool was = BotManager::IsLlmHealthy();
    BotManager::SetLlmHealthy(health.llm_healthy);
    if (was != health.llm_healthy) {
      LOG_INFO("module", "mod-llm-brain: LLM {}; {}",
               health.llm_healthy ? "healthy" : "unhealthy",
               health.llm_healthy ? "restoring brain popup ownership"
                                  : "handing popups to playerbots");
    }
  }

  void OnHarnessConnection(bool connected) {
    LOG_INFO("module", "mod-llm-brain: harness {}",
             connected ? "connected" : "disconnected");
    BotManager::SetBrainConnected(connected);
    BotManager::SetLlmHealthy(true);
    wrong_version_logged_ = false;
    harness_refused_ = false;
    for (size_t i = 0; i < bots_.size(); ++i) {
      Player* bot = ObjectAccessor::FindPlayerByName(config_.bots[i]);
      if (bot && GET_PLAYERBOT_AI(bot) != nullptr) {
        BotSessionRegistry::Instance().ResetOnDisconnect(
            bot->GetGUID().GetRawValue());
      }
      ResetRtiOverride(bot, bots_[i].rti_override);
      ResetStopDps(bot, bots_[i].stop_dps);
      ClearHealerManaWait(bots_[i].healer_mana_wait);
      ClearLeaderWait(bots_[i].leader_wait);
      bots_[i].errand_hold_active = false;
      if (bots_[i].active_act) {
        if (bot && GET_PLAYERBOT_AI(bot) != nullptr) {
          CancelActiveAct(bot, &*bots_[i].active_act);
        }
        bots_[i].active_act.reset();
      }
      bots_[i].pending_chats.clear();
      // A new harness has heard of no popup yet: report open ones again.
      bots_[i].popups.Reset();
      if (connected) {
        bots_[i].active_option.reset();
        bots_[i].active_item_spell_id = 0;
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

  // True only when `player` is in the world and controlled by Playerbots AI
  // (never a human player who logged into a character name listed in
  // `LlmBrain.Bots`).
  static bool IsControlledBot(Player* player) {
    return player != nullptr && player->IsInWorld() &&
           GET_PLAYERBOT_AI(player) != nullptr;
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
      case wlb::snapshot::RAID_WARNING:
        return "raid_warning";
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
      if (index == config_.bots.size() || !IsControlledBot(bot) ||
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
      // A human group member's command ("wait", "stop", "follow", "attack my
      // target"...) in party/raid/whisper is applied here, within a tick; the
      // brain then hears the line and what was done, to answer in character.
      std::string applied;
      if (line.chat.channel == wlb::snapshot::PARTY ||
          line.chat.channel == wlb::snapshot::RAID ||
          line.chat.channel == wlb::snapshot::RAID_WARNING ||
          line.chat.channel == wlb::snapshot::WHISPER) {
        applied = ApplyLeaderCommand(index, line.to, bot, line.chat);
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
      if (!applied.empty()) {
        wlb::snapshot::Event info;
        info.info.emplace().text = std::move(applied);
        SendImmediateEvent(index, line.to, std::move(info));
      }
    }
  }

  // The group's human leader (or a human naming this bot) typed a command:
  // carries it out and says what was done ("leader said: wait (applied: you
  // stay here)"), or returns empty when the line isn't one for this bot.
  // What a command means is mechanical (a mark, a ping, the speaker's spot);
  // what to say or do next is the brain's.
  std::string ApplyLeaderCommand(size_t index,
                                 const std::string& name,
                                 Player* bot,
                                 const wlb::snapshot::ChatEvent& chat) {
    Group* group = bot->GetGroup();
    Player* sender = ObjectAccessor::FindPlayerByName(chat.from);
    if (!group || !sender || sender == bot || sender->GetGroup() != group ||
        !IsHumanPlayer(sender)) {
      return "";
    }
    std::vector<std::string> names;
    for (const std::string& bot_name : config_.bots) {
      if (Player* p = ObjectAccessor::FindPlayerByName(bot_name);
          p && p->GetGroup() == group) {
        names.push_back(p->GetName());
      }
    }
    std::sort(names.begin(), names.end());
    const std::optional<ParsedLeaderCommand> parsed =
        ParseLeaderCommand(chat.text, names);
    if (!parsed) {
      return "";
    }
    BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
    Player* leader =
        ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
    const bool sender_is_leader = group->IsLeader(sender->GetGUID());
    const bool leader_is_human =
        sender_is_leader || (leader != nullptr && IsHumanPlayer(leader));
    const bool is_whisper = chat.channel == wlb::snapshot::WHISPER;
    bool for_me = IsLeaderCommandForBot(/*sender_is_human=*/true,
                                        sender_is_leader, leader_is_human,
                                        is_whisper, *parsed, bot->GetName());
    // An unaddressed "pull" in party/raid is for whoever tanks: the group's
    // tank role (the Dungeon Finder's, else the spec's), unless the bot let
    // someone else tank. An unaddressed "lust" in party/raid is only for bots
    // that know Bloodlust or Heroism.
    if (for_me && !is_whisper && parsed->addressed.empty() &&
        parsed->command == LeaderCommand::kPull) {
      for_me = IsGroupTank(group, bot) && !policy.not_tank;
    } else if (for_me && !is_whisper && parsed->addressed.empty() &&
               parsed->command == LeaderCommand::kLust) {
      for_me = LustSpellName(bot) != nullptr;
    }
    if (!for_me) {
      return "";
    }
    const std::string said = "leader said: " + DescribeLeaderCommand(*parsed);
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    auto halt_attacks = [&]() {
      RecordStopDps(bots_[index].stop_dps, IsPartyInCombat(bot));
      BotManager::SetStopDps(bot, true);
      bot->AttackStop();
      bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
      for (CurrentSpellTypes type :
           {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL}) {
        if (Spell* sp = bot->GetCurrentSpell(type);
            sp && sp->GetSpellInfo() && !sp->GetSpellInfo()->IsPositive()) {
          bot->InterruptSpell(type, false);
        }
      }
      if (Pet* pet = bot->GetPet()) {
        pet->AttackStop();
      }
    };
    std::string done;
    switch (parsed->command) {
      case LeaderCommand::kWait:
        policy.stay = true;
        BotManager::SetGroupPolicy(bot, policy);
        RecordLeaderWait(bots_[index].leader_wait, base::TimeTicks::Now());
        BotManager::ClearErrandHold(bot);
        bots_[index].errand_hold_active = false;
        bot->StopMoving();
        done = "you stop and wait here";
        break;
      case LeaderCommand::kStop:
        policy.stay = true;
        BotManager::SetGroupPolicy(bot, policy);
        RecordLeaderWait(bots_[index].leader_wait, base::TimeTicks::Now());
        BotManager::ClearErrandHold(bot);
        bots_[index].errand_hold_active = false;
        bot->StopMoving();
        halt_attacks();
        done = "you stop moving and attacking";
        break;
      case LeaderCommand::kFollow: {
        policy.stay = false;
        BotManager::SetGroupPolicy(bot, policy);
        ClearLeaderWait(bots_[index].leader_wait);
        if (!bot->IsInCombat() && !IsPartyInCombat(bot)) {
          ResetStopDps(bot, bots_[index].stop_dps);
        }
        // A leader's move still on its way ("go there", "los") would hold
        // the bot again when it arrives: following ends it.
        BotState& state = bots_[index];
        if (state.active_act && !state.active_act->for_option &&
            state.active_act->command_id == 0) {
          CancelActiveAct(bot, &*state.active_act);
          state.active_act.reset();
        }
        BotManager::ClearPingStay(bot);
        BotManager::ClearErrandHold(bot);
        state.errand_hold_active = false;
        done = "you follow " + sender->GetName() + " again";
        break;
      }
      case LeaderCommand::kAttackMyTarget: {
        Unit* target = sender->GetSelectedUnit();
        if (!target || !bot->IsValidAttackTarget(target)) {
          return said + " (not applied: no enemy targeted)";
        }
        BotManager::ClearErrandHold(bot);
        bots_[index].errand_hold_active = false;
        ResetStopDps(bot, bots_[index].stop_dps);
        LeaderAttack(bot, ai, target, /*mark=*/std::nullopt,
                     &bots_[index].rti_override);
        done = "you attack " + target->GetName();
        break;
      }
      case LeaderCommand::kAttackMark:
      case LeaderCommand::kPull: {
        // "pull" alone pulls what the speaker targets, else skull.
        std::optional<size_t> mark = parsed->mark;
        Unit* target = nullptr;
        if (!mark) {
          target = sender->GetSelectedUnit();
          if (!target || !bot->IsValidAttackTarget(target)) {
            target = nullptr;
            mark = kSkullIcon;
          }
        }
        if (mark) {
          target = ObjectAccessor::GetUnit(*bot, group->GetTargetIcon(*mark));
          if (!target || !bot->IsValidAttackTarget(target)) {
            return said + " (not applied: nothing marked " +
                   std::string(kRaidIconNames[*mark]) +
                   (parsed->mark ? "" : " or targeted") + ")";
          }
        }
        ResetStopDps(bot, bots_[index].stop_dps);
        LeaderAttack(bot, ai, target, mark, &bots_[index].rti_override);
        done = "you attack " + target->GetName() +
               (mark ? " (" + std::string(kRaidIconNames[*mark]) + ")" : "");
        break;
      }
      case LeaderCommand::kDrink: {
        const ActionOutcome rest = ExecuteRestAction(bot);
        if (!rest.error.empty()) {
          return said + " (not applied: " + rest.error + ")";
        }
        done = "you sit down to eat and drink";
        break;
      }
      case LeaderCommand::kMount:
        if (bot->IsMounted() || bot->IsInCombat() ||
            (bot->GetMap() && bot->GetMap()->IsDungeon()) || !ai ||
            !ai->DoSpecificAction("mount", Event(), true)) {
          return said + " (not applied: you can't mount here)";
        }
        done = "you mount up";
        break;
      case LeaderCommand::kHealMe:
        // Who heals and with what is the brain's call: it is woken for it.
        // The body's own heal reflexes (playerbots' heal strategies, when it
        // has them) look at the party now instead of after its react delay.
        if (ai) {
          ai->SetNextCheckDelay(0);
        }
        return said + " (" + sender->GetName() + " asks for heals)";
      case LeaderCommand::kMoveToPing:
      case LeaderCommand::kPullBack: {
        // To the party's minimap ping, or to the speaker's spot ("los": out
        // of the pull's sight), then hold there through the fight.
        wlb::snapshot::Command_Act act;
        act.verb = wlb::snapshot::Command_Act::TO;
        act.place = wlb::snapshot::Command_Act::AT;
        const bool to_ping = parsed->command == LeaderCommand::kMoveToPing;
        if (to_ping) {
          act.thing = wlb::snapshot::Command_Act::PING;
        } else {
          act.thing = wlb::snapshot::Command_Act::NAMED;
          act.name = sender->GetName();
        }
        const std::string error =
            StartLeaderMove(index, name, bot, act, /*hold=*/!to_ping);
        if (!error.empty()) {
          return said + " (not applied: " + error + ")";
        }
        policy.stay = false;
        BotManager::SetGroupPolicy(bot, policy);
        ClearLeaderWait(bots_[index].leader_wait);
        done = to_ping ? "you go to the ping and hold there"
                       : "you run to " + sender->GetName() + " and hold there";
        break;
      }
      case LeaderCommand::kLust: {
        const char* spell_name = LustSpellName(bot);
        if (!spell_name) {
          return said + " (not applied: you don't know Bloodlust or Heroism)";
        }
        if (!bot->IsAlive()) {
          return said + " (not applied: you are dead)";
        }
        if (bot->HasAura(kBloodlustSpellId) || bot->HasAura(kHeroismSpellId) ||
            bot->HasAura(kSatedAuraId) || bot->HasAura(kExhaustionAuraId)) {
          return said + " (not applied: already active or exhausted)";
        }
        wlb::snapshot::Command_Act act;
        act.verb = wlb::snapshot::Command_Act::USE;
        act.name = spell_name;
        ActiveAct pending;
        const ActStartResult res = StartAct(bot, /*command_id=*/0, act,
                                            base::TimeTicks::Now(), &pending);
        if (!res.error.empty()) {
          return said + " (not applied: " + res.error + ")";
        }
        done = std::string("you cast ") + spell_name;
        break;
      }
      case LeaderCommand::kSpread: {
        if (sender->GetMapId() != bot->GetMapId() ||
            sender->GetInstanceId() != bot->GetInstanceId()) {
          return said + " (not applied: leader is on another map)";
        }
        const auto self_it =
            std::find(names.begin(), names.end(), bot->GetName());
        const size_t slot_index =
            self_it != names.end()
                ? static_cast<size_t>(self_it - names.begin())
                : 0;
        const AnchorGeometry center{
            .pos = {sender->GetPositionX(), sender->GetPositionY(),
                    sender->GetPositionZ()},
            .facing_rad = sender->GetOrientation(),
            .radius_yd = sender->GetCombatReach(),
        };
        const std::string error = StartLeaderSpreadMove(
            index, name, bot, center, slot_index, names.size());
        if (!error.empty()) {
          return said + " (not applied: " + error + ")";
        }
        policy.stay = false;
        BotManager::SetGroupPolicy(bot, policy);
        ClearLeaderWait(bots_[index].leader_wait);
        done = "you spread out and hold there";
        break;
      }
      case LeaderCommand::kStack: {
        wlb::snapshot::Command_Act act;
        act.verb = wlb::snapshot::Command_Act::TO;
        act.place = wlb::snapshot::Command_Act::AT;
        act.thing = wlb::snapshot::Command_Act::NAMED;
        act.name = sender->GetName();
        const std::string error =
            StartLeaderMove(index, name, bot, act, /*hold=*/true);
        if (!error.empty()) {
          return said + " (not applied: " + error + ")";
        }
        policy.stay = false;
        BotManager::SetGroupPolicy(bot, policy);
        ClearLeaderWait(bots_[index].leader_wait);
        done = "you stack on " + sender->GetName() + " and hold there";
        break;
      }
      case LeaderCommand::kStopDps: {
        halt_attacks();
        done = "you stop attacking until resumed or combat ends";
        break;
      }
      case LeaderCommand::kResumeDps: {
        ResetStopDps(bot, bots_[index].stop_dps);
        if (ai) {
          ai->SetNextCheckDelay(0);
        }
        done = "you resume attacking";
        break;
      }
    }
    LOG_INFO("module", "mod-llm-brain: {} applied leader command {} from {}",
             bot->GetName(), DescribeLeaderCommand(*parsed), sender->GetName());
    return said + " (applied: " + done + ")";
  }

  static constexpr size_t kSkullIcon = 7;
  static constexpr uint32_t kBloodlustSpellId = 2825;
  static constexpr uint32_t kHeroismSpellId = 32182;
  static constexpr uint32_t kExhaustionAuraId = 57723;
  static constexpr uint32_t kSatedAuraId = 57724;

  static const char* LustSpellName(const Player* bot) {
    if (bot->HasSpell(kBloodlustSpellId)) {
      return "Bloodlust";
    }
    if (bot->HasSpell(kHeroismSpellId)) {
      return "Heroism";
    }
    return nullptr;
  }

  static GroupMemberRoleInput MemberRoleInput(Group* group, Player* member) {
    GroupMemberRoleInput in;
    in.guid = member->GetGUID().GetRawValue();
    in.online_in_world = member->IsInWorld();
    if (in.online_in_world) {
      in.spec_tank = PlayerbotAI::IsTank(member);
      in.spec_healer = PlayerbotAI::IsHeal(member);
      in.not_tank_policy = GET_PLAYERBOT_AI(member) &&
                           BotManager::GetGroupPolicy(member).not_tank;
    }
    for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
      if (slot.guid == member->GetGUID()) {
        in.subgroup = static_cast<uint32_t>(slot.group) + 1;
        in.main_tank_flag = (slot.flags & MEMBER_FLAG_MAINTANK) != 0;
        if (slot.roles & lfg::PLAYER_ROLE_TANK) {
          in.lfg_role = wlb::snapshot::Role::TANK;
        } else if (slot.roles & lfg::PLAYER_ROLE_HEALER) {
          in.lfg_role = wlb::snapshot::Role::HEALER;
        } else if (slot.roles & lfg::PLAYER_ROLE_DAMAGE) {
          in.lfg_role = wlb::snapshot::Role::DPS;
        }
        break;
      }
    }
    return in;
  }

  // The group's tank as party frames show it (unified via `group_roles.h`).
  static bool IsGroupTank(Group* group, Player* bot) {
    return IsEffectiveTank(MemberRoleInput(group, bot));
  }

  // The group's healer as party frames show it (unified via `group_roles.h`).
  static bool IsGroupHealer(Group* group, Player* member) {
    return IsEffectiveHealer(MemberRoleInput(group, member));
  }

  // The facts behind the pull gate (OptionFacts::hold_pulls/hold_explore):
  // what the group's humans are doing and whether a party healer is low on
  // mana / drinking. Whether to pull with what's left is the brain's call.
  static void FillPullFacts(Player* bot,
                            Group* group,
                            HealerManaWaitState* healer_wait,
                            base::TimeTicks now,
                            OptionFacts* facts) {
    constexpr float kHumanBehindYd = 25.0f;
    bool human_leads = false;
    bool human_holds = false;  // A human tanks, is AFK or is behind.
    std::vector<HealerManaMember> healer_members;
    for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
      if (slot.guid == bot->GetGUID()) {
        continue;
      }
      Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
      if (!member || !member->IsInWorld()) {
        continue;
      }
      if (IsGroupHealer(group, member)) {
        const bool same_map = member->GetMapId() == bot->GetMapId() &&
                              member->GetInstanceId() == bot->GetInstanceId();
        const bool uses_mana = member->GetMaxPower(POWER_MANA) > 0;
        const float mana_pct =
            uses_mana ? member->GetPowerPct(POWER_MANA) : 100.0f;
        const bool drinking =
            HasFoodOrDrinkAura(member) ||
            (member->getStandState() == UNIT_STAND_STATE_SIT &&
             mana_pct < 99.0f);
        healer_members.push_back({
            .is_healer = true,
            .online = true,
            .alive = member->IsAlive(),
            .same_map = same_map,
            .uses_mana = uses_mana,
            .dist_yd = same_map ? bot->GetDistance(member) : 0.0f,
            .mana_pct = mana_pct,
            .drinking = drinking,
        });
      }
      if (!IsHumanPlayer(member)) {
        continue;
      }
      human_leads |= slot.guid == group->GetLeaderGUID();
      human_holds |= (slot.flags & MEMBER_FLAG_MAINTANK) != 0 ||
                     IsGroupTank(group, member) || member->isAFK() ||
                     member->GetMapId() != bot->GetMapId() ||
                     bot->GetDistance(member) > kHumanBehindYd;
    }
    const bool healer_mana_hold =
        healer_wait != nullptr &&
        EvaluateHealerManaPullWait(*healer_wait, /*in_group=*/true,
                                   IsPartyInCombat(bot), healer_members, now);
    const bool bot_tanks =
        IsGroupTank(group, bot) && !BotManager::GetGroupPolicy(bot).not_tank;
    facts->hold_pulls =
        human_holds || healer_mana_hold || (human_leads && !bot_tanks);
    facts->hold_explore = (human_holds || healer_mana_hold) &&
                          group->IsLeader(bot->GetGUID()) && bot->GetMap() &&
                          bot->GetMap()->IsDungeon();
  }

  static bool IsPartyInCombat(Player* bot) {
    if (bot->IsInCombat()) {
      return true;
    }
    Group* group = bot->GetGroup();
    if (!group) {
      return false;
    }
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
         itr = itr->next()) {
      Player* member = itr->GetSource();
      if (member && member != bot && member->IsInWorld() &&
          member->GetMapId() == bot->GetMapId() &&
          member->GetInstanceId() == bot->GetInstanceId() &&
          member->IsInCombat()) {
        return true;
      }
    }
    return false;
  }

  static void ApplyDefaultRti(Player* bot) {
    if (PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr) {
      ai->GetAiObjectContext()->GetValue<std::string>("rti")->Set(
          std::string(kDefaultRti));
      ai->GetAiObjectContext()
          ->GetValue<GuidVector>("prioritized targets")
          ->Set({});
    }
  }

  static void ResetRtiOverride(Player* bot, RtiOverrideState& rti_override) {
    if (!ClearRtiOverride(rti_override)) {
      return;
    }
    ApplyDefaultRti(bot);
  }

  static void ResetStopDps(Player* bot, StopDpsState& stop_dps) {
    ClearStopDps(stop_dps);
    BotManager::SetStopDps(bot, false);
  }

  // Attacks `target` for a leader's command, through playerbots' own attack
  // (and its raid-target value for a mark) when it has one.
  static void LeaderAttack(Player* bot,
                           PlayerbotAI* ai,
                           Unit* target,
                           std::optional<size_t> mark,
                           RtiOverrideState* rti_override) {
    bot->SetSelection(target->GetGUID());
    if (ai) {
      ai->GetAiObjectContext()
          ->GetValue<GuidVector>("prioritized targets")
          ->Set({target->GetGUID()});
      if (mark) {
        ai->GetAiObjectContext()->GetValue<std::string>("rti")->Set(
            std::string(kRaidIconNames[*mark]));
        if (rti_override) {
          RecordRtiOverride(*rti_override, target->GetGUID().GetRawValue(),
                            IsPartyInCombat(bot));
        }
      }
    }
    const char* action = mark ? "attack rti target" : "attack my target";
    if (!ai || !ai->DoSpecificAction(action, Event(), true)) {
      bot->Attack(target, true);
    }
  }

  // Starts a leader's move like a brain's act (command 0: no plan waits on
  // it); it takes over from a running act the way OnAct does. Returns the
  // reason when it can't start.
  std::string StartLeaderMove(size_t index,
                              const std::string& name,
                              Player* bot,
                              const wlb::snapshot::Command_Act& act,
                              bool hold) {
    BotState& state = bots_[index];
    ActiveAct pending;
    const ActStartResult res =
        StartAct(bot, /*command_id=*/0, act, base::TimeTicks::Now(), &pending);
    if (!res.error.empty()) {
      return res.error;
    }
    BotManager::ClearPingStay(bot);
    ReportOptionUseDropped(index, name, bot);
    if (state.active_act && !state.active_act->for_option &&
        state.active_act->command_id != 0) {
      // The brain's own act gives way to the leader: its plan hears why.
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = "act cancelled by the leader's command";
      failed.command_id = state.active_act->command_id;
      EmitAlertingEvent(index, name, bot, std::move(payload));
    }
    if (res.done) {
      if (state.active_act) {
        CancelActiveAct(bot, &*state.active_act);
        state.active_act.reset();
      }
      // Already there (a `to ping` act holds by itself).
      if (hold || act.thing == wlb::snapshot::Command_Act::PING) {
        BotManager::StartPingStay(bot);
      }
    } else {
      // A movement act replaces the old one without stopping the new walk.
      pending.hold_on_arrival = hold;
      state.active_act = std::move(pending);
    }
    schedule_->SetActiveTargetPending(
        index, state.active_option.has_value() || state.active_act.has_value());
    return "";
  }

  std::string StartLeaderSpreadMove(size_t index,
                                    const std::string& name,
                                    Player* bot,
                                    const AnchorGeometry& center,
                                    size_t slot_index,
                                    size_t total_bots) {
    BotState& state = bots_[index];
    ActiveAct pending;
    const ActStartResult res = StartSpreadAct(
        bot, center, slot_index, total_bots, base::TimeTicks::Now(), &pending);
    if (!res.error.empty()) {
      return res.error;
    }
    BotManager::ClearPingStay(bot);
    ReportOptionUseDropped(index, name, bot);
    if (state.active_act && !state.active_act->for_option &&
        state.active_act->command_id != 0) {
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = "act cancelled by the leader's command";
      failed.command_id = state.active_act->command_id;
      EmitAlertingEvent(index, name, bot, std::move(payload));
    }
    if (res.done) {
      if (state.active_act) {
        CancelActiveAct(bot, &*state.active_act);
        state.active_act.reset();
      }
      BotManager::StartPingStay(bot);
    } else {
      state.active_act = std::move(pending);
    }
    schedule_->SetActiveTargetPending(
        index, state.active_option.has_value() || state.active_act.has_value());
    return "";
  }

  void CheckPendingGroupInvite(Player* bot,
                               size_t index,
                               const std::string& name,
                               base::TimeTicks now) {
    Group* invite = bot->GetGroupInvite();
    BotState& state = bots_[index];
    if (!invite) {
      state.seen_group_invite_leader.clear();
      state.pending_invite_since = base::TimeTicks();
      return;
    }
    const std::string leader = invite->GetLeaderName();
    if (leader.empty()) {
      return;
    }
    if (leader == state.seen_group_invite_leader) {
      // Match the WoW client's 60 s group-invite timeout so an unanswered
      // invite never stays pending forever and blocks other invites.
      static constexpr base::TimeDelta kInviteTimeout = base::Seconds(60);
      if (!state.pending_invite_since.is_null() &&
          now - state.pending_invite_since >= kInviteTimeout) {
        if (WorldSession* session = bot->GetSession()) {
          WorldPacket pkt(CMSG_GROUP_DECLINE, 0);
          session->HandleGroupDeclineOpcode(pkt);
          LOG_INFO("module",
                   "mod-llm-brain: {} auto-declined stale group invite from {}",
                   name, leader);
        }
        state.seen_group_invite_leader.clear();
        state.pending_invite_since = base::TimeTicks();
      }
      return;
    }
    state.seen_group_invite_leader = leader;
    state.pending_invite_since = now;
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
    // Before the version check: a hello of another version is what it names.
    if (command.hello) {
      OnClientHello(*command.hello);
      return;
    }
    if (harness_refused_) {
      return;  // Logged once, in OnClientHello.
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
    if (command.health) {
      OnClientHealth(*command.health);
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
    if (bot_index == config_.bots.size() || !IsControlledBot(bot) ||
        bots_[bot_index].seq == 0) {
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
    const bool keep = target && IsKeepAction(target->action);
    const bool attack = target && IsAttackAction(target->action);
    uint32_t pending_spell_id = 0;
    // A crowd-control cast that walks in / stealths / casts over ticks runs
    // as the `use` act machinery, finishing as this option.
    ActiveAct option_use;
    bool option_use_started = false;
    if (target) {
      // Validate BEFORE cancelling any active dodge/movement act so an invalid
      // or combat-blocked choice never aborts an active boss dodge.
      error = ValidateAction(bot, target->action);
      if (error.empty()) {
        if (!keep) {
          if (!attack) {
            // A new errand ends a `to ping` hold (a fight from there doesn't).
            BotManager::ClearPingStay(bot);
          }
          // Attack leaves a running act be, but not an earlier choice's
          // crowd-control cast (a new choice replaces it).
          if (state.active_act && (!attack || state.active_act->for_option)) {
            const uint32_t cancelled_cmd_id = state.active_act->command_id;
            CancelActiveAct(bot, &*state.active_act);
            state.active_act.reset();
            // A leader command's move (command 0) is the brain's to replace.
            if (cancelled_cmd_id != 0) {
              wlb::snapshot::Event payload;
              wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
              failed.text = "act cancelled by new choice";
              failed.command_id = cancelled_cmd_id;
              EmitAlertingEvent(bot_index, name, bot, std::move(payload));
            }
          }
          state.had_food_drink_aura = false;
          state.attack_idle_since = base::TimeTicks();
          state.walk = PendingWalk{};
        }
        ActionRunContext ctx;
        ctx.walk = &state.walk;
        ctx.dungeon_nav = &config_.dungeon_nav;
        ctx.out_active_act = &option_use;
        ctx.now = base::TimeTicks::Now();
        ExecuteResult result = ExecuteAction(bot, target->action, &ctx);
        error = std::move(result.error);
        used = result.used;
        info = std::move(result.info);
        pending_spell_id = result.pending_spell_id;
        option_use_started = result.act_started;
        if (!result.path_time.is_zero()) {
          path_times_.push_back(result.path_time);
        }
        if (IsRestAction(target->action)) {
          state.had_food_drink_aura = HasFoodOrDrinkAura(bot);
        }
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
      state.active_item_spell_id = 0;
      state.errand_hold_active = false;
      BotManager::ClearErrandHold(bot);
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
      // clicked yet. If no option is pending, drop any post-NPC grace hold so
      // follow resumes immediately.
      if (!state.active_option) {
        state.errand_hold_active = false;
        BotManager::ClearErrandHold(bot);
      }
      state.done_command_id = command.command_id;
    } else if (option_use_started) {
      state.active_option.reset();
      state.active_item_spell_id = 0;
      state.errand_hold_active = false;
      BotManager::ClearErrandHold(bot);
      option_use.command_id = command.command_id;
      option_use.for_option = true;
      state.active_act = std::move(option_use);
      schedule_->SetActiveTargetPending(bot_index, true);
    } else if (target) {
      // Hub work (quests, vendors, trainers, loot) writes no combat event:
      // this one shows the stall gate the bot is doing things.
      if (trace_writer_ && trace_writer_->enabled()) {
        TraceEvent ev;
        ev.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
        ev.monotonic_us =
            (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
        ev.role = "bot";
        ev.name = name;
        ev.event = "option_chosen";
        ev.detail = std::string(OptionKindName(target->kind));
        trace_writer_->WriteEvent(ev);
      }
      if (!info.empty()) {
        wlb::snapshot::Event payload;
        payload.info.emplace().text = std::move(info);
        SendImmediateEvent(bot_index, name, std::move(payload));
      }
      state.object_used = used;
      state.active_item_spell_id = pending_spell_id;
      state.active_command_id = command.command_id;
      const TargetProgress progress = OptionTargetProgress(
          bot, *target, used, &state.had_food_drink_aura, pending_spell_id);
      if (progress.pending) {
        state.active_option = *target;
        const ErrandKind errand_kind = ErrandKindForAction(target->action);
        if (errand_kind != ErrandKind::kNone) {
          state.errand_hold_active =
              BotManager::StartErrandHold(bot, errand_kind);
        } else if (ActionCancelsErrandHold(target->action)) {
          state.errand_hold_active = false;
          BotManager::ClearErrandHold(bot);
        }
        schedule_->SetActiveTargetPending(bot_index, true);
      } else if (!progress.interrupted.empty()) {
        state.active_option = *target;
        FailActiveOption(bot_index, name, bot, progress.interrupted);
      } else {
        state.active_option.reset();
        state.active_item_spell_id = 0;
        state.errand_hold_active = false;
        const ErrandKind errand_kind = ErrandKindForAction(target->action);
        if (errand_kind == ErrandKind::kTalk ||
            errand_kind == ErrandKind::kNpcInteract) {
          BotManager::StartErrandHold(bot, ErrandKind::kNpcInteract);
        } else {
          BotManager::ClearErrandHold(bot);
        }
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
    // Validate and run via the unified `ExecuteAction` path BEFORE cancelling
    // any running act so a failed act never aborts an active boss dodge.
    ActiveAct pending;
    ActionRunContext ctx;
    ctx.out_active_act = &pending;
    ctx.now = base::TimeTicks::Now();
    ExecuteResult res =
        ExecuteAction(bot, StandingActAction{command_id, act}, &ctx);
    LOG_INFO("module", "mod-llm-brain: {} act {} ({}): {}", name,
             wlb::snapshot::EnumName(act.verb), act.name,
             res.error.empty() ? (res.used ? "done" : "running") : res.error);
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
      schedule_->SetActiveTargetPending(
          bot_index,
          state.active_option.has_value() || state.active_act.has_value());
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = std::move(res.error);
      failed.command_id = command_id;
      EmitAlertingEvent(bot_index, name, bot, std::move(payload));
    } else if (res.used) {
      state.errand_hold_active = false;
      BotManager::ClearErrandHold(bot);
      if (IsMovementActVerb(act.verb) &&
          (bot->IsInCombat() || IsPartyInCombat(bot))) {
        BotManager::SetCombatPositionHold(bot, true);
      }
      const bool stops_movement =
          (act.verb == wlb::snapshot::Command_Act::STOP &&
           (act.stop == wlb::snapshot::Command_Act::MOVING ||
            act.stop == wlb::snapshot::Command_Act::STOP_UNSPECIFIED)) ||
          IsMovementActVerb(act.verb);
      if (stops_movement && state.active_act) {
        ReportOptionUseDropped(bot_index, name, bot);
        if (act.verb != wlb::snapshot::Command_Act::STOP) {
          CancelActiveAct(bot, &*state.active_act);
        }
        state.active_act.reset();
      }
      state.done_act_command_id = command_id;
      schedule_->SetActiveTargetPending(
          bot_index,
          state.active_option.has_value() || state.active_act.has_value());
    } else {
      state.errand_hold_active = false;
      BotManager::ClearErrandHold(bot);
      if (IsMovementActVerb(act.verb) &&
          (bot->IsInCombat() || IsPartyInCombat(bot))) {
        BotManager::SetCombatPositionHold(bot, true);
      }
      ReportOptionUseDropped(bot_index, name, bot);
      if (state.active_act && !IsMovementActVerb(act.verb)) {
        // Non-movement multi-tick act (e.g. cast-time `use`) supersedes an old
        // movement act's forced priority.
        CancelActiveAct(bot, &*state.active_act);
      }
      state.active_act = std::move(pending);
      schedule_->SetActiveTargetPending(bot_index, true);
    }
  }

  // Tells the bot manager's policy listener while a dodge moves the bot out
  // (ActMovesOut): playerbots' spells that would hold it still wait. Written
  // every tick of the move (the hold lapses within kMovingOutHoldMs if
  // ticking stops) and once when it ends.
  void SyncMovingOut(Player* bot, size_t index) {
    constexpr uint32_t kMovingOutHoldMs = 1000;
    BotState* state = &bots_[index];
    const bool moving_out =
        state->active_act && ActMovesOut(*state->active_act);
    if (!moving_out && !state->moving_out) {
      return;
    }
    state->moving_out = moving_out;
    const uint32_t until = moving_out ? getMSTime() + kMovingOutHoldMs : 0;
    BotSessionRegistry::Instance().WithSession(
        bot->GetGUID().GetRawValue(),
        [until](BotSession& s) { s.moving_out_until_ms = until; });
  }

  // Tells the bot manager's policy listener while a `use` of a stealth-only
  // spell walks in (ActHoldsStealth): playerbots' `unstealth` and casts would
  // break the stealth it entered (Sap from Stealth). Written every tick like
  // SyncMovingOut, and once when it ends.
  void SyncActStealth(Player* bot, size_t index) {
    constexpr uint32_t kActStealthHoldMs = 1000;
    BotState* state = &bots_[index];
    const bool holds = state->active_act && ActHoldsStealth(*state->active_act);
    if (!holds && !state->act_stealth) {
      return;
    }
    state->act_stealth = holds;
    const uint32_t until = holds ? getMSTime() + kActStealthHoldMs : 0;
    BotSessionRegistry::Instance().WithSession(
        bot->GetGUID().GetRawValue(),
        [until](BotSession& s) { s.act_stealth_until_ms = until; });
  }

  // An act taking over the body from a chosen crowd-control cast still
  // walking in or casting: that option fails (the caller drops or cancels the
  // act state itself).
  void ReportOptionUseDropped(size_t bot_index,
                              const std::string& name,
                              Player* bot) {
    BotState& state = bots_[bot_index];
    if (!state.active_act || !state.active_act->for_option) {
      return;
    }
    wlb::snapshot::Event payload;
    wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
    failed.text = "cancelled by an act";
    failed.command_id = state.active_act->command_id;
    EmitAlertingEvent(bot_index, name, bot, std::move(payload));
  }

  bool CanSendChatNow(size_t bot_index, base::TimeTicks now) {
    std::deque<base::TimeTicks>& sent_times = bots_[bot_index].recent_chat_sent;
    while (!sent_times.empty() &&
           now - sent_times.front() > base::Seconds(10)) {
      sent_times.pop_front();
    }
    return sent_times.empty() || (sent_times.size() < 5 &&
                                  now - sent_times.back() >= base::Seconds(1));
  }

  void SendChatNow(size_t bot_index,
                   const std::string& name,
                   Player* bot,
                   uint32_t command_id,
                   const wlb::snapshot::Command::Say& say,
                   base::TimeTicks now) {
    const std::string error = Speak(bot, say);
    if (error.empty()) {
      bots_[bot_index].recent_chat_sent.push_back(now);
    }
    LOG_INFO("module", "mod-llm-brain: {} says ({}{}{}, len={}): {}", name,
             wlb::snapshot::EnumName(say.channel), say.to.empty() ? "" : " to ",
             say.to, say.text.size(), error.empty() ? "ok" : error);
    if (!error.empty()) {
      wlb::snapshot::Event payload;
      wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
      failed.text = error;
      failed.command_id = command_id;
      SendImmediateEvent(bot_index, name, std::move(payload));
    }
  }

  void DrainPendingChat(Player* bot,
                        size_t bot_index,
                        const std::string& name,
                        base::TimeTicks now) {
    BotState& state = bots_[bot_index];
    if (state.pending_chats.empty() || !CanSendChatNow(bot_index, now)) {
      return;
    }
    BotState::PendingChat next = std::move(state.pending_chats.front());
    state.pending_chats.pop_front();
    SendChatNow(bot_index, name, bot, next.command_id, next.say, now);
  }

  void OnSay(size_t bot_index,
             const std::string& name,
             Player* bot,
             uint32_t command_id,
             const wlb::snapshot::Command::Say& say) {
    // Rate-limit outbound bot chat (max 5 lines per 10 s and min 1 s spacing)
    // to prevent spam loops while matching human chat cadence. Queue up to 4
    // messages so a plan with both `say:` and `tell:` spaces them rather than
    // dropping the second line.
    const base::TimeTicks now = base::TimeTicks::Now();
    BotState& state = bots_[bot_index];
    if (state.pending_chats.empty() && CanSendChatNow(bot_index, now)) {
      SendChatNow(bot_index, name, bot, command_id, say, now);
      return;
    }
    static constexpr size_t kMaxPendingChats = 4;
    if (state.pending_chats.size() < kMaxPendingChats) {
      state.pending_chats.push_back({command_id, say});
      return;
    }
    LOG_INFO("module",
             "mod-llm-brain: {} says ({}{}{}, len={}): chat rate limited", name,
             wlb::snapshot::EnumName(say.channel), say.to.empty() ? "" : " to ",
             say.to, say.text.size());
    wlb::snapshot::Event payload;
    wlb::snapshot::ErrorEvent& failed = payload.error.emplace();
    failed.text = "chat rate limited";
    failed.command_id = command_id;
    SendImmediateEvent(bot_index, name, std::move(payload));
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
        if (state.active_act->hold_on_arrival) {
          BotManager::StartPingStay(bot);
        }
        // A leader command's move (command 0) finishes no brain command.
        if (state.active_act->command_id != 0) {
          (state.active_act->for_option ? state.done_command_id
                                        : state.done_act_command_id) =
              state.active_act->command_id;
        }
        state.active_act.reset();
      } else {
        // An active standing-order act owns movement/casting until it finishes
        // or fails: pause any overlapping plan-option walk/approach so the two
        // don't fight over MovePoint.
        return;
      }
    }
    if (!state.active_option) {
      return;
    }
    if (state.errand_hold_active && !BotManager::ErrandHoldActive(bot)) {
      state.errand_hold_active = false;
      ClearLastMovement(bot);
      if (bot->isMoving()) {
        bot->StopMoving();
      }
      FailActiveOption(
          index, name, bot,
          IsPartyInCombat(bot) ? "interrupted by combat" : "errand timed out");
      return;
    }
    if (state.object_used) {
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
      if (!IsGameObjectAction(state.active_option->action)) {
        return;
      }
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
      const WalkStepResult step =
          TickLongWalk(bot, now, &state.walk, config_.dungeon_nav);
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
    state.active_item_spell_id = 0;
    state.errand_hold_active = false;
    BotManager::ClearErrandHold(bot);
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
      const TargetProgress progress = OptionTargetProgress(
          bot, *state->active_option, state->object_used,
          &state->had_food_drink_aura, state->active_item_spell_id);
      pending = progress.pending;
      if (!progress.interrupted.empty()) {
        LOG_INFO("module", "mod-llm-brain: {}: {} interrupted: {}",
                 bot->GetName(), state->active_option->name,
                 progress.interrupted);
        FailActiveOption(index, bot->GetName(), bot, progress.interrupted);
      } else if (!pending) {
        const OptionTarget::Kind done_kind = state->active_option->kind;
        state->active_option.reset();
        state->active_item_spell_id = 0;
        if (done_kind == OptionTarget::Kind::kTalk) {
          state->errand_hold_active = false;
          BotManager::StartErrandHold(bot, ErrandKind::kNpcInteract);
        } else if (state->errand_hold_active) {
          state->errand_hold_active = false;
          BotManager::ClearErrandHold(bot);
        }
        state->done_command_id = state->active_command_id;
      }
    }
    bool reactive =
        state->seq > 0 && HasNewCastOrAura(bot, state->last_reactive);
    if (state->seq > 0) {
      if (const std::optional<RecentSpellMiss> miss = CurrentSpellMiss(bot);
          miss && miss->seq != state->last_spell_miss_seq) {
        reactive = true;
      }
    }
    // Nearby hostile casts and spell areas, looked at most every
    // kMinHazardGap in combat: a new one is due in the next snapshot.
    const base::TimeTicks now = base::TimeTicks::Now();
    if (!bot->IsInCombat()) {
      state->known_hazards.clear();
      state->new_hazard = false;
    } else if (now >= state->next_hazard_scan) {
      state->next_hazard_scan = now + SnapshotSchedule::kMinHazardGap;
      std::vector<std::pair<uint64_t, uint32_t>> hazards =
          CollectHostileHazards(bot);
      state->new_hazard |= HasNewEntry(hazards, state->known_hazards);
      state->known_hazards = std::move(hazards);
    }
    return SnapshotSchedule::Flags{
        .in_combat = bot->IsInCombat(),
        .alive = bot->IsAlive(),
        .control_mask = ControlMask(bot),
        .attacker_count = static_cast<uint32_t>(bot->getAttackers().size()),
        .active_target_pending = pending || state->active_act.has_value(),
        .reactive_event = reactive,
        .hazard_event = state->seq > 0 && state->new_hazard,
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
    std::string bytes = wlb::snapshot::WriteBeve(message);
    if (bytes.empty()) {
      LOG_ERROR("module", "mod-llm-brain: failed to encode event for {}", name);
      return;
    }
    if (bytes.size() > net::kMaxFramePayloadBytes) {
      LOG_ERROR("module",
                "mod-llm-brain: dropping oversize event for {} ({} B > {} B)",
                name, bytes.size(), net::kMaxFramePayloadBytes);
      return;
    }
    bridge_->Send(std::move(bytes), name);
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
      state.builder = std::make_unique<SnapshotBuilder>(
          NearbyBuilder::Options{config_.nameplate_range_yd,
                                 config_.los_raycasts_per_snapshot},
          config_.dungeon_nav);
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
    state.last_spell_miss_seq =
        snapshot.base.self.spell_miss ? snapshot.base.self.spell_miss->seq : 0;
    if (Group* invite = bot->GetGroupInvite()) {
      facts.pending_group_invite_from = invite->GetLeaderName();
    }
    Group* cur_group = bot->GetGroup();
    facts.can_leave_group = (cur_group != nullptr && !bot->IsInCombat());
    if (cur_group) {
      FillPullFacts(bot, cur_group, &state.healer_mana_wait, start, &facts);
    } else {
      ClearHealerManaWait(state.healer_mana_wait);
    }
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
    if (bytes.empty()) {
      LOG_ERROR("module", "mod-llm-brain: failed to encode snapshot for {}",
                name);
      return;
    }
    if (bytes.size() > net::kMaxFramePayloadBytes) {
      LOG_ERROR("module",
                "mod-llm-brain: dropping oversize snapshot for {} ({} B > {} B"
                ", units={}, objects={})",
                name, bytes.size(), net::kMaxFramePayloadBytes, nearby.units,
                nearby.objects);
      return;
    }
    stats_.Add({base::TimeTicks::Now() - start, bytes.size(), nearby.units,
                nearby.objects, nearby.hidden, nearby.raycasts});
    // The newest snapshot of a bot wins if the harness is behind.
    bridge_->SendLatest(name, std::move(bytes));
    state.last_reactive = CollectReactiveState(bot);
    state.new_hazard = false;
  }

  // Samples both observed humans and tracked bots through the exact same
  // observer-view path at 5 Hz (200 ms) plus movement-packet / combat edges so
  // downstream trajectory reconstruction sees identical timing and schema
  // (experimental/docs/self-improving-loop.md, section F).
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
    s.in_dungeon = player->GetMap() != nullptr && player->GetMap()->IsDungeon();
    s.x = player->GetPositionX();
    s.y = player->GetPositionY();
    s.z = player->GetPositionZ();
    s.moving = player->isMoving();
    s.in_combat = player->IsInCombat();
    s.dead = !player->IsAlive();
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
      const bool target_hostile =
          !player->IsFriendlyTo(target) && target->IsAlive();
      if (target_hostile) {
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
        s.target_targeting_me = target->GetTarget() == player->GetGUID() ||
                                target->GetVictim() == player;
      }
    }
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
      if (IsControlledBot(bot)) {
        SampleOneObserved(bot, "bot", now, time_ms, mono_us);
      }
    }
    if (config_.observe_humans) {
      for (const auto& [guid, player] : ObjectAccessor::GetPlayers()) {
        if (!player || !player->IsInWorld() ||
            GET_PLAYERBOT_AI(player) != nullptr ||
            BotIndex(player->GetName()) != config_.bots.size()) {
          continue;
        }
        SampleOneObserved(player, "human", now, time_ms, mono_us);
      }
    }
    if (!observed_state_.empty() &&
        (last_observed_prune_.is_null() ||
         now - last_observed_prune_ >= base::Seconds(10))) {
      last_observed_prune_ = now;
      std::erase_if(observed_state_, [](const auto& kv) {
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid(kv.first));
        return !p || !p->IsInWorld();
      });
    }
  }

  Config config_;
  const base::raw_ptr<ChatInbox> inbox_;
  const base::raw_ptr<std::atomic<BehaviorTraceWriter*>> human_trace_;
  const base::raw_ptr<TraceHookState> trace_hooks_;
  std::unique_ptr<CharacterLoader> character_loader_;
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
  base::TimeTicks last_observed_prune_;
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
    uint32_t active_item_spell_id = 0;
    // Rest: whether a food or drink aura was observed while resting.
    bool had_food_drink_aura = false;
    // Pre-pull wait state when a party healer is low on mana or drinking.
    HealerManaWaitState healer_mana_wait;
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
    // Whether the last SyncMovingOut saw it moving the bot out.
    bool moving_out = false;
    // Whether the last SyncActStealth saw a stealth-only `use` walking in.
    bool act_stealth = false;
    // Whether the current `active_option` holds a grouped-follower errand pause
    // on playerbots `follow` and the 40 yd leash (Milestone 5a).
    bool errand_hold_active = false;
    // Last observed map/instance so map changes clear active acts & `back`.
    bool has_last_map = false;
    uint32_t last_map_id = 0;
    uint32_t last_instance_id = 0;
    // Casts and auras on bot/target/attackers at the last sent snapshot.
    ReactiveSnapshotState last_reactive;
    // Sequence number of the last spell miss already shown in a snapshot.
    uint32_t last_spell_miss_seq = 0;
    // Nearby hostile casts and spell areas at the last look (in combat), when
    // to look again, and whether one appeared that no snapshot showed yet.
    std::vector<std::pair<uint64_t, uint32_t>> known_hazards;
    base::TimeTicks next_hazard_scan;
    bool new_hazard = false;
    // Players who recently chatted to/near this bot (for group invite options).
    std::deque<RecentChatter> recent_chatters;
    // Timestamps of recent outbound bot chat messages (for rate limiting).
    std::deque<base::TimeTicks> recent_chat_sent;
    struct PendingChat {
      uint32_t command_id = 0;
      wlb::snapshot::Command::Say say;
    };
    std::deque<PendingChat> pending_chats;
    // Leader name and arrival time of the currently surfaced pending invite.
    std::string seen_group_invite_leader;
    base::TimeTicks pending_invite_since;
    // Which timed popups the harness was already told about.
    PopupWatch popups;
    // Active `rti` override from a leader chat command (reset after combat,
    // map change, or disconnect).
    RtiOverrideState rti_override;
    // Active "stop dps" pause from a leader chat command (reset on resume,
    // attack command, combat end, map change, or disconnect).
    StopDpsState stop_dps;
    // Active leader "wait"/"stop" stay hold (expires after timeout out of
    // combat, or on follow, group leave, map change, or disconnect).
    LeaderWaitState leader_wait;
  };
  std::vector<BotState> bots_;
  // A wrong-version command was logged on this connection.
  bool wrong_version_logged_ = false;
  // The connected client's hello named another build (reset on reconnect).
  bool harness_refused_ = false;
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
  llm_brain::AddLlmBrainBotManagerScripts();
  llm_brain::AddLlmBrainChatScripts(inbox, human_trace);
  llm_brain::AddLlmBrainTraceScripts(trace_hooks);
#if defined(LLM_BRAIN_TEST_HOOKS)
  llm_brain::AddLlmBrainTestCommandScripts();
#endif
}
