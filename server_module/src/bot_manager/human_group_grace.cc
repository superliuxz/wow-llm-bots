// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/human_group_grace.h"

#include <cstdint>

namespace llm_brain {

uint32_t OfflineGraceMs(bool in_instance) {
  return in_instance ? kInstanceOfflineGraceMs : kOpenWorldOfflineGraceMs;
}

HumanGroupAction EvaluateHumanGroupGrace(HumanGroupGraceState& state,
                                         const HumanGroupGraceInput& input) {
  if (input.group_guid == 0) {
    if (state.human_group_guid == 0) {
      state.human_offline_since_ms.reset();
      return HumanGroupAction::kKeep;
    }
    if (input.in_combat) {
      return HumanGroupAction::kKeep;
    }
    state.human_group_guid = 0;
    state.human_offline_since_ms.reset();
    return HumanGroupAction::kCleanupAfterGroupGone;
  }

  if (input.has_online_human) {
    state.human_group_guid = input.group_guid;
    state.human_offline_since_ms.reset();
    return HumanGroupAction::kKeep;
  }

  if (state.human_group_guid != input.group_guid) {
    state.human_group_guid = 0;
    state.human_offline_since_ms.reset();
    return HumanGroupAction::kKeep;
  }

  if (input.has_offline_human) {
    if (!state.human_offline_since_ms) {
      state.human_offline_since_ms = input.now_ms;
      return HumanGroupAction::kKeep;
    }
    const uint32_t elapsed_ms = input.now_ms - *state.human_offline_since_ms;
    if (elapsed_ms < OfflineGraceMs(input.in_instance)) {
      return HumanGroupAction::kKeep;
    }
  }

  if (input.in_combat) {
    return HumanGroupAction::kKeep;
  }

  state.human_group_guid = 0;
  state.human_offline_since_ms.reset();
  return HumanGroupAction::kAbandonGroup;
}

}  // namespace llm_brain
