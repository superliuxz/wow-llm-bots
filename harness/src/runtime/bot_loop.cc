// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "runtime/bot_loop.h"

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
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "llm/llm_client.h"
#include "llm/scheduler.h"
#include "plan/order_evaluator.h"
#include "plan/orders.h"
#include "plan/plan.h"
#include "plan/plan_runner.h"
#include "plan/triggers.h"
#include "runtime/decision_log.h"
#include "runtime/file_util.h"
#include "screen/knowledge.h"
#include "screen/prompt.h"
#include "screen/screen_renderer.h"
#include "snapshot/snapshot.h"

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
      scheduler_(config_.max_in_flight) {
  if (!config_.knowledge_path.empty()) {
    std::string err;
    if (std::optional<WorldKnowledge> wk =
            WorldKnowledge::LoadFile(config_.knowledge_path, &err)) {
      knowledge_ = std::move(*wk);
      Log(base::StringPrintf(
          "harness: loaded world knowledge (%zu quests, %zu zones, %zu "
          "dungeons) from %s",
          knowledge_.quest_count(), knowledge_.zone_count(),
          knowledge_.dungeon_count(), config_.knowledge_path.c_str()));
    } else {
      Log(base::StrCat({"harness: world knowledge unavailable (", err, ")"}));
    }
  }
}

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
      state.orders.Clear();
      state.latest_snapshot.reset();
      state.look_deadline = base::TimeTicks();
      state.has_ever_woken = false;
      state.was_in_instance = false;
      state.last_map.clear();
      state.last_selected_notes.clear();
      state.recent_chat_command_ids.clear();
      state.has_queued_wake = false;
      state.call_in_flight = false;
      state.early_steps_dispatched = false;
      state.early_step_failed = false;
      state.step_refusals = 0;
      state.last_outcome.clear();
      state.order_wakes = 0;
      state.pending_reasons.clear();
      state.recent_events.clear();
      state.whisperers.clear();
      ResetLooking(&state);
      ++state.call_epoch;
    }
    // Answers to screens of the old connection are useless: free the slots.
    scheduler_.CancelAllInFlight();
  }
}

void BotLoop::NoteEvent(BotState* state, const pb::Event& event) {
  NoteText(state, DescribeEvent(event));
}

// static
void BotLoop::ResetLooking(BotState* state) {
  state->pending_look.reset();
  state->glancing.clear();
  state->was_in_combat = false;
  state->last_bag_free_slots = 1;
  state->last_quest_state.reset();
}

void BotLoop::NoteText(BotState* state, std::string text) {
  if (text.empty()) {
    return;
  }
  if (!state->recent_events.empty() && state->recent_events.back() == text) {
    return;
  }
  state->recent_events.push_back(std::move(text));
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
  // Only actual whisperers enter the bounded whisperer list; Say/General/LFG
  // and invite senders must not evict whisperers or let bots whisper General
  // strangers. Current party members are added separately in StartBotCall().
  if (!event.chat || event.chat->channel != pb::WHISPER ||
      event.chat->from.empty()) {
    return;
  }
  std::string from = event.chat->from;
  std::erase_if(state->whisperers,
                [&](const BotState::Whisperer& w) { return w.name == from; });
  state->whisperers.push_front({std::move(from), now});
  if (state->whisperers.size() > kMaxWhisperers) {
    state->whisperers.pop_back();
  }
}

