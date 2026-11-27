// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Decides which woken bot's brain is asked next. Many bots can be woken at
// once and the model can only work on so many questions at a time (the slots
// of llama-server), so the rest wait. Urgent wake-ups (a popup with a timer,
// a fight) go first; within the same urgency, the bot that has waited longest.
//
// A bot has at most one question in progress. If it is woken again while one
// is waiting for its turn, the newer wake-up replaces the old (the old screen
// is stale); if one is in progress, the newer waits behind it.
//
// A human playing with bots shouldn't wait behind the rest of the world: while
// any bot shares a party with an online human (`SetHumanParty`) and there are
// at least 2 permits, one permit is held for those bots; others run on the
// rest. Slots are not reserved, only the permit: a human-party bot still
// takes any free slot (preferring its own, for the KV cache).
//
// Not thread-safe: use on one sequence. Work reports back by calling `done`
// on that sequence.

#ifndef HARNESS_LLM_SCHEDULER_H_
#define HARNESS_LLM_SCHEDULER_H_

#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <flat_set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"

namespace harness {

// Priority classes from docs/design.md §6 (lower integer = higher priority).
enum class Urgency {
  kP0Combat = 0,   // Combat / raid calls / death / loss of control (~1 s).
  kP1Danger = 1,   // Group / danger / timed prompts / enemy player watches.
  kP2Routine = 2,  // Questing / navigation / stuck / plan done / look timer.
  kP3Chat = 3,     // Chat and whisper replies.
  kP4Reflect = 4,  // Reflection / background planning when GPU is idle.

  // Shorthand aliases.
  kUrgent = kP0Combat,
  kNormal = kP2Routine,
};

// Declared group role for scheduling priority within the same urgency tier
// (lower integer = higher priority). Tanks and healers go ahead of DPS when
// many raid bots wake at once.
enum class BotRole {
  kTank = 0,
  kHealer = 1,
  kDefault = 2,
};

class Scheduler {
 public:
  // Starts the work for one bot (asks the model). `SlotWork` also receives the
  // assigned llama-server slot `0 .. max_in_flight - 1` (reusing the bot's
  // previous slot when idle so its prompt prefix stays cached, else any free
  // slot without waiting). Work must call `done` once on the scheduler's
  // sequence when finished. `CancelableSlotWork` returns an optional closure
  // that aborts the in-flight call early if a `kP0Combat` wake preempts a
  // lower-priority call for the same bot.
  using Work = base::OnceCallback<void(base::OnceClosure done)>;
  using SlotWork = base::OnceCallback<void(int slot, base::OnceClosure done)>;
  using CancelableSlotWork =
      base::OnceCallback<base::OnceClosure(int slot, base::OnceClosure done)>;

  explicit Scheduler(size_t max_in_flight,
                     base::TimeDelta aging_step = base::Seconds(2));
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  ~Scheduler();

  // Optional test clock for deterministic aging tests.
  void SetNowForTesting(base::RepeatingCallback<base::TimeTicks()> now_fn);

  void SubmitCancelable(const std::string& bot,
                        Urgency urgency,
                        CancelableSlotWork work);
  void Submit(const std::string& bot, Urgency urgency, SlotWork work);
  void Submit(const std::string& bot, Urgency urgency, Work work);

  // Whether `bot` plays in a party with an online human (from its latest
  // snapshot). Turning it off frees the held permit as soon as the next call
  // finishes or is submitted (never runs work from inside this call).
  void SetHumanParty(const std::string& bot, bool in_human_party);

  // Declared group role of `bot` (from its latest snapshot) used as a
  // tie-breaker within the same effective urgency tier (tanks -> healers ->
  // DPS/other). Never starts work from inside this call.
  void SetBotRole(const std::string& bot, BotRole role);

  // Aborts all cancelable in-flight calls (e.g. on harness disconnect).
  void CancelAllInFlight();

  size_t in_flight() const { return in_flight_.size(); }
  size_t queued() const { return queue_.size(); }

 private:
  struct Waiting {
    std::string bot;
    Urgency urgency = Urgency::kP2Routine;
    uint64_t order = 0;  // Lower waited longer.
    base::TimeTicks enqueued_at;
    CancelableSlotWork work;
  };

  struct InFlight {
    uint64_t run_id = 0;
    int slot = 0;
    Urgency urgency = Urgency::kP2Routine;
    base::OnceClosure cancel;
  };

  base::TimeTicks Now() const;
  int EffectivePriority(const Waiting& w, base::TimeTicks now) const;
  BotRole RoleOf(const std::string& bot) const;
  int PickSlot(const std::string& bot);
  // One permit is held for human-party bots and `bot` isn't one of them.
  bool HeldBackForHumanParty(const std::string& bot) const;
  void StartWaiting();
  void OnDone(const std::string& bot, uint64_t run_id);

  const size_t max_in_flight_;
  const base::TimeDelta aging_step_;
  base::RepeatingCallback<base::TimeTicks()> now_fn_;
  uint64_t next_order_ = 0;
  uint64_t next_run_id_ = 0;
  size_t next_slot_ = 0;
  std::vector<Waiting> queue_;
  std::vector<bool> slot_busy_;
  std::flat_map<std::string, InFlight> in_flight_;
  std::flat_map<std::string, int> last_slot_;
  std::flat_set<std::string> human_party_;
  std::flat_map<std::string, BotRole> bot_roles_;
  bool starting_ = false;  // StartWaiting is running (work may finish inside).
  base::WeakPtrFactory<Scheduler> weak_factory_{this};
};

}  // namespace harness

#endif  // HARNESS_LLM_SCHEDULER_H_
