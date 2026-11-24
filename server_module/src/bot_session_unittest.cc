// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_session.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(BotSessionRegistryTest, UnifiedResetsClearLifecycleState) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1001;

  reg.WithSession(kBot, [](BotSession& s) {
    s.dungeon_dead_since_ms = 1234;
    s.rez_wait_until_ms = 9999;
    s.group_guid = 42;
    s.group_policy.stay = true;
    s.pending_shared_quest_id = 104;
    s.pending_ready_check_ms = 500;
    s.pending_lfg_proposal_id = 7;
    s.avoided_targets = {0x99, 0x100};
    s.last_interrupted.active = true;
    s.last_interrupted.map_id = 602;
  });
  EXPECT_EQ(reg.size(), 1u);

  // Map change clears act coordinates & avoided targets while keeping group
  // policy and pending prompts intact.
  reg.ResetOnMapChange(kBot);
  reg.InspectSession(kBot, [](const BotSession* s) {
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->last_interrupted.active);
    EXPECT_TRUE(s->avoided_targets.empty());
    EXPECT_TRUE(s->group_policy.stay);
    EXPECT_EQ(s->pending_shared_quest_id, 104u);
  });

  // Logout removes the entire per-bot session cleanly.
  reg.ResetOnLogout(kBot);
  EXPECT_EQ(reg.size(), 0u);
  reg.InspectSession(kBot, [](const BotSession* s) { EXPECT_EQ(s, nullptr); });
}

}  // namespace
}  // namespace llm_brain