void BotLoop::Requeue(BotState* state,
                      std::vector<std::string> reasons,
                      std::vector<std::string> events,
                      std::string outcome) {
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
  if (state->last_outcome.empty() && !outcome.empty()) {
    state->last_outcome = std::move(outcome);
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

  const bool look_woke = OnLookSnapshot(bot, &state, received_at);

  // Pre-pull standing-order compilation on instance entry (A4): when a bot
  // enters an instance with boss notes, install cached compiled orders at once
  // or wake urgently out-of-combat to compile them before the pull.
  const bool in_instance = state.latest_snapshot->base.where.in_instance;
  const std::string cur_map = state.latest_snapshot->base.where.map;
  const bool left_or_changed_instance =
      state.was_in_instance && (!in_instance || state.last_map != cur_map);
  if (left_or_changed_instance && !state.orders.orders().empty()) {
    if (state.orders.SetOrders({}, received_at)) {
      SendStopMoving(bot);
    }
    Log(base::StrCat(
        {bot, ": cleared standing orders after leaving ", state.last_map}));
  }
  const bool entered_instance =
      in_instance && (!state.was_in_instance || state.last_map != cur_map);
  state.was_in_instance = in_instance;
  state.last_map = cur_map;
  bool pre_pull_woke = false;
  if (entered_instance) {
    if (!config_.bots_dir.empty()) {
      state.notes =
          ReadFile(base::StrCat({config_.bots_dir, "/", bot, "/notes.md"}))
              .value_or("");
    }
    const std::string selected_notes =
        SelectRelevantNotes(state.notes, *state.latest_snapshot);
    if (!selected_notes.empty()) {
      const CompiledOrdersKey key{bot, cur_map, selected_notes};
      if (auto cache_it = compiled_orders_cache_.find(key);
          cache_it != compiled_orders_cache_.end() &&
          !cache_it->second.empty()) {
        state.orders.SetOrders(cache_it->second, received_at);
        Log(base::StrCat({bot, ": pre-pull orders cache hit for ", cur_map,
                          " (", std::to_string(cache_it->second.size()),
                          " orders)"}));
      } else if (!state.latest_snapshot->base.self.in_combat) {
        state.has_ever_woken = true;
        WakeBot(bot, Urgency::kUrgent,
                base::StrCat({"entered instance ", cur_map,
                              ": compile standing orders from boss notes"}));
        pre_pull_woke = true;
      }
    }
  }

  if (!state.has_ever_woken) {
    state.has_ever_woken = true;
    WakeBot(bot, trig ? trig->urgency : Urgency::kNormal,
            trig ? trig->reason : "first look");
    return;
  }
  if (pre_pull_woke) {
    return;
  }
  const uint32_t prev_order_cmd = state.orders.pending_command_id();
  const OrderEvalResult ord =
      state.orders.OnSnapshot(*state.latest_snapshot, received_at);
  if (prev_order_cmd != 0 &&
      state.latest_snapshot->done_act_command_id == prev_order_cmd) {
    state.order_wakes = 0;
    state.order_quiet_since = base::TimeTicks();
  }
  if (!ord.covered && ord.wake_reason.empty() && !ord.act) {
    if (state.order_wakes > 0) {
      if (state.order_quiet_since.is_null()) {
        state.order_quiet_since = received_at;
      } else if (received_at - state.order_quiet_since >= base::Seconds(10)) {
        state.order_wakes = 0;
        state.order_quiet_since = base::TimeTicks();
      }
    }
  } else {
    state.order_quiet_since = base::TimeTicks();
  }
  DispatchOrderEval(bot, &state, ord, received_at);
  if (!ord.wake_reason.empty()) {
    return;
  }
  const bool trig_covered =
      trig && ord.covered && ord.order_index >= 0 &&
      static_cast<size_t>(ord.order_index) < state.orders.orders().size() &&
      OrderCoversWake(
          state.orders.orders()[static_cast<size_t>(ord.order_index)],
          trig->kinds, trig->attackers);
  if (trig && !trig_covered) {
    state.runner.Stop();
    state.look_deadline = base::TimeTicks();
    WakeBot(bot, trig->urgency, trig->reason);
    return;
  }
  if (look_woke || state.call_in_flight || state.has_queued_wake ||
      ord.covered) {
    return;
  }
  const RunnerAction action = state.runner.OnSnapshot(
      *state.latest_snapshot, state.latest_screen, received_at);
  if (action.step_done) {
    state.step_refusals = 0;
    state.last_outcome.clear();
  }
  DispatchRunnerAction(bot, &state, action);
  if (!state.has_queued_wake && !state.call_in_flight &&
      !state.look_deadline.is_null() && received_at >= state.look_deadline) {
    state.look_deadline = base::TimeTicks();
    state.runner.Stop();
    const bool backoff_wake = !state.pending_reasons.empty();
    WakeBot(bot, Urgency::kNormal, backoff_wake ? "" : "look timer",
            /*refusal=*/backoff_wake);
  }
}

void SetEarlierDeadline(base::TimeTicks* deadline, base::TimeTicks candidate) {
  if (deadline->is_null() || candidate < *deadline) {
    *deadline = candidate;
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
    if (state.orders.active() &&
        imm.event.error->command_id == state.orders.pending_command_id()) {
      OrderEvalResult failed = state.orders.OnStepFailed(imm.event.error->text);
      state.order_quiet_since = base::TimeTicks();
      // An "act cancelled by new choice" event is our own plan's choice
      // replacing an old movement act (whether it arrives mid-call or after
      // the reply): never report it as a standing-order failure or stop the
      // new plan.
      if (imm.event.error->text == "act cancelled by new choice") {
        return;
      }
      if (state.call_in_flight) {
        if (!failed.wake_reason.empty()) {
          if (std::find(state.pending_reasons.begin(),
                        state.pending_reasons.end(),
                        failed.wake_reason) == state.pending_reasons.end()) {
            state.pending_reasons.push_back(std::move(failed.wake_reason));
          }
          state.order_failure_during_call = true;
        }
        return;
      }
      state.runner.Stop();
      if (!failed.wake_reason.empty()) {
        WakeForOrder(bot, &state, failed.wake_reason, received_at,
                     /*is_failure=*/true);
      }
      return;
    }
    auto chat_it = std::find(state.recent_chat_command_ids.begin(),
                             state.recent_chat_command_ids.end(),
                             imm.event.error->command_id);
    if (chat_it != state.recent_chat_command_ids.end()) {
      state.recent_chat_command_ids.erase(chat_it);
      // Chat error with real command_id: wake the brain so it sees the refusal
      // without stopping the active runner step or freezing step_refusals.
      if (const std::optional<WakeTrigger> trig =
              EvaluateEventTrigger(imm.event, state.triggers.watches())) {
        if (state.latest_snapshot) {
          WakeBot(bot, trig->urgency, trig->reason, /*refusal=*/false);
        } else if (std::find(state.pending_reasons.begin(),
                             state.pending_reasons.end(),
                             trig->reason) == state.pending_reasons.end()) {
          state.pending_reasons.push_back(trig->reason);
        }
      }
      return;
    } else if (!state.runner.active() ||
               imm.event.error->command_id != state.runner.last_command_id()) {
      return;
    } else {
      state.runner.Stop();
      if (!imm.event.error->text.empty()) {
        state.last_outcome = imm.event.error->text;
      }
      if (state.call_in_flight) {
        state.early_step_failed = true;
      }
      const base::TimeDelta backoff = StepRefusalBackoff(++state.step_refusals);
      if (backoff.is_positive()) {
        if (const std::optional<WakeTrigger> trig =
                EvaluateEventTrigger(imm.event, state.triggers.watches());
            trig && std::find(state.pending_reasons.begin(),
                              state.pending_reasons.end(),
                              trig->reason) == state.pending_reasons.end()) {
          state.pending_reasons.push_back(trig->reason);
        }
        SetEarlierDeadline(&state.look_deadline, received_at + backoff);
        Log(base::StringPrintf(
            "harness: %s step refused %d times in a row; "
            "next look in %.0f s",
            bot.c_str(), state.step_refusals, backoff.InSecondsF()));
        return;
      }
    }
  }
  const bool refusal = imm.event.error && imm.event.error->command_id != 0;
  const std::optional<WakeTrigger> trig =
      EvaluateEventTrigger(imm.event, state.triggers.watches());
  if (!trig) {
    return;
  }
  state.runner.Stop();
  state.look_deadline = base::TimeTicks();
  if (state.latest_snapshot) {
    WakeBot(bot, trig->urgency, trig->reason, refusal);
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
      state.orders.Clear();
      state.latest_snapshot.reset();
      state.look_deadline = base::TimeTicks();
      state.has_ever_woken = false;
      state.was_in_instance = false;
      state.last_map.clear();
      state.last_selected_notes.clear();
      state.recent_chat_command_ids.clear();
      state.has_queued_wake = false;
      state.call_in_flight = false;
      state.early_steps_dispatched = false;
      state.early_step_failed = false;
      state.step_refusals = 0;
      state.last_outcome.clear();
      state.order_wakes = 0;
      state.pending_reasons.clear();
      state.recent_events.clear();
      ResetLooking(&state);
      ++state.call_epoch;
      continue;
    }
    if (!state.latest_snapshot || state.call_in_flight ||
        state.has_queued_wake) {
      continue;
    }
    if (const OrderEvalResult timeout = state.orders.CheckTimeout(now);
        !timeout.wake_reason.empty()) {
      DispatchOrderEval(bot, &state, timeout, now);
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
      WakeBot(bot, Urgency::kNormal, backoff_wake ? "" : "look timer",
              /*refusal=*/backoff_wake);
    }
  }
}

