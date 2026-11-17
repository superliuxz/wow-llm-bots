// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_loop.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <flat_set>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "decision_log.h"
#include "file_util.h"
#include "llm_client.h"
#include "plan.h"
#include "plan_runner.h"
#include "prompt.h"
#include "proto/snapshot.h"
#include "scheduler.h"
#include "screen_renderer.h"
#include "triggers.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

std::optional<double> AgeMsSince(int64_t snapshot_monotonic_us,
                                 base::TimeTicks at) {
  if (snapshot_monotonic_us <= 0) {
    return std::nullopt;
  }
  const base::TimeTicks built =
      base::TimeTicks() + base::Microseconds(snapshot_monotonic_us);
  return (at - built).InMicroseconds() / 1000.0;
}

std::flat_set<std::string> CollectExpectedNames(
    const std::vector<PlanStep>& steps,
    const std::vector<int>& expect,
    const Screen& screen) {
  std::flat_set<std::string> names;
  for (int opt : expect) {
    if (opt >= 1 && static_cast<size_t>(opt) <= screen.details.size()) {
      const std::string& name =
          screen.details[static_cast<size_t>(opt - 1)].name;
      if (!name.empty()) {
        names.insert(name);
      }
    }
  }
  for (const PlanStep& step : steps) {
    if (step.option >= 1 &&
        static_cast<size_t>(step.option) <= screen.details.size()) {
      const ScreenOption& d =
          screen.details[static_cast<size_t>(step.option - 1)];
      if (d.kind == OptionKind::kAttack && !d.name.empty()) {
        names.insert(d.name);
      }
    }
  }
  return names;
}

}  // namespace

BotLoop::BotLoop(BotLoopConfig config,
                 AsyncAskFn ask,
                 SendCommandFn send_command,
                 LogMessageFn log_message)
    : config_(std::move(config)),
      ask_(std::move(ask)),
      send_command_(std::move(send_command)),
      log_message_(std::move(log_message)),
      decision_log_(config_.bots_dir.empty()
                        ? nullptr
                        : std::make_unique<DecisionLog>(config_.bots_dir)),
      scheduler_(config_.max_in_flight) {}

BotLoop::~BotLoop() = default;

std::string BotLoop::CurrentGoal(const std::string& bot) const {
  const auto it = bots_.find(bot);
  return it == bots_.end() ? "" : it->second.current_goal;
}

void BotLoop::OnConnectionChanged(bool connected) {
  Log(connected ? "harness: connected to mod-llm-brain"
                : "harness: disconnected from mod-llm-brain");
  if (!connected) {
    for (auto&& [name, state] : bots_) {
      state.runner.Stop();
      state.latest_snapshot.reset();
      state.look_deadline = base::TimeTicks();
      state.has_ever_woken = false;
      state.has_queued_wake = false;
      state.call_in_flight = false;
      state.early_steps_dispatched = false;
      state.early_step_failed = false;
      state.step_refusals = 0;
      state.pending_reasons.clear();
      state.recent_events.clear();
      state.whisperers.clear();
      ++state.call_epoch;
    }
    // Answers to screens of the old connection are useless: free the slots.
    scheduler_.CancelAllInFlight();
  }
}

void BotLoop::NoteEvent(BotState* state, const pb::Event& event) {
  const std::string text = DescribeEvent(event);
  if (text.empty()) {
    return;
  }
  if (!state->recent_events.empty() && state->recent_events.back() == text) {
    return;
  }
  state->recent_events.push_back(text);
  while (state->recent_events.size() >
         std::max<size_t>(config_.max_recent_events, 1)) {
    state->recent_events.pop_front();
  }
}

// How long and how many whisperers a bot can answer with `tell`: a player
// answers the last few people who talked to them, not everyone from an hour
// ago.
constexpr base::TimeDelta kWhispererMemory = base::Seconds(600);
constexpr size_t kMaxWhisperers = 4;

// static
void BotLoop::NoteWhisperer(BotState* state,
                            const pb::Event& event,
                            base::TimeTicks now) {
  if (!event.chat || event.chat->channel != pb::WHISPER ||
      event.chat->from.empty()) {
    return;
  }
  const std::string& from = event.chat->from;
  std::erase_if(state->whisperers,
                [&](const BotState::Whisperer& w) { return w.name == from; });
  state->whisperers.push_front({from, now});
  if (state->whisperers.size() > kMaxWhisperers) {
    state->whisperers.pop_back();
  }
}

