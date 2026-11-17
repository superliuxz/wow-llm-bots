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

    TriggerEvaluator triggers;
    PlanRunner runner;

    bool has_ever_woken = false;
    bool has_queued_wake = false;
    bool call_in_flight = false;
    bool early_steps_dispatched = false;
    uint64_t call_epoch = 0;
    base::TimeTicks look_deadline;
  };

  void NoteEvent(BotState* state, const wlb::snapshot::Event& event);
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
