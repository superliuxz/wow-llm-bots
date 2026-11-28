// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/errand_hold.h"

#include <cstdint>
#include <type_traits>
#include <variant>

#include "options/action_types.h"

namespace llm_brain {
namespace {

bool CanHoldErrand(const ErrandHoldInput& input) {
  return input.in_group && !input.is_leader && input.alive &&
         !input.in_combat && !input.party_in_combat && !input.in_battleground &&
         input.harness_connected;
}

}  // namespace

uint32_t ErrandTimeoutMs(ErrandKind kind) {
  switch (kind) {
    case ErrandKind::kNone:
      return 0;
    case ErrandKind::kLoot:
    case ErrandKind::kGather:
    case ErrandKind::kUseObject:
    case ErrandKind::kUseCast:
    case ErrandKind::kTalk:
    case ErrandKind::kRest:
      return kDefaultErrandTimeoutMs;
    case ErrandKind::kNpcInteract:
      return kNpcInteractionGraceMs;
    case ErrandKind::kWalk:
      return kWalkErrandTimeoutMs;
  }
  return 0;
}

ErrandKind ErrandKindForAction(const BotAction& action) {
  return std::visit(
      [](const auto& a) -> ErrandKind {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, LootAction>) {
          return ErrandKind::kLoot;
        } else if constexpr (std::is_same_v<T, GatherAction>) {
          return ErrandKind::kGather;
        } else if constexpr (std::is_same_v<T, UseObjectAction>) {
          return ErrandKind::kUseObject;
        } else if constexpr (std::is_same_v<T, UseBagItemAction> ||
                             std::is_same_v<T, MountAction>) {
          return ErrandKind::kUseCast;
        } else if constexpr (std::is_same_v<T, TalkAction>) {
          return ErrandKind::kTalk;
        } else if constexpr (std::is_same_v<T, NpcAction>) {
          return a.op == NpcAction::Op::kFlyTo ? ErrandKind::kNone
                                               : ErrandKind::kNpcInteract;
        } else if constexpr (std::is_same_v<T, ObjectQuestAction>) {
          return ErrandKind::kNpcInteract;
        } else if constexpr (std::is_same_v<T, WalkAction>) {
          return ErrandKind::kWalk;
        } else if constexpr (std::is_same_v<T, RestAction>) {
          return ErrandKind::kRest;
        } else {
          return ErrandKind::kNone;
        }
      },
      action);
}

bool ActionCancelsErrandHold(const BotAction& action) {
  return std::visit(
      [](const auto& a) -> bool {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, AttackAction> ||
                      std::is_same_v<T, CrowdControlAction> ||
                      std::is_same_v<T, TauntAction> ||
                      std::is_same_v<T, FollowLeaderAction> ||
                      std::is_same_v<T, StandingActAction>) {
          return true;
        } else if constexpr (std::is_same_v<T, GroupPolicyAction>) {
          return a.op == GroupPolicyAction::Op::kFollow;
        } else {
          return false;
        }
      },
      action);
}

bool StartErrandHold(ErrandHoldState& state,
                     ErrandKind kind,
                     const ErrandHoldInput& input,
                     uint32_t timeout_override_ms) {
  const uint32_t timeout_ms =
      timeout_override_ms != 0 ? timeout_override_ms : ErrandTimeoutMs(kind);
  if (kind == ErrandKind::kNone || timeout_ms == 0 || !CanHoldErrand(input)) {
    state = {};
    return false;
  }
  state = ErrandHoldState{
      .active = true,
      .kind = kind,
      .started_ms = input.now_ms,
      .timeout_ms = timeout_ms,
      .map_id = input.map_id,
      .instance_id = input.instance_id,
  };
  return true;
}

bool EvaluateErrandHold(ErrandHoldState& state, const ErrandHoldInput& input) {
  if (!state.active) {
    return false;
  }
  if (!CanHoldErrand(input) || input.map_id != state.map_id ||
      input.instance_id != state.instance_id) {
    state = {};
    return false;
  }
  // Unsigned subtraction handles uint32_t getMSTime() wrap-around cleanly.
  const uint32_t elapsed_ms = input.now_ms - state.started_ms;
  if (elapsed_ms >= state.timeout_ms) {
    state = {};
    return false;
  }
  return true;
}

bool ClearErrandHold(ErrandHoldState& state) {
  const bool was_active = state.active;
  state = {};
  return was_active;
}

}  // namespace llm_brain