void BotLoop::Requeue(BotState* state,
                      std::vector<std::string> reasons,
                      std::vector<std::string> events) {
  for (std::string& r : state->pending_reasons) {
    if (std::find(reasons.begin(), reasons.end(), r) == reasons.end()) {
      reasons.push_back(std::move(r));
    }
  }
  state->pending_reasons = std::move(reasons);
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    state->recent_events.push_front(std::move(*it));
  }
  while (state->recent_events.size() >
         std::max<size_t>(config_.max_recent_events, 1)) {
    state->recent_events.pop_front();
  }
}

void BotLoop::OnSnapshot(pb::Snapshot snapshot, base::TimeTicks received_at) {
  const std::string bot = snapshot.bot;
  if (bot.empty()) {
    return;
  }
  BotState& state = bots_[bot];
  for (const pb::Event& ev : snapshot.events) {
    NoteEvent(&state, ev);
    NoteWhisperer(&state, ev, received_at);
  }
  Screen screen = RenderScreen(snapshot);
  const std::optional<WakeTrigger> trig =
      state.triggers.EvaluateSnapshot(snapshot);
  state.latest_snapshot = std::move(snapshot);
  state.latest_screen = std::move(screen);
  state.latest_received_at = received_at;

  if (!state.has_ever_woken) {
    state.has_ever_woken = true;
    WakeBot(bot, trig ? trig->urgency : Urgency::kNormal,
            trig ? trig->reason : "first look");
    return;
  }
  if (trig) {
    state.runner.Stop();
    state.look_deadline = base::TimeTicks();
    WakeBot(bot, trig->urgency, trig->reason);
    return;
  }
  if (state.call_in_flight || state.has_queued_wake) {
    return;
  }
  const RunnerAction action = state.runner.OnSnapshot(
      *state.latest_snapshot, state.latest_screen, received_at);
  if (action.step_done) {
    state.step_refusals = 0;
  }
  DispatchRunnerAction(bot, &state, action);
  if (!state.has_queued_wake && !state.call_in_flight &&
      !state.look_deadline.is_null() && received_at >= state.look_deadline) {
    state.look_deadline = base::TimeTicks();
    state.runner.Stop();
    const bool backoff_wake = !state.pending_reasons.empty();
    if (!backoff_wake) {
      state.step_refusals = 0;
    }
    WakeBot(bot, Urgency::kNormal, backoff_wake ? "" : "look timer");
  }
}

base::TimeDelta StepRefusalBackoff(int refusals) {
  if (refusals <= 1) {
    return base::TimeDelta();
  }
  constexpr int kMaxSeconds = 30;
  const int shift = std::min(refusals - 1, 5);
  return base::Seconds(std::min(1 << shift, kMaxSeconds));
}

void BotLoop::OnEvent(pb::ImmediateEvent imm, base::TimeTicks received_at) {
  const std::string& bot = imm.bot;
  if (bot.empty()) {
    return;
  }
  BotState& state = bots_[bot];
  NoteEvent(&state, imm.event);
  NoteWhisperer(&state, imm.event, received_at);
  if (imm.event.error && imm.event.error->command_id != 0) {
    if (!state.runner.active() ||
        imm.event.error->command_id != state.runner.last_command_id()) {
      return;
    }
    state.runner.Stop();
    if (state.call_in_flight) {
      state.early_step_failed = true;
    }
    const base::TimeDelta backoff = StepRefusalBackoff(++state.step_refusals);
    if (backoff.is_positive()) {
      if (const std::optional<WakeTrigger> trig =
              EvaluateEventTrigger(imm.event, state.triggers.watches());
          trig &&
          std::find(state.pending_reasons.begin(), state.pending_reasons.end(),
                    trig->reason) == state.pending_reasons.end()) {
        state.pending_reasons.push_back(trig->reason);
      }
      state.look_deadline = received_at + backoff;
      Log(base::StringPrintf(
          "harness: %s step refused %d times in a row; "
          "next look in %.0f s",
          bot.c_str(), state.step_refusals, backoff.InSecondsF()));
      return;
    }
  }
  const std::optional<WakeTrigger> trig =
      EvaluateEventTrigger(imm.event, state.triggers.watches());
  if (!trig) {
    return;
  }
  state.runner.Stop();
  state.look_deadline = base::TimeTicks();
  if (state.latest_snapshot) {
    WakeBot(bot, trig->urgency, trig->reason);
  } else if (std::find(state.pending_reasons.begin(),
                       state.pending_reasons.end(),
                       trig->reason) == state.pending_reasons.end()) {
    state.pending_reasons.push_back(trig->reason);
  }
}

