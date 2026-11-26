// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Evaluates when a bot's brain should be woken (docs/specs/triggers.md).
// C++ makes no game decisions here: it checks the client's own attention alerts
// (prompts, whispers, death, loss of control, unexpected combat or attackers)
// and the watches (`hp<N`, `mana<N`, `enemy_player<YARDS`, `attacked`,
// `whisper`) and expectations (`expect`) the brain itself set in its last plan.

#ifndef HARNESS_PLAN_TRIGGERS_H_
#define HARNESS_PLAN_TRIGGERS_H_

#include <cstdint>
#include <flat_set>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llm/scheduler.h"
#include "plan/plan.h"
#include "snapshot/snapshot.h"

namespace harness {

// What a snapshot wake is about, as bits (a merged trigger has several), so
// a standing order can cover it by its condition (OrderCoversWake), not by
// its wording.
enum WakeKind : uint32_t {
  kWakeOther = 1u << 0,
  kWakeDied = 1u << 1,
  kWakeAlive = 1u << 2,
  kWakeLostControl = 1u << 3,
  kWakeAttacked = 1u << 4,
  kWakeEnteredCombat = 1u << 5,
  kWakeHealth = 1u << 6,  // A `hp<N` watch: my own health.
  kWakeMana = 1u << 7,    // A `mana<N` watch.
  kWakeEnemyPlayer = 1u << 8,
  kWakeParty = 1u << 9,  // The party: fighting, a member died, healer oom.
};

struct WakeTrigger {
  Urgency urgency = Urgency::kNormal;
  std::string reason;
  uint32_t kinds = kWakeOther;
  // Names of new attackers that caused `kWakeAttacked` (empty for generic
  // combat entry). Used by `OrderCoversWake` so `X targets me` only hides
  // attacks by X.
  std::vector<std::string> attackers = {};

  WakeTrigger() = default;
  WakeTrigger(Urgency u,
              std::string r,
              uint32_t k = kWakeOther,
              std::vector<std::string> a = {})
      : urgency(u), reason(std::move(r)), kinds(k), attackers(std::move(a)) {}

  // What the brain is told; `kinds` / `attackers` are bookkeeping.
  bool operator==(const WakeTrigger& other) const {
    return urgency == other.urgency && reason == other.reason;
  }
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
  // What a snapshot says about the watches' conditions.
  struct WatchInputs {
    struct EnemyPlayer {
      std::string key;
      float dist_yd = 0;
    };
    bool alive = true;
    std::optional<double> hp_pct;
    std::optional<double> mana_pct;
    std::vector<EnemyPlayer> enemy_players;
  };
  static WatchInputs InputsOf(const wlb::snapshot::Snapshot& snapshot);

  TriggerEvaluator() = default;

  // Installs the active plan's watches and expected mob names. Each watch's
  // latch starts from `seen`, the snapshot the brain was asked about: a
  // condition already true there does not fire until it clears and crosses
  // again, but one that crossed while the call was in flight fires. Without
  // `seen`, the last snapshot evaluated.
  void SetPlanContext(std::vector<Watch> watches,
                      std::flat_set<std::string> expected_names,
                      const WatchInputs* seen = nullptr);

  // Replaces only the expected mob names, keeping the watches and latches
  // (the first streamed line of a plan, before its `expect:` arrives).
  void SetExpectedNames(std::flat_set<std::string> expected_names);

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
  // The party as last seen (T8): who was dead, whether anyone in range had
  // aggro, and which healers were under 30% mana.
  std::flat_set<std::string> prev_dead_members_;
  bool prev_party_aggro_ = false;
  std::flat_set<std::string> prev_low_mana_healers_;
  // Members under 35% health in a fight, latched until back over 50%.
  std::flat_set<std::string> prev_critical_members_;
  // Online members (T11): one who leaves or logs out wakes the bot.
  std::flat_set<std::string> prev_online_members_;
  std::flat_set<std::string> prev_attackers_;
  // For a bot whose group role is tank: living mobs (not crowd-controlled)
  // whose target is another, non-tank party member.
  std::flat_set<std::string> prev_loose_mobs_;
  // Last seen spell miss on `self`, so a miss wakes once while it remains on
  // screen, and a second miss (higher `seq`) wakes again.
  std::optional<wlb::snapshot::Self::SpellMiss> prev_spell_miss_;
  // Last snapshot's watch inputs, to seed latches in SetPlanContext().
  WatchInputs last_;

  std::vector<Watch> watches_;
  std::vector<bool> watch_fired_;
  std::vector<std::flat_set<std::string>> watch_seen_players_;
  std::flat_set<std::string> expected_names_;
};

}  // namespace harness

#endif  // HARNESS_PLAN_TRIGGERS_H_
