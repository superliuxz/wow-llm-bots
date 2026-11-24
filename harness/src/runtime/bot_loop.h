// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Per-bot state and the live decision loop (`docs/specs/harness.md`, H4).
// Receives snapshots, immediate events, and periodic timer ticks on one
// sequence; evaluates default triggers and active watches (`triggers.h`);
// schedules brain calls by urgency with soft slot affinity (`scheduler.h`);
// dispatches `Command.Choose` immediately on the streamed `do:` line
// (`plan_runner.h`); logs every decision with `CLOCK_MONOTONIC` snapshot ages
// (`decision_log.h`).

#ifndef HARNESS_RUNTIME_BOT_LOOP_H_
#define HARNESS_RUNTIME_BOT_LOOP_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <flat_map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "llm/llm_client.h"
#include "llm/scheduler.h"
#include "plan/order_evaluator.h"
#include "plan/orders.h"
#include "plan/plan_runner.h"
#include "plan/triggers.h"
#include "runtime/decision_log.h"
#include "screen/knowledge.h"
#include "screen/screen_renderer.h"
#include "snapshot/snapshot.h"

namespace harness {

struct BotLoopConfig {
  std::string rules;      // Shared RULES.md text.
  std::string bots_dir;   // Optional: var/bots (identity.md + logs).
  std::string notes_dir;  // Optional: content/notes (<instance>/notes.md).
  // Optional: content/identities (<bot>.md), used when <bots_dir>/<bot>/
  // identity.md is missing (RH-M4).
  std::string identities_dir;
  std::string knowledge_path;  // Optional: var/knowledge/world_knowledge.json.
  size_t max_in_flight = 16;   // llama-server slots / concurrent calls.
  float temperature = 0.7f;
  size_t max_recent_events = 12;
  // A bot with no snapshot for this long has left the world (logged out,
  // loading screen, server trouble): its last screen is no longer true, so it
  // isn't asked about it; it gets a first look when snapshots come back.
  // Snapshots come every 1 s (LlmBrain.SnapshotIntervalMs).
  base::TimeDelta stale_snapshot_after = base::Seconds(5);
  // Routine looking the harness does with no brain call (spec decision 12,
  // docs/specs/options.md); defaults a player has, parameters a
  // brain can change later.
  struct Habits {
    bool open_bags_when_full = true;     // bag_free_slots reaches 0.
    bool glance_at_quest_log = true;     // The quest tracker changed.
    bool close_panels_in_combat = true;  // Combat starts.
  } habits;
  // How long a reply's `open:` / `hover:` waits for the snapshot that shows
  // it before the brain is woken to read it; then the look is given up.
  base::TimeDelta look_wait = base::Seconds(3);
  // How long a closed panel stays in the prompt's memory ("You remember");
  // then it is forgotten (the prompt has to fit the slot: review of f40b0b6,
  // finding 7).
  base::TimeDelta remember_panels_for = base::Minutes(5);
  // Condense older `remember:` notes in `memory.md` into an `## Earlier
  // summary` section via a P4 LLM call once recent notes reach this threshold
  // (0 disables condensing), keeping the newest `memory_keep_recent` raw lines
  // verbatim (F5).
  size_t memory_condense_threshold = 24;
  size_t memory_keep_recent = 10;
  // Overall wall-clock deadlines for in-flight LLM calls (A-M13): a slow or
  // wedged llama-server is cancelled on tick rather than stalling a bot for
  // 60 s.
  base::TimeDelta combat_call_timeout = base::Seconds(10);
  base::TimeDelta call_timeout = base::Seconds(30);
};

// Starts one brain call. Returns a handle whose Cancel() aborts it early (the
// reply then still arrives, with `LlmResult::cancelled` set); may return null
// when the call can't be cancelled.
using AsyncAskFn = base::RepeatingCallback<base::scoped_refptr<LlmCancelHandle>(
    LlmRequest request,
    base::OnceCallback<void(LlmResult)> reply,
    FirstLineCallback on_first_line)>;

using SendCommandFn =
    base::RepeatingCallback<void(const wlb::snapshot::Command&)>;

using LogMessageFn = base::RepeatingCallback<void(const std::string&)>;

// How long the wake after the `refusals`-th refused step in a row waits: none
// for the first, then 2, 4, 8, 16, 30, 30... seconds.
base::TimeDelta StepRefusalBackoff(int refusals);

class BotLoop {
 public:
  BotLoop(BotLoopConfig config,
          AsyncAskFn ask,
          SendCommandFn send_command,
          LogMessageFn log_message = {});
  BotLoop(const BotLoop&) = delete;
  BotLoop& operator=(const BotLoop&) = delete;
  ~BotLoop();

