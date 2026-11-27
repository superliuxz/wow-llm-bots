// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure helpers for mapping AzerothCore boss AI state into snapshot encounter
// fields (`Unit::boss.phase` and `Extensions::encounter.boss`).

#ifndef SERVER_MODULE_SNAPSHOT_ENCOUNTER_PHASE_H_
#define SERVER_MODULE_SNAPSHOT_ENCOUNTER_PHASE_H_

#include <cstdint>
#include <vector>

namespace llm_brain {

// Converts an AzerothCore `EventMap::GetPhaseMask()` bitmask (bit 0 = phase 1
// .. bit 7 = phase 8) into a 1-based encounter phase number. When no phase bit
// is set, returns 1 while the boss is in combat (many boss scripts only call
// `SetPhase(2)` on the first transition) and 0 out of combat.
uint32_t PhaseFromEventMapMask(uint8_t phase_mask, bool in_combat);

struct EncounterBossCandidate {
  bool is_boss = false;
  bool dead = false;
  bool friendly = false;
  bool in_combat = false;
  bool engaged = false;
  bool is_target = false;
  float dist_yd = 0.0f;
};

// Returns the index of the primary living boss to expose in
// `snapshot.ext.encounter->boss`, or -1 if none qualify. Prefers an actively
// engaged/in-combat boss over an idle boss, then the bot's current target,
// then the closest boss.
int SelectEncounterBossIndex(
    const std::vector<EncounterBossCandidate>& candidates);

}  // namespace llm_brain

#endif  // SERVER_MODULE_SNAPSHOT_ENCOUNTER_PHASE_H_
