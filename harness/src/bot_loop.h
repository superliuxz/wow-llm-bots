// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Per-bot state and the live decision loop (`docs/plans/harness.md`, H4).
// Receives snapshots, immediate events, and periodic timer ticks on one
// sequence; evaluates default triggers and active watches (`triggers.h`);
// schedules brain calls by urgency with soft slot affinity (`scheduler.h`);
// dispatches `Command.Choose` immediately on the streamed `do:` line
// (`plan_runner.h`); logs every decision with `CLOCK_MONOTONIC` snapshot ages
// (`decision_log.h`).

#ifndef HARNESS_BOT_LOOP_H_
#define HARNESS_BOT_LOOP_H_

#include <cstddef>
#include <cstdint>
#include <deque>
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
#include "decision_log.h"
#include "llm_client.h"
#include "plan_runner.h"
#include "proto/snapshot.h"
#include "scheduler.h"
#include "screen_renderer.h"
#include "triggers.h"

namespace harness {

struct BotLoopConfig {
  std::string rules;          // Shared RULES.md text.
  std::string bots_dir;       // Optional: var/bots (identity.md + logs).
  size_t max_in_flight = 16;  // llama-server slots / concurrent calls.
  float temperature = 0.7f;
  size_t max_recent_events = 12;
  // A bot with no snapshot for this long has left the world (logged out,
  // loading screen, server trouble): its last screen is no longer true, so it
  // isn't asked about it; it gets a first look when snapshots come back.
  // Snapshots come every 1 s (LlmBrain.SnapshotIntervalMs).
  base::TimeDelta stale_snapshot_after = base::Seconds(5);
  // Routine looking the harness does with no brain call (spec decision 12,
  // docs/plans/options-and-panels.md); defaults a player has, parameters a
  // brain can change later.
  struct Habits {
    bool open_bags_when_full = true;     // bag_free_slots reaches 0.
    bool glance_at_quest_log = true;     // The quest tracker changed.
    bool close_panels_in_combat = true;  // Combat starts.
  } habits;
  // How long a reply's `open:` / `hover:` waits for the snapshot that shows
  // it before the brain is woken to read it; then the look is given up.
  base::TimeDelta look_wait = base::Seconds(3);
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
    std::string current_goal;
    std::deque<std::string> recent_events;
    std::vector<std::string> pending_reasons;
    // Who whispered the bot lately, newest first: the names its `tell` line
    // may answer (kWhispererMemory, kMaxWhisperers in bot_loop.cc).
    struct Whisperer {
      std::string name;
      base::TimeTicks at;
    };
    std::deque<Whisperer> whisperers;

    TriggerEvaluator triggers;
    PlanRunner runner;

    bool has_ever_woken = false;
    bool has_queued_wake = false;
    bool call_in_flight = false;
    bool early_steps_dispatched = false;
    bool early_step_failed = false;
    // Refused steps in a row since the last completed one; from the second,
    // the wake is delayed (StepRefusalBackoff()) so a body that can't act
    // (dead, out of range) doesn't ask the brain again many times a second.
    int step_refusals = 0;
    uint64_t call_epoch = 0;
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
    // Panels a habit opened only for a glance: closed once seen.
    std::vector<wlb::snapshot::Panel> glancing;
    bool was_in_combat = false;
    uint32_t last_bag_free_slots = 1;
    std::string last_quest_state;
  };

  void NoteEvent(BotState* state, const wlb::snapshot::Event& event);
  // Remembers who whispered (for the `tell` grammar).
  static void NoteWhisperer(BotState* state,
                            const wlb::snapshot::Event& event,
                            base::TimeTicks now);
  // Puts a call's wake reasons and recent events back in front of any newer
  // ones, for a call that ended without installing a plan, so the next call
  // still sees them (a whisper is not lost to a cancelled or failed call).
  void Requeue(BotState* state,
               std::vector<std::string> reasons,
               std::vector<std::string> events);
  void WakeBot(const std::string& bot,
               Urgency urgency,
               const std::string& reason);
  // Returns a closure that cancels the started call (empty if none started).
  base::OnceClosure StartBotCall(const std::string& bot,
                                 int slot,
                                 base::OnceClosure done);
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
               std::string wake_reason,
               std::vector<std::string> asked_reasons,
               std::vector<std::string> asked_events,
               LlmRequest request,
               int64_t snapshot_monotonic_us,
               std::optional<double> age_at_ask_ms,
               double age_at_first_line_ms,
               base::OnceClosure done,
               LlmResult result);
  void DispatchRunnerAction(const std::string& bot,
                            BotState* state,
                            const RunnerAction& action);
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
  static std::vector<std::string> RememberedPanels(const BotState& state,
                                                   base::TimeTicks now);
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
  Scheduler scheduler_;
  std::flat_map<std::string, BotState> bots_;
  uint32_t next_command_id_ = 0;
  uint64_t decisions_completed_ = 0;
  uint64_t calls_cancelled_ = 0;
  uint64_t commands_sent_ = 0;
  base::WeakPtrFactory<BotLoop> weak_factory_{this};
};

}  // namespace harness

#endif  // HARNESS_BOT_LOOP_H_