  void OnConnectionChanged(bool connected);
  void OnSnapshot(wlb::snapshot::Snapshot snapshot,
                  base::TimeTicks received_at);
  void OnEvent(wlb::snapshot::ImmediateEvent event,
               base::TimeTicks received_at);
  void OnTick(base::TimeTicks now);

  size_t bot_count() const { return bots_.size(); }
  size_t in_flight() const { return scheduler_.in_flight(); }
  size_t queued() const { return scheduler_.queued(); }
  uint64_t decisions_completed() const { return decisions_completed_; }
  uint64_t calls_cancelled() const { return calls_cancelled_; }
  uint64_t commands_sent() const { return commands_sent_; }
  std::string CurrentGoal(const std::string& bot) const;

 private:
  struct BotState {
    std::optional<wlb::snapshot::Snapshot> latest_snapshot;
    Screen latest_screen;
    base::TimeTicks latest_received_at;

    bool identity_loaded = false;
    std::string identity;
    std::string notes;
    std::string current_goal;
    std::string last_outcome;
    std::deque<std::string> recent_events;
    std::vector<std::string> pending_reasons;
    // Structured chat metadata recorded directly from `pb::ChatEvent` so reply
    // routing, party-chat responder selection, and relationship attribution
    // never re-parse joined wake strings (A-M14, A-MH6).
    struct ObservedChat {
      wlb::snapshot::ChatChannel channel =
          wlb::snapshot::CHAT_CHANNEL_UNSPECIFIED;
      std::string from;
      std::string text;
      bool at_me = false;
      bool triggered_wake = false;
    };
    std::vector<ObservedChat> pending_chats;
    // Per-sender recent chat wake timestamps for spam rate limiting (A-M8).
    std::flat_map<std::string, std::deque<base::TimeTicks>>
        chat_wake_times_by_sender;
    // Who whispered the bot lately, newest first: the names its `tell` line
    // may answer (kWhispererMemory, kMaxWhisperers in bot_loop.cc).
    struct Whisperer {
      std::string name;
      base::TimeTicks at;
    };
    std::deque<Whisperer> whisperers;

    TriggerEvaluator triggers;
    // The watch inputs of the snapshot the call in flight asked about: its
    // plan's watches are latched from them (a crossing during the call fires).
    TriggerEvaluator::WatchInputs ask_watch_inputs;
    PlanRunner runner;
    OrderEvaluator orders;

    // Explicit brain-call lifecycle state (replaces separate `call_in_flight` +
    // `has_queued_wake` booleans so transitions and resets stay consistent).
    enum class CallState {
      kIdle,
      kQueued,
      kInFlight,
      kInFlightQueued,
    };
    CallState call_state = CallState::kIdle;
    bool has_queued_wake() const {
      return call_state == CallState::kQueued ||
             call_state == CallState::kInFlightQueued;
    }
    bool call_in_flight() const {
      return call_state == CallState::kInFlight ||
             call_state == CallState::kInFlightQueued;
    }
    void MarkQueued() {
      call_state =
          call_in_flight() ? CallState::kInFlightQueued : CallState::kQueued;
    }
    void ClearQueued() {
      call_state = call_in_flight() ? CallState::kInFlight : CallState::kIdle;
    }
    void MarkStarted() { call_state = CallState::kInFlight; }
    void MarkFinished() {
      call_state = has_queued_wake() ? CallState::kQueued : CallState::kIdle;
    }

