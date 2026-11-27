// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/human_group_grace.h"

#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

constexpr uint64_t kGroupGuid = 0x50001;
constexpr uint64_t kOtherGroupGuid = 0x50002;

TEST(HumanGroupGraceTest, GraceDurationsMatchMapType) {
  EXPECT_EQ(OfflineGraceMs(/*in_instance=*/false), kOpenWorldOfflineGraceMs);
  EXPECT_EQ(OfflineGraceMs(/*in_instance=*/true), kInstanceOfflineGraceMs);
  EXPECT_EQ(kOpenWorldOfflineGraceMs, 60u * 1000u);
  EXPECT_EQ(kInstanceOfflineGraceMs, 30u * 60u * 1000u);
  EXPECT_GT(kInstanceOfflineGraceMs, kOpenWorldOfflineGraceMs);
}

// Counter-test: open-world groups still abandon after 60 s so stray bot
// parties do not linger indefinitely outside instances.
TEST(HumanGroupGraceTest, OpenWorldLeavesAfterSixtySeconds) {
  HumanGroupGraceState state;
  // Human is online -> records human_group_guid.
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = true,
                                            .in_instance = false,
                                            .now_ms = 1000}),
            HumanGroupAction::kKeep);
  EXPECT_EQ(state.human_group_guid, kGroupGuid);
  EXPECT_FALSE(state.human_offline_since_ms.has_value());

  // Human disconnects -> starts timer at t=2000.
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = false,
                                            .has_offline_human = true,
                                            .in_instance = false,
                                            .now_ms = 2000}),
            HumanGroupAction::kKeep);
  ASSERT_TRUE(state.human_offline_since_ms.has_value());
  EXPECT_EQ(*state.human_offline_since_ms, 2000u);

  // Just before 60 s: still waiting.
  EXPECT_EQ(EvaluateHumanGroupGrace(
                state, {.group_guid = kGroupGuid,
                        .has_online_human = false,
                        .has_offline_human = true,
                        .in_instance = false,
                        .now_ms = 2000 + kOpenWorldOfflineGraceMs - 1}),
            HumanGroupAction::kKeep);
  EXPECT_EQ(state.human_group_guid, kGroupGuid);

  // At 60 s: abandons group and clears state.
  EXPECT_EQ(EvaluateHumanGroupGrace(
                state, {.group_guid = kGroupGuid,
                        .has_online_human = false,
                        .has_offline_human = true,
                        .in_instance = false,
                        .now_ms = 2000 + kOpenWorldOfflineGraceMs}),
            HumanGroupAction::kAbandonGroup);
  EXPECT_EQ(state.human_group_guid, 0u);
  EXPECT_FALSE(state.human_offline_since_ms.has_value());
}