void BotLoop::OnTick(base::TimeTicks now) {
  for (auto&& [bot, state] : bots_) {
    if (state.latest_snapshot &&
        now - state.latest_received_at > config_.stale_snapshot_after) {
      Log(base::StringPrintf(
          "harness: %s: no snapshot for %.0f s; waiting "
          "for it to come back",
          bot.c_str(), (now - state.latest_received_at).InSecondsF()));
      state.runner.Stop();
      state.latest_snapshot.reset();
      state.look_deadline = base::TimeTicks();
      state.has_ever_woken = false;
      state.has_queued_wake = false;
      state.call_in_flight = false;
      state.early_steps_dispatched = false;
      state.early_step_failed = false;
      state.step_refusals = 0;
      state.pending_reasons.clear();
      state.recent_events.clear();
      ++state.call_epoch;
      continue;
    }
    if (!state.latest_snapshot || state.call_in_flight ||
        state.has_queued_wake) {
      continue;
    }
    if (const std::optional<std::string> timeout =
            state.runner.CheckTimeout(now)) {
      state.look_deadline = base::TimeTicks();
      WakeBot(bot, Urgency::kNormal, *timeout);
      continue;
    }
    if (!state.look_deadline.is_null() && now >= state.look_deadline) {
      state.look_deadline = base::TimeTicks();
      state.runner.Stop();
      const bool backoff_wake = !state.pending_reasons.empty();
      if (!backoff_wake) {
        state.step_refusals = 0;
      }
      WakeBot(bot, Urgency::kNormal, backoff_wake ? "" : "look timer");
    }
  }
}

void BotLoop::WakeBot(const std::string& bot,
                      Urgency urgency,
                      const std::string& reason) {
  BotState& state = bots_[bot];
  if (!reason.empty() &&
      std::find(state.pending_reasons.begin(), state.pending_reasons.end(),
                reason) == state.pending_reasons.end()) {
    state.pending_reasons.push_back(reason);
  }
  state.has_queued_wake = true;
  base::WeakPtr<BotLoop> self = weak_factory_.GetWeakPtr();
  scheduler_.SubmitCancelable(
      bot, urgency, [self, bot](int slot, base::OnceClosure done) {
        if (!self) {
          std::move(done).Run();
          return base::OnceClosure{};
        }
        return self->StartBotCall(bot, slot, std::move(done));
      });
}

base::OnceClosure BotLoop::StartBotCall(const std::string& bot,
                                        int slot,
                                        base::OnceClosure done) {
  auto it = bots_.find(bot);
  if (it == bots_.end() || !it->second.latest_snapshot) {
    if (it != bots_.end()) {
      it->second.has_queued_wake = false;
      it->second.pending_reasons.clear();
    }
    std::move(done).Run();
    return base::OnceClosure{};
  }
  BotState& state = it->second;
  if (!state.identity_loaded) {
    state.identity_loaded = true;
    if (!config_.bots_dir.empty()) {
      if (const std::optional<std::string> id = ReadFile(
              base::StrCat({config_.bots_dir, "/", bot, "/identity.md"}))) {
        state.identity = *id;
      }
    }
  }

  state.has_queued_wake = false;
  state.call_in_flight = true;
  state.early_steps_dispatched = false;
  state.early_step_failed = false;
  const uint64_t epoch = ++state.call_epoch;

  const std::string wake_reason =
      state.pending_reasons.empty()
          ? "plan done"
          : base::JoinString(state.pending_reasons, "; ");
  std::vector<std::string> asked_reasons = std::move(state.pending_reasons);
  state.pending_reasons.clear();

  const base::TimeTicks asked_at_for_whisperers = base::TimeTicks::Now();
  PromptInput input;
  input.rules = config_.rules;
  input.bot_name = bot;
  input.identity = state.identity;
  input.current_goal = state.current_goal;
  input.wake_reason = wake_reason;
  input.recent_events.assign(state.recent_events.begin(),
                             state.recent_events.end());
  state.recent_events.clear();
  std::vector<std::string> asked_events = input.recent_events;
  for (const BotState::Whisperer& w : state.whisperers) {
    if (asked_at_for_whisperers - w.at <= kWhispererMemory) {
      input.whisperers.push_back(w.name);
    }
  }
  input.slot = slot;
  input.temperature = config_.temperature;

  const uint32_t ask_seq = state.latest_snapshot->seq;
  const int64_t snap_mono_us = state.latest_snapshot->monotonic_us;
  const Screen ask_screen = state.latest_screen;
  const base::TimeTicks asked_at = base::TimeTicks::Now();
  const std::optional<double> age_at_ask_ms =
      AgeMsSince(snap_mono_us, asked_at);

  LlmRequest request = BuildPlanRequest(input, ask_screen);
  LlmRequest request_copy = request;

  // Shared strictly on this single sequence between OnFirstLine and OnReply.
  auto first_line_age_holder = std::make_unique<double>(0.0);
  double* first_line_age_ptr = first_line_age_holder.get();

  base::WeakPtr<BotLoop> self = weak_factory_.GetWeakPtr();
  FirstLineCallback on_first_line =
      [self, bot, epoch, ask_seq, ask_screen, snap_mono_us,
       first_line_age_ptr](std::string_view line) {
        if (self) {
          self->OnFirstLine(bot, epoch, ask_seq, ask_screen, snap_mono_us,
                            first_line_age_ptr, line);
        }
      };

  base::scoped_refptr<LlmCancelHandle> handle = ask_.Run(
      std::move(request),
      [self, bot, epoch, ask_seq, ask_screen, wake_reason,
       asked_reasons = std::move(asked_reasons),
       asked_events = std::move(asked_events),
       request_copy = std::move(request_copy), snap_mono_us, age_at_ask_ms,
       holder = std::move(first_line_age_holder),
       done = std::move(done)](LlmResult result) mutable {
        if (!self) {
          std::move(done).Run();
          return;
        }
        self->OnReply(bot, epoch, ask_seq, std::move(ask_screen),
                      std::move(wake_reason), std::move(asked_reasons),
                      std::move(asked_events), std::move(request_copy),
                      snap_mono_us, age_at_ask_ms, *holder, std::move(done),
                      std::move(result));
      },
      std::move(on_first_line));
  if (!handle) {
    return base::OnceClosure{};
  }
  // Cancel() is thread-safe and never runs the reply inline: the reply (with
  // `cancelled` set) arrives later on this sequence and releases the slot.
  return [handle = std::move(handle)]() { handle->Cancel(); };
}