    bool has_ever_woken = false;
    Urgency queued_urgency = Urgency::kP2Routine;
    base::scoped_refptr<LlmCancelHandle> in_flight_cancel;
    base::TimeTicks call_deadline;
    uint64_t call_timed_out_epoch = 0;
    bool early_steps_dispatched = false;
    bool early_step_failed = false;
    bool order_failure_during_call = false;
    // Refused steps in a row since the last completed one; from the second,
    // the wake is delayed (StepRefusalBackoff()) so a body that can't act
    // (dead, out of range) doesn't ask the brain again many times a second.
    int step_refusals = 0;
    // Consecutive order step failures/timeouts since the last completed order
    // step, normal look, or sustained quiet period.
    int order_wakes = 0;
    base::TimeTicks order_quiet_since;
    uint64_t call_epoch = 0;
    // The call (epoch) woken by a party/raid line that another bot answers
    // for the group (RH-H4): its `say:` / `party:` lines are not sent.
    uint64_t party_reply_muted_epoch = 0;
    base::TimeTicks look_deadline;
    // Panels as the bot last saw them, for the prompt once they are closed.
    struct SeenPanel {
      std::vector<std::string> lines;
      base::TimeTicks seen_at;
    };
    std::flat_map<wlb::snapshot::Panel, SeenPanel> seen_panels;
    // A look the brain asked for (`open:` / `hover:`): it is woken again once
    // a snapshot shows it, or the look is dropped at `until`.
    struct PendingLook {
      std::vector<wlb::snapshot::Panel> panels;
      std::string hover;
      base::TimeTicks until;
    };
    std::optional<PendingLook> pending_look;
    // Panels a habit opened only for a glance: closed once seen; given up at
    // `until` if never shown (else the habit would never glance again).
    struct Glance {
      wlb::snapshot::Panel panel;
      base::TimeTicks until;
    };
    std::vector<Glance> glancing;
    bool was_in_combat = false;
    bool was_in_instance = false;
    std::string last_map;
    // Notes slice actually shown to the brain on the last call (used to key
    // compiled_orders_cache_ so truncated entrance notes never pollute the
    // full-file key).
    std::string last_selected_notes;
    // Outbound chat command IDs so chat error events with a real command_id
    // are recognized and wake the brain instead of being dropped as stale.
    std::deque<uint32_t> recent_chat_command_ids;
    uint32_t last_bag_free_slots = 1;
    // The quest tracker's state; nullopt before the first snapshot (no
    // glance for what was there already).
    std::optional<std::string> last_quest_state;
    // Memory condensing (F5): at most one P4 reflection call per bot at a
    // time, scheduled under the bot's own scheduler key (so it does not evict
    // other bots' slots and is preemptible by P0 combat wakes), guarded by
    // `memory_condense_epoch`.
    enum class CondenseState { kIdle, kQueued, kInFlight };
    CondenseState condense_state = CondenseState::kIdle;
    uint64_t memory_condense_epoch = 0;
    base::TimeTicks condense_retry_after;
  };

  struct CompiledOrdersKey {
    std::string bot;
    std::string map;
    std::string notes;
    auto operator<=>(const CompiledOrdersKey&) const = default;
  };