// Core behavior (M1a #1): inside a dungeon or raid, bots survive past 60 s
// (client crash / long loading screen) and wait up to 30 min.
TEST(HumanGroupGraceTest, DungeonAndRaidWaitPastSixtySeconds) {
  HumanGroupGraceState state;
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = true,
                                            .in_instance = true,
                                            .now_ms = 10'000}),
            HumanGroupAction::kKeep);

  // Human crashes at t=11,000.
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = false,
                                            .has_offline_human = true,
                                            .in_instance = true,
                                            .now_ms = 11'000}),
            HumanGroupAction::kKeep);

  // At 60 s, 5 min, and 29m59.999s: still waiting in the instance.
  for (uint32_t elapsed : {kOpenWorldOfflineGraceMs, 5u * 60u * 1000u,
                           kInstanceOfflineGraceMs - 1}) {
    EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                              .has_online_human = false,
                                              .has_offline_human = true,
                                              .in_instance = true,
                                              .now_ms = 11'000 + elapsed}),
              HumanGroupAction::kKeep)
        << "elapsed=" << elapsed;
    EXPECT_EQ(state.human_group_guid, kGroupGuid);
  }

  // At 30 min: finally abandons and cleans up.
  EXPECT_EQ(EvaluateHumanGroupGrace(
                state, {.group_guid = kGroupGuid,
                        .has_online_human = false,
                        .has_offline_human = true,
                        .in_instance = true,
                        .now_ms = 11'000 + kInstanceOfflineGraceMs}),
            HumanGroupAction::kAbandonGroup);
  EXPECT_EQ(state.human_group_guid, 0u);
  EXPECT_FALSE(state.human_offline_since_ms.has_value());
}

// Leader reconnect resets the offline timer so a later disconnect gets a full
// fresh grace window.
TEST(HumanGroupGraceTest, LeaderReconnectResetsOfflineTimer) {
  HumanGroupGraceState state;
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = true,
                                            .in_instance = true,
                                            .now_ms = 1000}),
            HumanGroupAction::kKeep);

  // First disconnect for 20 minutes.
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = false,
                                            .has_offline_human = true,
                                            .in_instance = true,
                                            .now_ms = 2000}),
            HumanGroupAction::kKeep);
  EXPECT_EQ(
      EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                      .has_online_human = false,
                                      .has_offline_human = true,
                                      .in_instance = true,
                                      .now_ms = 2000 + 20u * 60u * 1000u}),
      HumanGroupAction::kKeep);

  // Human reconnects -> timer clears.
  const uint32_t reconnect_ms = 2000 + 20u * 60u * 1000u + 1000;
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = true,
                                            .in_instance = true,
                                            .now_ms = reconnect_ms}),
            HumanGroupAction::kKeep);
  EXPECT_EQ(state.human_group_guid, kGroupGuid);
  EXPECT_FALSE(state.human_offline_since_ms.has_value());

  // Second disconnect starts a fresh 30-min window (does not combine with the
  // earlier 20 min).
  const uint32_t second_dc_ms = reconnect_ms + 5000;
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = false,
                                            .has_offline_human = true,
                                            .in_instance = true,
                                            .now_ms = second_dc_ms}),
            HumanGroupAction::kKeep);
  EXPECT_EQ(EvaluateHumanGroupGrace(
                state, {.group_guid = kGroupGuid,
                        .has_online_human = false,
                        .has_offline_human = true,
                        .in_instance = true,
                        .now_ms = second_dc_ms + 20u * 60u * 1000u}),
            HumanGroupAction::kKeep);
}

// Adversarial counter-test: if the human explicitly leaves the group (no
// offline human slot remains), bots STILL leave and teleport out immediately
// even inside a dungeon or raid (required by LFG e2e).
TEST(HumanGroupGraceTest, ExplicitHumanLeaveInInstanceAbandonsImmediately) {
  HumanGroupGraceState state;
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = true,
                                            .in_instance = true,
                                            .now_ms = 1000}),
            HumanGroupAction::kKeep);

  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = false,
                                            .has_offline_human = false,
                                            .in_instance = true,
                                            .now_ms = 2000}),
            HumanGroupAction::kAbandonGroup);
  EXPECT_EQ(state.human_group_guid, 0u);
  EXPECT_FALSE(state.human_offline_since_ms.has_value());
}

// Adversarial counter-test: when the group actually disbands (group_guid == 0),
// bots clean up once out of combat and never teleport mid-combat.
TEST(HumanGroupGraceTest, GroupDisbandCleansUpAfterCombat) {
  HumanGroupGraceState state;
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = true,
                                            .in_instance = true,
                                            .now_ms = 1000}),
            HumanGroupAction::kKeep);

  // Group disbanded while in combat: wait until combat ends.
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = 0,
                                            .in_instance = true,
                                            .in_combat = true,
                                            .now_ms = 2000}),
            HumanGroupAction::kKeep);
  EXPECT_EQ(state.human_group_guid, kGroupGuid);

  // Combat ends: clean up and clear session state.
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = 0,
                                            .in_instance = true,
                                            .in_combat = false,
                                            .now_ms = 3000}),
            HumanGroupAction::kCleanupAfterGroupGone);
  EXPECT_EQ(state.human_group_guid, 0u);
  EXPECT_FALSE(state.human_offline_since_ms.has_value());

  // Subsequent ticks while solo do nothing.
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = 0,
                                            .in_instance = false,
                                            .in_combat = false,
                                            .now_ms = 4000}),
            HumanGroupAction::kKeep);
}

