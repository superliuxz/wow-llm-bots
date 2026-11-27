// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/encounter_phase.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace llm_brain {

uint32_t PhaseFromEventMapMask(uint8_t phase_mask, bool in_combat) {
  if (phase_mask != 0) {
    return static_cast<uint32_t>(std::bit_width(phase_mask));
  }
  return in_combat ? 1u : 0u;
}

int SelectEncounterBossIndex(
    const std::vector<EncounterBossCandidate>& candidates) {
  int best_index = -1;
  for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
    const EncounterBossCandidate& c = candidates[static_cast<size_t>(i)];
    if (!c.is_boss || c.dead || c.friendly) {
      continue;
    }
    if (best_index < 0) {
      best_index = i;
      continue;
    }
    const EncounterBossCandidate& best =
        candidates[static_cast<size_t>(best_index)];
    const bool c_active = c.in_combat || c.engaged;
    const bool best_active = best.in_combat || best.engaged;
    if (c_active != best_active) {
      if (c_active) {
        best_index = i;
      }
      continue;
    }
    if (c.is_target != best.is_target) {
      if (c.is_target) {
        best_index = i;
      }
      continue;
    }
    if (c.dist_yd < best.dist_yd) {
      best_index = i;
    }
  }
  return best_index;
}

}  // namespace llm_brain