void BotLoop::OnFirstLine(const std::string& bot,
                          uint64_t epoch,
                          uint32_t ask_seq,
                          const Screen& ask_screen,
                          int64_t snapshot_monotonic_us,
                          double* out_age_at_first_line_ms,
                          std::string_view line) {
  if (const auto age =
          AgeMsSince(snapshot_monotonic_us, base::TimeTicks::Now())) {
    *out_age_at_first_line_ms = *age;
  }
  auto it = bots_.find(bot);
  if (it == bots_.end()) {
    return;
  }
  BotState& state = it->second;
  if (!state.call_in_flight || state.call_epoch != epoch ||
      state.has_queued_wake || !state.latest_snapshot) {
    return;
  }
  const std::optional<std::vector<PlanStep>> steps =
      ParsePlanStepsLine(line, static_cast<int>(ask_screen.options.size()));
  if (!steps) {
    return;
  }
  state.early_steps_dispatched = true;
  // Provisionally mark attacked targets as expected while the rest of the plan
  // streams, so entering combat before `expect:` arrives does not re-wake.
  state.triggers.SetExpectedNames(CollectExpectedNames(*steps, {}, ask_screen));
  const RunnerAction action =
      state.runner.Start(*steps, ask_seq, ask_screen, base::TimeTicks::Now());
  DispatchRunnerAction(bot, &state, action);
}