  void NoteEvent(BotState* state,
                 const wlb::snapshot::Event& event,
                 bool triggered_wake = false);
  // A line for the next prompt's "Since your last look".
  void NoteText(BotState* state, std::string text);
  static void ResetBotSession(BotState* state, bool cancel_in_flight);
  // What a habit or a disconnect leaves behind: forgotten.
  static void ResetLooking(BotState* state);
  // `path`'s text, re-read only when its modification time changed (it is
  // read on every brain call: RH-L5); "" when missing.
  std::string ReadFileIfChanged(const std::string& path);
  // Whether `bot` answers the party/raid chat lines among `chats` (RH-H4):
  // a bot named in the line does; when no brain bot of the group is named,
  // only the first of them by name does. True when no such line woke it.
  bool AnswersPartyChat(const std::string& bot,
                        const std::vector<BotState::ObservedChat>& chats,
                        const wlb::snapshot::Snapshot& snapshot) const;
  // Remembers who whispered (for the `tell` grammar).
  static void NoteWhisperer(BotState* state,
                            const wlb::snapshot::Event& event,
                            base::TimeTicks now);
  // Puts a call's wake reasons, chats, and recent events back in front of any
  // newer ones, for a call that ended without installing a plan, so the next
  // call still sees them (a whisper is not lost to a cancelled or failed call).
  void Requeue(BotState* state,
               std::vector<std::string> reasons,
               std::vector<std::string> events,
               std::vector<BotState::ObservedChat> chats,
               std::string outcome);
  // `preserve_backoffs`: keep `step_refusals` and `order_wakes` intact (set
  // for refusal retries, chat wakes, and order-wake backoff so chat spam or
  // repeated refusals never clear refusal backoff).
  void WakeBot(const std::string& bot,
               Urgency urgency,
               const std::string& reason,
               bool preserve_backoffs = false);
  // Returns a closure that cancels the started call (empty if none started).
  base::OnceClosure StartBotCall(const std::string& bot,
                                 int slot,
                                 base::OnceClosure done);
  void MaybeScheduleMemoryCondense(const std::string& bot,
                                   BotState* state,
                                   base::TimeTicks now);
  base::OnceClosure StartMemoryCondense(const std::string& bot,
                                        int slot,
                                        base::OnceClosure done);
  void OnMemoryCondensed(const std::string& bot,
                         uint64_t condense_epoch,
                         size_t condensed_recent_count,
                         base::OnceClosure done,
                         LlmResult result);
  void RecordRelationshipNotes(
      const std::string& bot,
      const BotState& state,
      const std::vector<BotState::ObservedChat>& asked_chats,
      std::string_view date,
      std::string_view memo);
  void OnFirstLine(const std::string& bot,
                   uint64_t epoch,
                   uint32_t ask_seq,
                   const Screen& ask_screen,
                   int64_t snapshot_monotonic_us,
                   double* out_age_at_first_line_ms,
                   std::string_view line);
  void OnReply(const std::string& bot,
               uint64_t epoch,
               uint32_t ask_seq,
               Screen ask_screen,
               std::vector<std::string> order_names,
               std::string wake_reason,
               std::vector<std::string> asked_reasons,
               std::vector<std::string> asked_events,
               std::vector<BotState::ObservedChat> asked_chats,
               std::string asked_outcome,
               LlmRequest request,
               int64_t snapshot_monotonic_us,
               std::optional<double> age_at_ask_ms,
               double age_at_first_line_ms,
               base::OnceClosure done,
               LlmResult result);
  void DispatchRunnerAction(const std::string& bot,
                            BotState* state,
                            const RunnerAction& action);
  bool DispatchOrderEval(const std::string& bot,
                         BotState* state,
                         const OrderEvalResult& eval,
                         base::TimeTicks now,
                         std::string_view extra_wake_reason = {});
  bool WakeForOrder(const std::string& bot,
                    BotState* state,
                    std::string_view reason,
                    base::TimeTicks now,
                    bool is_failure);
  void SendStopMoving(const std::string& bot);
  uint32_t SendAct(const std::string& bot, wlb::snapshot::Command::Act act);
  uint32_t SendChoose(const std::string& bot,
                      const wlb::snapshot::Command::Choose& choose);
  // `to`: the player whispered (channel WHISPER only).
  void SendPanel(const std::string& bot, wlb::snapshot::Panel panel, bool open);
  void SendHover(const std::string& bot, const wlb::snapshot::HoverRef& ref);
  // Remembers open panels, runs the habits and wakes the brain when the look
  // it asked for shows; true when it woke the brain.
  bool OnLookSnapshot(const std::string& bot,
                      BotState* state,
                      base::TimeTicks received_at);
  // The prompt's memory of panels not open now.
  static std::vector<std::string> RememberedPanels(
      const BotState& state,
      base::TimeTicks now,
      base::TimeDelta remember_for);
  void SendSay(const std::string& bot,
               wlb::snapshot::ChatChannel channel,
               const std::string& to,
               const std::string& text);
  void Log(const std::string& line);

  const BotLoopConfig config_;
  AsyncAskFn ask_;
  SendCommandFn send_command_;
  LogMessageFn log_message_;
  std::unique_ptr<DecisionLog> decision_log_;
  WorldKnowledge knowledge_;
  // Lowercased instance map name -> path to `content/notes/<dir>/notes.md`.
  std::flat_map<std::string, std::string> instance_notes_index_;
  Scheduler scheduler_;
  std::flat_map<std::string, BotState> bots_;
  // Pre-pull compiled standing orders cached by (bot, map, notes).
  static constexpr size_t kMaxCompiledOrdersCache = 256;
  // How many of the newest `remember:` notes go into each prompt (RH-M4).
  static constexpr size_t kMemoryLinesInPrompt = 15;
  std::flat_map<CompiledOrdersKey, std::vector<Order>> compiled_orders_cache_;
  struct CachedFile {
    std::filesystem::file_time_type mtime;
    std::string text;
  };
  std::flat_map<std::string, CachedFile> file_cache_;
  uint32_t next_command_id_ = 0;
  uint64_t decisions_completed_ = 0;
  uint64_t calls_cancelled_ = 0;
  uint64_t commands_sent_ = 0;
  base::WeakPtrFactory<BotLoop> weak_factory_{this};
};

}  // namespace harness

#endif  // HARNESS_RUNTIME_BOT_LOOP_H_
