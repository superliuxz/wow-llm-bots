// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure C++ errand-hold state machine (Milestone 5a).
//
// For a grouped follower, playerbots' `follow`, BotManager::KeepSafeGroupFollow
// (5.5 yd), and BotManager::KeepNearHumanLeader (40 yd leash) would otherwise
// cancel brain-initiated loot, gather, talk, use, rest, and walk moves or
// interrupt their casts. While a brain errand is active, this state machine
// pauses follow and the leash with a bounded timeout so a stuck errand never
// permanently orphans the bot, and immediately yields on P0 combat (self or
// party), completion, timeout, death, map/instance change, or disconnect.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_ERRAND_HOLD_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_ERRAND_HOLD_H_

#include <cstdint>

#include "options/action_types.h"

namespace llm_brain {

// Maximum duration a single brain-initiated errand (loot, gather, use, talk,
// rest) may pause follow and the 40 yd leash before automatically timing out.
inline constexpr uint32_t kDefaultErrandTimeoutMs = 30000;

// Maximum duration a brain-initiated walk (`kGoTo` / `kExplore`) may pause
// follow and the 40 yd leash before timing out.
inline constexpr uint32_t kWalkErrandTimeoutMs = 60000;

// Brief grace window after arriving at an NPC (`Talk`) or completing an NPC
// interaction (quest accept/turn-in, vendor, train, buy, bind) so the brain
// can issue the follow-up click before `follow` pulls the bot out of talk
// range.
inline constexpr uint32_t kNpcInteractionGraceMs = 5000;

enum class ErrandKind : uint8_t {
  kNone,
  kLoot,
  kGather,
  kUseObject,
  kUseCast,
  kTalk,
  kNpcInteract,
  kWalk,
  kRest,
};

struct ErrandHoldState {
  bool active = false;
  ErrandKind kind = ErrandKind::kNone;
  uint32_t started_ms = 0;
  uint32_t timeout_ms = 0;
  uint32_t map_id = 0;
  uint32_t instance_id = 0;

  bool operator==(const ErrandHoldState&) const = default;
};

struct ErrandHoldInput {
  bool in_group = false;
  bool is_leader = false;
  bool alive = true;
  bool in_combat = false;
  bool party_in_combat = false;
  bool in_battleground = false;
  bool harness_connected = true;
  uint32_t map_id = 0;
  uint32_t instance_id = 0;
  uint32_t now_ms = 0;
};

// Default timeout in ms for `kind` (0 for `kNone`).
uint32_t ErrandTimeoutMs(ErrandKind kind);

// Classifies a `BotAction` into the `ErrandKind` that should pause follow and
// the 40 yd leash while running, or `ErrandKind::kNone` if it is not an errand.
ErrandKind ErrandKindForAction(const BotAction& action);

// True if `action` explicitly cancels any active errand hold (combat actions,
// explicit follow, or standing-order acts).
bool ActionCancelsErrandHold(const BotAction& action);

// Starts (or replaces) an errand hold for a grouped follower out of combat.
// Clears `state` and returns false when `kind == kNone`, `timeout == 0`, solo,
// group leader, dead, in combat, party in combat, in a battleground, or
// disconnected.
bool StartErrandHold(ErrandHoldState& state,
                     ErrandKind kind,
                     const ErrandHoldInput& input,
                     uint32_t timeout_override_ms = 0);

// Evaluates whether an active errand hold still pauses follow and the 40 yd
// leash on this tick. Immediately clears `state` and returns false when not
// active, solo, leader, dead, P0 combat starts (self or party), in a
// battleground, disconnected, map/instance changed, or timed out.
bool EvaluateErrandHold(ErrandHoldState& state, const ErrandHoldInput& input);

// Unconditionally clears an active errand hold. Returns true if one was active.
bool ClearErrandHold(ErrandHoldState& state);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_ERRAND_HOLD_H_