// Adversarial counter-test: combat defers abandonment even after grace expires,
// and if the human reconnects during combat, the group is kept.
TEST(HumanGroupGraceTest, CombatDefersAbandonmentAndAllowsReconnect) {
  HumanGroupGraceState state;
  EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                  .has_online_human = true,
                                  .in_instance = false,
                                  .now_ms = 1000});
  EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                  .has_online_human = false,
                                  .has_offline_human = true,
                                  .in_instance = false,
                                  .now_ms = 2000});

  // Grace expired at 62,000, but bot is in combat -> hold.
  EXPECT_EQ(EvaluateHumanGroupGrace(
                state, {.group_guid = kGroupGuid,
                        .has_online_human = false,
                        .has_offline_human = true,
                        .in_instance = false,
                        .in_combat = true,
                        .now_ms = 2000 + kOpenWorldOfflineGraceMs}),
            HumanGroupAction::kKeep);
  EXPECT_EQ(state.human_group_guid, kGroupGuid);

  // Human reconnects before combat ends -> resets timer.
  EXPECT_EQ(EvaluateHumanGroupGrace(
                state, {.group_guid = kGroupGuid,
                        .has_online_human = true,
                        .in_instance = false,
                        .in_combat = true,
                        .now_ms = 2000 + kOpenWorldOfflineGraceMs + 1000}),
            HumanGroupAction::kKeep);
  EXPECT_FALSE(state.human_offline_since_ms.has_value());

  // Combat ends with human still online -> group is kept.
  EXPECT_EQ(EvaluateHumanGroupGrace(
                state, {.group_guid = kGroupGuid,
                        .has_online_human = true,
                        .in_instance = false,
                        .in_combat = false,
                        .now_ms = 2000 + kOpenWorldOfflineGraceMs + 2000}),
            HumanGroupAction::kKeep);
}

// Adversarial counter-test: bot-only groups are never abandoned, even if the
// bot was previously in a different human group.
TEST(HumanGroupGraceTest, BotOnlyGroupIsNeverAbandoned) {
  HumanGroupGraceState state;
  // Pure bot-only group from the start.
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = false,
                                            .has_offline_human = false,
                                            .in_instance = true,
                                            .now_ms = 1000}),
            HumanGroupAction::kKeep);

  // Was in human group kGroupGuid, then switched to bot-only kOtherGroupGuid.
  EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                  .has_online_human = true,
                                  .in_instance = false,
                                  .now_ms = 2000});
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kOtherGroupGuid,
                                            .has_online_human = false,
                                            .has_offline_human = false,
                                            .in_instance = false,
                                            .now_ms = 3000}),
            HumanGroupAction::kKeep);
  EXPECT_EQ(state.human_group_guid, 0u);
  EXPECT_FALSE(state.human_offline_since_ms.has_value());
}

// Adversarial counter-test: uint32_t getMSTime() wraparound across 0xFFFFFFFF
// does not cause premature or missed abandonment.
TEST(HumanGroupGraceTest, HandlesMsTimeWraparound) {
  HumanGroupGraceState state;
  const uint32_t near_max = std::numeric_limits<uint32_t>::max() - 10'000u;
  EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                  .has_online_human = true,
                                  .in_instance = true,
                                  .now_ms = near_max - 1000u});
  EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                  .has_online_human = false,
                                  .has_offline_human = true,
                                  .in_instance = true,
                                  .now_ms = near_max});

  // 20 s after disconnect (wrapped past 0): still waiting in instance.
  const uint32_t after_wrap = near_max + 20'000u;
  EXPECT_LT(after_wrap, near_max);
  EXPECT_EQ(EvaluateHumanGroupGrace(state, {.group_guid = kGroupGuid,
                                            .has_online_human = false,
                                            .has_offline_human = true,
                                            .in_instance = true,
                                            .now_ms = after_wrap}),
            HumanGroupAction::kKeep);

  // At 30 min after wrap: abandons cleanly.
  EXPECT_EQ(EvaluateHumanGroupGrace(
                state, {.group_guid = kGroupGuid,
                        .has_online_human = false,
                        .has_offline_human = true,
                        .in_instance = true,
                        .now_ms = near_max + kInstanceOfflineGraceMs}),
            HumanGroupAction::kAbandonGroup);
}

}  // namespace
}  // namespace llm_brain