void BotLoop::WakeBot(const std::string& bot,
                      Urgency urgency,
                      const std::string& reason,
                      bool refusal) {
  BotState& state = bots_[bot];
  if (!refusal) {
    state.step_refusals = 0;
    state.order_wakes = 0;
  }
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
  if (!config_.bots_dir.empty()) {
    state.notes =
        ReadFile(base::StrCat({config_.bots_dir, "/", bot, "/notes.md"}))
            .value_or("");
  }

  state.has_queued_wake = false;
  state.call_in_flight = true;
  state.early_steps_dispatched = false;
  state.early_step_failed = false;
  state.order_failure_during_call = false;
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
  // Party members may also be whispered via `tell` without crowding out recent
  // human whisperers.
  if (state.latest_snapshot->ext.party) {
    for (const pb::Unit& m : state.latest_snapshot->ext.party->members) {
      if (!m.name.empty() && m.name != bot &&
          std::find(input.whisperers.begin(), input.whisperers.end(), m.name) ==
              input.whisperers.end()) {
        input.whisperers.push_back(m.name);
      }
    }
  }
  // Ages on the snapshots' clock (seen_at is a snapshot's received time).
  input.remembered = RememberedPanels(state, state.latest_received_at,
                                      config_.remember_panels_for);
  // Consume `last_outcome` once delivered so it never goes stale across
  // unrelated future wakes (requeued on cancel/error via `asked_outcome`).
  std::string asked_outcome = std::move(state.last_outcome);
  state.last_outcome.clear();
  input.last_outcome = asked_outcome;
  input.notes = SelectRelevantNotes(state.notes, *state.latest_snapshot);
  input.knowledge =
      knowledge_.SelectRelevant(*state.latest_snapshot, !input.notes.empty());
  state.last_selected_notes = input.notes;
  for (const Order& o : state.orders.orders()) {
    input.active_orders.push_back(FormatOrder(o));
  }
  input.order_names = CollectOrderNames(*state.latest_snapshot, input.notes);
  std::vector<std::string> order_names = input.order_names;
  input.slot = slot;
  input.temperature = config_.temperature;

  const uint32_t ask_seq = state.latest_snapshot->seq;
  state.ask_watch_inputs = TriggerEvaluator::InputsOf(*state.latest_snapshot);
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
      [self, bot, epoch, ask_seq, ask_screen,
       order_names = std::move(order_names), wake_reason,
       asked_reasons = std::move(asked_reasons),
       asked_events = std::move(asked_events),
       asked_outcome = std::move(asked_outcome),
       request_copy = std::move(request_copy), snap_mono_us, age_at_ask_ms,
       holder = std::move(first_line_age_holder),
       done = std::move(done)](LlmResult result) mutable {
        if (!self) {
          std::move(done).Run();
          return;
        }
        self->OnReply(bot, epoch, ask_seq, std::move(ask_screen),
                      std::move(order_names), std::move(wake_reason),
                      std::move(asked_reasons), std::move(asked_events),
                      std::move(asked_outcome), std::move(request_copy),
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
                      std::vector<std::string> order_names,
                      std::string wake_reason,
                      std::vector<std::string> asked_reasons,
                      std::vector<std::string> asked_events,
                      std::string asked_outcome,
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
      Requeue(&it->second, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_outcome));
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
    plan = ParsePlan(
        StripTruncatedLastLine(result.reply->text, result.reply->finish_reason),
        static_cast<int>(ask_screen.options.size()), order_names);
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
      Requeue(&state, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_outcome));
    } else if (!plan) {
      Log(base::StringPrintf("harness: %s invalid plan (%.0f ms)", bot.c_str(),
                             decision.result.round_trip_ms));
      state.look_deadline = now + base::Seconds(10);
      Requeue(&state, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_outcome));
    } else {
      if (!plan->goal.empty()) {
        state.current_goal = plan->goal;
      }
      const std::string formatted = FormatPlan(*plan);
      const std::string do_line = formatted.substr(0, formatted.find('\n'));
      Log(base::StringPrintf(
          "harness: %s woke (%s) -> %s (do %.0f ms, total %.0f ms, look %d s, "
          "prompt %d tok)",
          bot.c_str(), decision.reason.c_str(), do_line.c_str(),
          decision.result.first_line_ms, decision.result.round_trip_ms,
          plan->look_s, decision.result.reply->prompt_tokens));
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
                : CollectExpectedNames(plan->steps, plan->expect, ask_screen),
            &state.ask_watch_inputs);
        if (plan->clear_orders && plan->orders.empty()) {
          if (state.orders.SetOrders({}, now)) {
            SendStopMoving(bot);
          }
        } else if (!plan->orders.empty()) {
          if (state.orders.SetOrders(plan->orders, now)) {
            SendStopMoving(bot);
          }
        }
        if (!plan->orders.empty() &&
            state.latest_snapshot->base.where.in_instance &&
            !state.latest_snapshot->base.self.in_combat &&
            !state.last_selected_notes.empty()) {
          const std::string& map_name = state.latest_snapshot->base.where.map;
          const CompiledOrdersKey key{bot, map_name, state.last_selected_notes};
          if (compiled_orders_cache_.try_emplace(key, plan->orders).second) {
            Log(base::StrCat({bot, ": cached ",
                              std::to_string(plan->orders.size()),
                              " pre-pull standing orders for ", map_name}));
          }
        }
        if (!state.early_step_failed) {
          state.look_deadline = now + base::Seconds(plan->look_s);
        }
        if (!plan->say.empty()) {
          pb::ChatChannel ch = pb::SAY;
          if (state.latest_snapshot->ext.party &&
              !state.latest_snapshot->ext.party->members.empty()) {
            for (const std::string& r : asked_reasons) {
              if (r.find("party from ") != std::string::npos) {
                ch = pb::PARTY;
                break;
              }
              if (r.find("raid from ") != std::string::npos) {
                ch = pb::RAID;
                break;
              }
            }
          }
          SendSay(bot, ch, "", plan->say);
        }
        if (!plan->tell.empty()) {
          SendSay(bot, pb::WHISPER, plan->tell_to, plan->tell);
        }
        for (const std::string& name : plan->close) {
          SendPanel(bot, PanelFromName(name), /*open=*/false);
        }
        // A look at what the brain already had on its screen shows nothing
        // new: no wake for it (it woke the bot on every snapshot: review of
        // f40b0b6, finding 5).
        BotState::PendingLook look;
        for (const std::string& name : plan->open) {
          const pb::Panel panel = PanelFromName(name);
          SendPanel(bot, panel, /*open=*/true);
          if (std::find(ask_screen.open_panels.begin(),
                        ask_screen.open_panels.end(),
                        panel) == ask_screen.open_panels.end()) {
            look.panels.push_back(panel);
          }
          // The brain keeps it open.
          std::erase_if(state.glancing, [&](const BotState::Glance& g) {
            return g.panel == panel;
          });
        }
        if (!plan->hover.empty()) {
          for (const Hoverable& h : ask_screen.hoverables) {
            if (h.name == plan->hover) {
              SendHover(bot, h.ref);
              if (std::find(ask_screen.shown_tooltips.begin(),
                            ask_screen.shown_tooltips.end(),
                            h.name) == ask_screen.shown_tooltips.end()) {
                look.hover = h.name;
              }
              break;
            }
          }
        }
        if (!look.panels.empty() || !look.hover.empty()) {
          look.until = now + config_.look_wait;
          state.pending_look = std::move(look);
        }
        if (!state.early_steps_dispatched) {
          const RunnerAction action =
              state.runner.Start(plan->steps, ask_seq, ask_screen, now);
          DispatchRunnerAction(bot, &state, action);
        }
        if (state.order_failure_during_call) {
          state.order_failure_during_call = false;
          WakeForOrder(bot, &state, "", now, /*is_failure=*/true);
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

bool BotLoop::OnLookSnapshot(const std::string& bot,
                             BotState* state,
                             base::TimeTicks received_at) {
  const pb::Snapshot& snap = *state->latest_snapshot;
  const pb::Ui& ui = snap.base.ui;
  const auto is_open = [&ui](pb::Panel panel) {
    return std::find(ui.open_panels.begin(), ui.open_panels.end(), panel) !=
           ui.open_panels.end();
  };
  for (const pb::Panel panel : ui.open_panels) {
    if (std::vector<std::string> lines = PanelLines(snap, panel);
        !lines.empty()) {
      state->seen_panels[panel] = {std::move(lines), received_at};
    }
  }

  // Habits: no brain call. They act on panels a habit opened only, never on
  // one the brain opened, and each act is a line in the next prompt, so the
  // brain knows what its body did (review of f40b0b6, finding 8).
  const BotLoopConfig::Habits& habits = config_.habits;
  const bool in_combat = snap.base.self.in_combat;
  const auto asked = [&](pb::Panel panel) {
    return state->pending_look &&
           std::find(state->pending_look->panels.begin(),
                     state->pending_look->panels.end(),
                     panel) != state->pending_look->panels.end();
  };
  if (habits.close_panels_in_combat && in_combat && !state->was_in_combat) {
    for (const BotState::Glance& g : state->glancing) {
      if (is_open(g.panel)) {
        SendPanel(bot, g.panel, /*open=*/false);
        NoteText(state, base::StrCat({"you closed your ", PanelName(g.panel),
                                      " (combat started)"}));
      }
    }
    state->glancing.clear();
  }
  state->was_in_combat = in_combat;
  std::erase_if(state->glancing, [&](const BotState::Glance& g) {
    if (asked(g.panel)) {
      return true;  // The brain asked for it meanwhile: it stays open.
    }
    if (!is_open(g.panel)) {
      return received_at >= g.until;  // Never shown: given up.
    }
    SendPanel(bot, g.panel, /*open=*/false);  // Seen (and remembered).
    return true;
  });
  const auto glance = [&](pb::Panel panel, std::string_view why) {
    if (in_combat || is_open(panel) || asked(panel) ||
        std::any_of(
            state->glancing.begin(), state->glancing.end(),
            [&](const BotState::Glance& g) { return g.panel == panel; })) {
      return;
    }
    SendPanel(bot, panel, /*open=*/true);
    state->glancing.push_back({panel, received_at + config_.look_wait});
    NoteText(state, base::StrCat({"you glanced at your ", PanelName(panel),
                                  " (", why, ")"}));
  };
  if (const std::optional<uint32_t> bag_free =
          snap.base.self.indicators.bag_free_slots) {
    const uint32_t free = *bag_free;
    if (habits.open_bags_when_full && free == 0 &&
        state->last_bag_free_slots != 0) {
      glance(pb::BAGS, "they were full");
    }
    state->last_bag_free_slots = free;
  }
  std::string quest_state;
  for (const pb::Objective& o : snap.ext.quest_tracker) {
    if (o.quest) {
      base::StrAppend(&quest_state, {base::NumberToString(o.quest->quest_id),
                                     o.quest->complete ? "c" : "", ";"});
    }
  }
  if (habits.glance_at_quest_log && state->last_quest_state &&
      quest_state != *state->last_quest_state) {
    glance(pb::QUEST_LOG, "your quests changed");
  }
  state->last_quest_state = std::move(quest_state);

  // The look the brain asked for.
  if (!state->pending_look) {
    return false;
  }
  const BotState::PendingLook& look = *state->pending_look;
  bool shown = std::all_of(look.panels.begin(), look.panels.end(), is_open);
  if (!look.hover.empty()) {
    shown = shown &&
            std::any_of(snap.base.tooltips.begin(), snap.base.tooltips.end(),
                        [&](const pb::Tooltipped& t) {
                          return t.ref && t.ref->name == look.hover;
                        });
  }
  if (!shown) {
    if (received_at >= look.until) {
      state->pending_look.reset();
    }
    return false;
  }
  std::vector<std::string> what;
  for (const pb::Panel panel : look.panels) {
    what.emplace_back(PanelName(panel));
  }
  if (!look.hover.empty()) {
    what.push_back(look.hover);
  }
  state->pending_look.reset();
  if (state->call_in_flight || state->has_queued_wake) {
    return false;  // A newer call sees it anyway.
  }
  state->look_deadline = base::TimeTicks();
  WakeBot(bot, Urgency::kNormal,
          base::StrCat({"looked at ", base::JoinString(what, ", ")}));
  return true;
}

// static
std::vector<std::string> BotLoop::RememberedPanels(
    const BotState& state,
    base::TimeTicks now,
    base::TimeDelta remember_for) {
  std::vector<std::string> out;
  const pb::Ui& ui = state.latest_snapshot->base.ui;
  for (const auto& [panel, seen] : state.seen_panels) {
    if (std::find(ui.open_panels.begin(), ui.open_panels.end(), panel) !=
            ui.open_panels.end() ||
        seen.lines.empty() || now - seen.seen_at > remember_for) {
      continue;  // On screen now, or forgotten.
    }
    std::vector<std::string> lines = seen.lines;
    lines.front() = base::StringPrintf(
        "%s, seen %.0f s ago: %s", std::string(PanelName(panel)).c_str(),
        (now - seen.seen_at).InSecondsF(),
        std::string(base::TrimWhitespaceASCII(lines.front())).c_str());
    out.insert(out.end(), lines.begin(), lines.end());
  }
  return out;
}

void BotLoop::SendPanel(const std::string& bot, pb::Panel panel, bool open) {
  if (panel == pb::PANEL_UNSPECIFIED) {
    return;
  }
  pb::Command cmd;
  cmd.bot = bot;
  cmd.command_id = ++next_command_id_;
  if (open) {
    cmd.open.emplace().panel = panel;
  } else {
    cmd.close.emplace().panel = panel;
  }
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
}

void BotLoop::SendHover(const std::string& bot, const pb::HoverRef& ref) {
  pb::Command cmd;
  cmd.bot = bot;
  cmd.command_id = ++next_command_id_;
  cmd.hover.emplace().ref = ref;
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
}

void BotLoop::SendStopMoving(const std::string& bot) {
  pb::Command::Act stop_moving;
  stop_moving.verb = pb::Command::Act::STOP;
  stop_moving.stop = pb::Command::Act::MOVING;
  SendAct(bot, std::move(stop_moving));
}

void BotLoop::WakeForOrder(const std::string& bot,
                           BotState* state,
                           std::string_view reason,
                           base::TimeTicks now,
                           bool is_failure) {
  if (!is_failure) {
    state->order_wakes = 0;
    state->look_deadline = base::TimeTicks();
    WakeBot(bot, Urgency::kUrgent, std::string(reason), /*refusal=*/false);
    return;
  }
  const base::TimeDelta backoff = StepRefusalBackoff(++state->order_wakes);
  if (backoff.is_positive()) {
    if (!reason.empty()) {
      const std::string r(reason);
      if (std::find(state->pending_reasons.begin(),
                    state->pending_reasons.end(),
                    r) == state->pending_reasons.end()) {
        state->pending_reasons.push_back(r);
      }
    }
    // Never push an earlier plan/backoff look deadline further out.
    SetEarlierDeadline(&state->look_deadline, now + backoff);
    Log(base::StringPrintf(
        "harness: %s order failure %d in a row; next look in %.0f s",
        bot.c_str(), state->order_wakes, backoff.InSecondsF()));
    return;
  }
  state->look_deadline = base::TimeTicks();
  WakeBot(bot, Urgency::kUrgent, std::string(reason), /*refusal=*/true);
}

void BotLoop::DispatchOrderEval(const std::string& bot,
                                BotState* state,
                                const OrderEvalResult& eval,
                                base::TimeTicks now) {
  if (eval.kite_ended) {
    SendStopMoving(bot);
    return;
  }
  if (!eval.wake_reason.empty()) {
    if (eval.timed_out || eval.preempted) {
      // Stop any in-progress movement (e.g. a timed-out act or preempted kite)
      // and release `MOVEMENT_FORCED` on the module before waking the brain.
      SendStopMoving(bot);
    }
    state->runner.Stop();
    WakeForOrder(bot, state, eval.wake_reason, now,
                 /*is_failure=*/eval.timed_out);
    return;
  }
  if (!eval.act) {
    return;
  }
  if (eval.preempted) {
    SendStopMoving(bot);
  }
  std::optional<pb::Command::Act> wire = OrderActToCommand(
      *eval.act, eval.it_guid, eval.it_name, eval.target_guid);
  if (!wire) {
    OrderEvalResult failed = state->orders.OnStepFailed("unsupported act verb");
    state->runner.Stop();
    state->look_deadline = base::TimeTicks();
    if (!failed.wake_reason.empty()) {
      WakeForOrder(bot, state, failed.wake_reason, now, /*is_failure=*/true);
    }
    return;
  }
  const uint32_t id = SendAct(bot, std::move(*wire));
  state->orders.set_pending_command_id(id);
}

uint32_t BotLoop::SendAct(const std::string& bot, pb::Command::Act act) {
  pb::Command cmd;
  cmd.bot = bot;
  cmd.command_id = ++next_command_id_;
  cmd.act = std::move(act);
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
  return cmd.command_id;
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
  auto& ids = bots_[bot].recent_chat_command_ids;
  ids.push_back(cmd.command_id);
  while (ids.size() > 16) {
    ids.pop_front();
  }
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
