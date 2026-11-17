// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Evaluates when a bot's brain should be woken (docs/specs/triggers.md).
// C++ makes no game decisions here: it checks the client's own attention alerts
// (prompts, whispers, death, loss of control, unexpected combat or attackers)
// and the watches (`hp<N`, `mana<N`, `enemy_player<YARDS`, `attacked`,
// `whisper`) and expectations (`expect`) the brain itself set in its last plan.

#ifndef HARNESS_TRIGGERS_H_
#define HARNESS_TRIGGERS_H_

#include <flat_set>
#include <optional>
#include <string>
#include <vector>

#include "plan.h"
#include "proto/snapshot.h"
#include "scheduler.h"

namespace harness {

struct WakeTrigger {
  Urgency urgency = Urgency::kNormal;
  std::string reason;

  bool operator==(const WakeTrigger&) const = default;
};

// One-line plain-English description of an event for `Since your last look:`
// and wake reasons; empty when the event has nothing worth noting.
std::string DescribeEvent(const wlb::snapshot::Event& event);

// Checks whether an immediate (or snapshot-embedded) event wakes the brain by
// default or via `watches`. Events that return nullopt (party chat, XP, loot
// lines) are noted for the next look without waking the brain.
std::optional<WakeTrigger> EvaluateEventTrigger(
    const wlb::snapshot::Event& event,
    const std::vector<Watch>& watches = {});

// Stateful per-bot evaluator across consecutive snapshots: fires each crossing
// or new attacker once rather than on every tick while the condition holds.
class TriggerEvaluator {
 public:
  TriggerEvaluator() = default;

  // Installs the active plan's watches and expected mob names, resetting
  // watch crossing latches.
  void SetPlanContext(std::vector<Watch> watches,
                      std::flat_set<std::string> expected_names);

  const std::vector<Watch>& watches() const { return watches_; }
  const std::flat_set<std::string>& expected_names() const {
    return expected_names_;
  }

  // Compares `snapshot` against the previous snapshot state, updates stored
  // state, and returns the highest-urgency trigger that fired (if any).
  std::optional<WakeTrigger> EvaluateSnapshot(
      const wlb::snapshot::Snapshot& snapshot);

 private:
  struct ControlFlags {
    bool stunned = false;
    bool feared = false;
    bool charmed = false;
    bool silenced = false;
    bool rooted = false;
    bool pacified = false;
    bool disarmed = false;
  };

  bool seen_snapshot_ = false;
  bool was_alive_ = true;
  bool was_in_combat_ = false;
  ControlFlags prev_control_;
  std::flat_set<std::string> prev_attackers_;

  std::vector<Watch> watches_;
  std::vector<bool> watch_fired_;
  std::vector<std::flat_set<std::string>> watch_seen_players_;
  std::flat_set<std::string> expected_names_;
};

}  // namespace harness

#endif  // HARNESS_TRIGGERS_H_