void BotLoop::OnReply(const std::string& bot,
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
                      LlmResult result) {
  const base::TimeTicks now = base::TimeTicks::Now();
  if (result.cancelled) {
    // Preempted by a more urgent wake, or the module disconnected: not a
    // decision and not a brain error, so no retry timer. A newer wake (if any)
    // is already queued and starts once `done` frees the slot.
    if (auto it = bots_.find(bot);
        it != bots_.end() && it->second.call_epoch == epoch) {
      it->second.call_in_flight = false;
      Requeue(&it->second, std::move(asked_reasons), std::move(asked_events));
    }
    ++calls_cancelled_;
    Log(base::StringPrintf("harness: %s call cancelled after %.0f ms",
                           bot.c_str(), result.round_trip_ms));
    std::move(done).Run();
    return;
  }
  const std::optional<double> age_at_plan_ms =
      AgeMsSince(snapshot_monotonic_us, now);

  std::optional<Plan> plan;
  if (result.reply) {
    plan = ParsePlan(result.reply->text,
                     static_cast<int>(ask_screen.options.size()));
  }

  Decision decision;
  decision.bot = bot;
  decision.reason = std::move(wake_reason);
  decision.request = std::move(request);
  decision.result = std::move(result);
  decision.plan = plan;
  decision.time = base::Time::Now();
  decision.age_at_ask_ms = age_at_ask_ms;
  if (age_at_first_line_ms > 0) {
    decision.age_at_first_line_ms = age_at_first_line_ms;
  } else if (age_at_plan_ms) {
    decision.age_at_first_line_ms = age_at_plan_ms;
  }
  decision.age_at_plan_ms = age_at_plan_ms;
  if (decision_log_) {
    decision_log_->Record(decision);
  }
  ++decisions_completed_;

  auto it = bots_.find(bot);
  if (it != bots_.end() && it->second.call_epoch == epoch) {
    BotState& state = it->second;
    state.call_in_flight = false;
    if (!decision.result.reply) {
      Log(base::StringPrintf("harness: %s brain error (%.0f ms): %s",
                             bot.c_str(), decision.result.round_trip_ms,
                             decision.result.error.c_str()));
      state.look_deadline = now + base::Seconds(10);
      Requeue(&state, std::move(asked_reasons), std::move(asked_events));
    } else if (!plan) {
      Log(base::StringPrintf("harness: %s invalid plan (%.0f ms)", bot.c_str(),
                             decision.result.round_trip_ms));
      state.look_deadline = now + base::Seconds(10);
      Requeue(&state, std::move(asked_reasons), std::move(asked_events));
    } else {
      if (!plan->goal.empty()) {
        state.current_goal = plan->goal;
      }
      const std::string formatted = FormatPlan(*plan);
      const std::string do_line = formatted.substr(0, formatted.find('\n'));
      Log(base::StringPrintf(
          "harness: %s woke (%s) -> %s (do %.0f ms, total %.0f ms, look %d s)",
          bot.c_str(), decision.reason.c_str(), do_line.c_str(),
          decision.result.first_line_ms, decision.result.round_trip_ms,
          plan->look_s));
      // Only install watches/steps if no newer surprise already superseded this
      // call while it was in flight. When an early-dispatched step of this
      // same call failed before the reply finished, still deliver say/tell and
      // watches, and keep the refusal wake or backoff timer instead of
      // requeueing the already-answered wake reasons.
      if ((!state.has_queued_wake || state.early_step_failed) &&
          state.latest_snapshot) {
        state.triggers.SetPlanContext(
            plan->watches,
            state.early_step_failed
                ? std::flat_set<std::string>{}
                : CollectExpectedNames(plan->steps, plan->expect, ask_screen));
        if (!state.early_step_failed) {
          state.look_deadline = now + base::Seconds(plan->look_s);
        }
        if (!plan->say.empty()) {
          SendSay(bot, pb::SAY, "", plan->say);
        }
        if (!plan->tell.empty()) {
          SendSay(bot, pb::WHISPER, plan->tell_to, plan->tell);
        }
        if (!state.early_steps_dispatched) {
          const RunnerAction action =
              state.runner.Start(plan->steps, ask_seq, ask_screen, now);
          DispatchRunnerAction(bot, &state, action);
        }
      } else {
        // Superseded by a newer wake while in flight: that call answers.
        Requeue(&state, std::move(asked_reasons), std::move(asked_events));
      }
    }
  }
  std::move(done).Run();
}

void BotLoop::DispatchRunnerAction(const std::string& bot,
                                   BotState* state,
                                   const RunnerAction& action) {
  if (action.choose) {
    const uint32_t id = SendChoose(bot, *action.choose);
    state->runner.set_last_command_id(id);
  }
  if (!action.finished_reason.empty()) {
    state->look_deadline = base::TimeTicks();
    WakeBot(bot, Urgency::kNormal, action.finished_reason);
  }
}

uint32_t BotLoop::SendChoose(const std::string& bot,
                             const pb::Command::Choose& choose) {
  pb::Command cmd;
  cmd.bot = bot;
  const uint32_t id = ++next_command_id_;
  cmd.command_id = id;
  cmd.choose = choose;
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
  return id;
}

void BotLoop::SendSay(const std::string& bot,
                      pb::ChatChannel channel,
                      const std::string& to,
                      const std::string& text) {
  pb::Command cmd;
  cmd.bot = bot;
  cmd.command_id = ++next_command_id_;
  pb::Command::Say& say = cmd.say.emplace();
  say.channel = channel;
  say.to = to;
  say.text = text;
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
}

void BotLoop::Log(const std::string& line) {
  if (log_message_) {
    log_message_.Run(line);
  }
}

}  // namespace harness
