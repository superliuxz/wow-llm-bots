// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Evaluates a plan's standing orders (`docs/specs/triggers.md`) against
// incoming snapshots:
// - Checks orders in priority order (first match wins); a higher-priority
//   order interrupts a running lower-priority one.
// - Binds `it` to what the first non-negated condition matched on.
// - Tracks cross-snapshot history for time windows (`cast in last Ns`,
//   `appears in last Ns`), `after Ns`, and `every Ns`.
// - Runs `then` chains (1..3 steps) one step at a time, advancing on
//   `done_command_id` (or `OnStepDone`), with a per-step safety timeout
//   (default 5 s) that wakes the brain on timeout or failure.
// - Reports `covered` when a non-`wake` order is handling the current state so
//   default wakes are suppressed.

#ifndef HARNESS_PLAN_ORDER_EVALUATOR_H_
#define HARNESS_PLAN_ORDER_EVALUATOR_H_

#include <cstdint>
#include <flat_map>
#include <flat_set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "plan/orders.h"
#include "snapshot/snapshot.h"

namespace harness {

inline constexpr base::TimeDelta kDefaultOrderStepTimeout = base::Seconds(5);
inline constexpr base::TimeDelta kLongOrderMoveTimeout = base::Seconds(15);
inline constexpr base::TimeDelta kMaxOrderHistoryAge = base::Seconds(120);

struct OrderEvalResult {
  // Next act to send to the module (nullopt if no new command this tick).
  std::optional<Act> act;
  // Resolved `it` binding for the active/fired order (empty if unbound).
  std::string it_guid;
  std::string it_name;
  // Index of the order and step that fired or is currently running (-1 if
  // idle).
  int order_index = -1;
  int step_index = -1;
  // True when a non-`wake` conditional order fired or is actively running.
  // Use `OrderCoversWakeReason` before suppressing a specific WakeTrigger.
  bool covered = false;
  // True when a higher-priority order preempted a running lower-priority one.
  bool preempted = false;
  // True when the active step timed out (caller should send `stop moving` to
  // release `MOVEMENT_FORCED` on the module).
  bool timed_out = false;
  // True when a running `kite` ended because its order's `when` went false:
  // the caller sends `stop moving` (no wake; the order did its job).
  bool kite_ended = false;
  // Non-empty when the brain should wake: a `wake` act ran, a step timed out,
  // or a step failed.
  std::string wake_reason;
};

// Whether `order`'s condition covers every kind of a snapshot wake (`kinds`,
// WakeTrigger::kinds): `me hp<N` covers my health watch, `mana<N` the mana
// one, `X targets me` being attacked by X (only X, not other attackers). Death,
// loss of control and the rest always wake; `always` orders never cover
// anything.
bool OrderCoversWake(const Order& order,
                     uint32_t kinds,
                     const std::vector<std::string>& attackers = {});

// Converts a grammar `Act` into a wire `Command::Act` for the verbs supported
// on the wire (`to`, `away`, `target`, `use`, `stop`, `wake`). Returns nullopt
// for verbs not yet implemented on the wire (docs/TODO.md #5d).
std::optional<wlb::snapshot::Command::Act> OrderActToCommand(
    const Act& act,
    std::string_view it_guid,
    std::string_view it_name);

class OrderEvaluator {
 public:
  explicit OrderEvaluator(
      base::TimeDelta step_timeout = kDefaultOrderStepTimeout);

  // Replaces all active orders (a new plan replaces the old plan's orders).
  // Preserves edge latches and any currently running step whose order text is
  // still present in the new plan. Returns true if an active movement step was
  // dropped (caller should send `stop moving`).
  bool SetOrders(std::vector<Order> orders, base::TimeTicks now);

  void Clear();

  const std::vector<Order>& orders() const { return orders_; }
  bool active() const { return active_order_ >= 0; }
  bool ActiveStepMoves() const;
  int active_order() const { return active_order_; }
  int active_step() const { return active_order_ >= 0 ? active_step_ : -1; }

  // Records the wire `command_id` assigned when dispatching the last returned
  // `act`, so `snapshot.done_command_id` (or `OnStepDone`) can advance the
  // `then` chain.
  uint32_t pending_command_id() const { return pending_command_id_; }
  void set_pending_command_id(uint32_t id) { pending_command_id_ = id; }

  // Evaluates standing orders against `snapshot` at `now`.
  OrderEvalResult OnSnapshot(const wlb::snapshot::Snapshot& snapshot,
                             base::TimeTicks now);

  // Advances an active `then` chain when `command_id` finishes outside of a
  // snapshot. Returns the next step's act if more steps remain.
  OrderEvalResult OnStepDone(uint32_t command_id, base::TimeTicks now);

  // Cancels any active order on step failure and wakes the brain.
  OrderEvalResult OnStepFailed(std::string_view error);

  // Checks the per-step safety timeout even if no snapshot arrived recently.
  OrderEvalResult CheckTimeout(base::TimeTicks now);

 private:
  struct SeenEvent {
    base::TimeTicks when;
    std::string guid;
    std::string name;
  };

  std::optional<base::TimeDelta> TimeoutForAct(const Act& act) const;
  void UpdateHistory(const wlb::snapshot::Snapshot& snapshot,
                     base::TimeTicks now);
  OrderEvalResult DispatchStep(int order_idx,
                               int step_idx,
                               std::string it_guid,
                               std::string it_name,
                               bool preempted,
                               base::TimeTicks now);
  void ResetActive();

  base::TimeDelta step_timeout_;
  std::vector<Order> orders_;
  base::TimeTicks orders_installed_at_;
  std::vector<std::optional<base::TimeTicks>> last_every_fired_;
  // Edge-trigger latch per order: true after firing until `when` goes false.
  std::vector<bool> order_latched_;

  int active_order_ = -1;
  int active_step_ = 0;
  std::string active_it_guid_;
  std::string active_it_name_;
  base::TimeTicks step_started_at_;
  uint32_t pending_command_id_ = 0;

  // Cross-snapshot history keyed by ASCII-lowercased spell/entity name.
  bool history_seeded_ = false;
  std::flat_map<std::string, SeenEvent> last_casts_;
  std::flat_map<std::string, SeenEvent> last_appears_;
  std::flat_set<std::string> prev_entity_keys_;
};

}  // namespace harness

#endif  // HARNESS_PLAN_ORDER_EVALUATOR_H_
