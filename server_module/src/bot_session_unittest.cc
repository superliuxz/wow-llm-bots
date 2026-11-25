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
    s.dead_since_ms = 1234;
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

  // Logout clears everything but the group policy: a relog keeps its group.
  reg.ResetOnLogout(kBot);
  reg.InspectSession(kBot, [](const BotSession* s) {
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->group_guid, 42u);
    EXPECT_TRUE(s->group_policy.stay);
    EXPECT_FALSE(s->dead_since_ms.has_value());
    EXPECT_FALSE(s->rez_wait_until_ms.has_value());
    EXPECT_EQ(s->pending_shared_quest_id, 0u);
    EXPECT_FALSE(s->pending_ready_check_ms.has_value());
    EXPECT_TRUE(s->avoided_targets.empty());
  });

  // With no group policy, logout removes the whole session.
  reg.WithSession(kBot, [](BotSession& s) {
    s.group_guid = 0;
    s.group_policy = {};
  });
  reg.ResetOnLogout(kBot);
  EXPECT_EQ(reg.size(), 0u);
  reg.InspectSession(kBot, [](const BotSession* s) { EXPECT_EQ(s, nullptr); });
}

}  // namespace
}  // namespace llm_brain
