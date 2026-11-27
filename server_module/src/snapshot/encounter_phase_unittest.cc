// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/encounter_phase.h"

#include <gtest/gtest.h>

#include <vector>

namespace llm_brain {
namespace {

TEST(EncounterPhaseTest, PhaseFromEventMapMask) {
  // Out of combat with no phase bits set: phase 0 (no active phase).
  EXPECT_EQ(PhaseFromEventMapMask(0u, /*in_combat=*/false), 0u);
  // In combat with no phase bits set: defaults to phase 1.
  EXPECT_EQ(PhaseFromEventMapMask(0u, /*in_combat=*/true), 1u);

  // Single-bit SetPhase(N) masks: bit (N-1) -> phase N.
  EXPECT_EQ(PhaseFromEventMapMask(1u << 0, /*in_combat=*/true), 1u);
  EXPECT_EQ(PhaseFromEventMapMask(1u << 1, /*in_combat=*/true), 2u);
  EXPECT_EQ(PhaseFromEventMapMask(1u << 2, /*in_combat=*/true), 3u);
  EXPECT_EQ(PhaseFromEventMapMask(1u << 7, /*in_combat=*/true), 8u);

  // Stacked AddPhase(2) on top of phase 1: highest active phase wins.
  EXPECT_EQ(PhaseFromEventMapMask((1u << 0) | (1u << 1), /*in_combat=*/true),
            2u);
  // Explicit phase bit set before combat still reports that phase.
  EXPECT_EQ(PhaseFromEventMapMask(1u << 1, /*in_combat=*/false), 2u);
}

TEST(EncounterPhaseTest,
     SelectEncounterBossIndexPrefersActiveThenTargetThenNear) {
  // Empty / non-boss / dead / friendly candidates return -1.
  EXPECT_EQ(SelectEncounterBossIndex({}), -1);
  EXPECT_EQ(SelectEncounterBossIndex({
                EncounterBossCandidate{.is_boss = false, .in_combat = true},
                EncounterBossCandidate{.is_boss = true, .dead = true},
                EncounterBossCandidate{.is_boss = true, .friendly = true},
            }),
            -1);

  // Active (engaged/in_combat) boss beats closer idle boss.
  const std::vector<EncounterBossCandidate> active_vs_idle = {
      {.is_boss = true, .in_combat = false, .is_target = true, .dist_yd = 5.0f},
      {.is_boss = true,
       .in_combat = true,
       .is_target = false,
       .dist_yd = 40.0f},
  };
  EXPECT_EQ(SelectEncounterBossIndex(active_vs_idle), 1);

  // Among two active bosses (e.g. Blood Prince Council), bot's target wins.
  const std::vector<EncounterBossCandidate> two_active = {
      {.is_boss = true, .in_combat = true, .is_target = false, .dist_yd = 8.0f},
      {.is_boss = true, .in_combat = true, .is_target = true, .dist_yd = 25.0f},
  };
  EXPECT_EQ(SelectEncounterBossIndex(two_active), 1);

  // When neither is targeted, closest active boss wins.
  const std::vector<EncounterBossCandidate> closest = {
      {.is_boss = true, .engaged = true, .dist_yd = 30.0f},
      {.is_boss = true, .engaged = true, .dist_yd = 12.0f},
  };
  EXPECT_EQ(SelectEncounterBossIndex(closest), 1);
}

}  // namespace
}  // namespace llm_brain
