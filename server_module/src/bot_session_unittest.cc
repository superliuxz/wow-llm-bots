// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_session.h"

#include <cstdint>

#include "base/time/time.h"
#include "bot_manager/errand_hold.h"
#include "bot_manager/pull_discipline.h"
#include "bot_manager/tank_swap.h"
#include "gtest/gtest.h"
#include "options/bot_leader_guard.h"
#include "options/loot_roll_policy.h"

namespace llm_brain {
namespace {

TEST(BotSessionRegistryTest, UnifiedResetsClearLifecycleState) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1001;

  reg.WithSession(kBot, [](BotSession& s) {
    s.dead_since_ms = 1234;
    s.rez_wait_until_ms = 9999;
    s.group_guid = 42;
    s.group_policy.not_tank = true;
    s.group_policy.stay = true;
    s.group_policy.stay_in_combat = true;
    s.pending_shared_quests.push_back(
        {.quest_id = 104, .sharer_guid = 0x55, .received_ms = 100});
    s.pending_ready_check_ms = 500;
    s.pending_lfg_proposal_id = 7;
    s.avoided_targets = {0x99, 0x100};
    s.last_interrupted.active = true;
    s.last_interrupted.map_id = 602;
    s.party_ping = BotSession::PartyPing{.from_guid = 7, .from = "Allyone"};
  });
  EXPECT_EQ(reg.size(), 1u);

  // Map change clears act coordinates, avoided targets, and transient stay
  // holds while keeping role policy (`not_tank`) and pending prompts intact.
  reg.ResetOnMapChange(kBot);
  reg.InspectSession(kBot, [](const BotSession* s) {
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->last_interrupted.active);
    EXPECT_TRUE(s->avoided_targets.empty());
    EXPECT_FALSE(s->party_ping.has_value());
    EXPECT_TRUE(s->group_policy.not_tank);
    EXPECT_FALSE(s->group_policy.stay);
    EXPECT_FALSE(s->group_policy.stay_in_combat);
    EXPECT_TRUE(s->pending_shared_quests.empty());
  });

  // Logout clears everything but the group role policy (`not_tank`): a relog
  // keeps its group without staying frozen in `stay`.
  reg.WithSession(kBot, [](BotSession& s) {
    s.group_policy.stay = true;
    s.group_policy.stay_in_combat = true;
    s.pending_shared_quests.push_back(
        {.quest_id = 105, .sharer_guid = 0x55, .received_ms = 200});
  });
  reg.ResetOnLogout(kBot);
  reg.InspectSession(kBot, [](const BotSession* s) {
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->group_guid, 42u);
    EXPECT_TRUE(s->group_policy.not_tank);
    EXPECT_FALSE(s->group_policy.stay);
    EXPECT_FALSE(s->group_policy.stay_in_combat);
    EXPECT_FALSE(s->dead_since_ms.has_value());
    EXPECT_FALSE(s->rez_wait_until_ms.has_value());
    EXPECT_TRUE(s->pending_shared_quests.empty());
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

// A party ping never outlives a harness disconnect or a relog, while a relog
// still keeps the group policy (adversarial counter-test for the reset hooks).
TEST(BotSessionRegistryTest, PartyPingClearsOnDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1002;
  const auto ping = BotSession::PartyPing{
      .from_guid = 7, .from = "Allyone", .x = 1.0f, .y = 2.0f, .ms = 10};
  reg.WithSession(kBot, [&](BotSession& s) {
    s.group_guid = 42;
    s.party_ping = ping;
  });
  reg.ResetOnDisconnect(kBot);
  reg.InspectSession(kBot, [](const BotSession* s) {
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->party_ping.has_value());
    EXPECT_EQ(s->group_guid, 42u);
  });
  reg.WithSession(kBot, [&](BotSession& s) { s.party_ping = ping; });
  reg.ResetOnLogout(kBot);
  reg.InspectSession(kBot, [](const BotSession* s) {
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->party_ping.has_value());
    EXPECT_EQ(s->group_guid, 42u);
  });
}

// A `to ping` hold (and a dodge's or a stealth walk-in's hold on playerbots'
// casts) never outlives a map change, a harness disconnect or a relog (the
// group policy, `Wait here`, does survive a relog).
TEST(BotSessionRegistryTest, PingStayClearsOnMapChangeDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1003;
  const auto stay =
      BotSession::PingStay{.map_id = 36, .x = 1.0f, .y = 2.0f, .z = 3.0f};
  const auto set = [&] {
    reg.WithSession(kBot, [&](BotSession& s) {
      s.group_guid = 42;
      s.group_policy.not_tank = true;
      s.group_policy.stay = true;
      s.ping_stay = stay;
      s.moving_out_until_ms = 12345;
      s.act_stealth_until_ms = 23456;
    });
  };
  const auto expect_cleared = [&] {
    reg.InspectSession(kBot, [](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_FALSE(s->ping_stay.has_value());
      EXPECT_EQ(s->moving_out_until_ms, 0u);   // A dodge's cast hold too.
      EXPECT_EQ(s->act_stealth_until_ms, 0u);  // And a Sap walk-in's.
      EXPECT_EQ(s->group_guid, 42u);
      EXPECT_TRUE(s->group_policy.not_tank);
      EXPECT_FALSE(s->group_policy.stay);
    });
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_cleared();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_cleared();
  set();
  reg.ResetOnLogout(kBot);
  expect_cleared();
}

// Adversarial counter-test: a spell miss and cast failure never outlive a map
// change, harness disconnect, or logout, while group policy still survives.
TEST(BotSessionRegistryTest, SpellMissClearsOnMapChangeDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1004;
  const auto set = [&] {
    reg.WithSession(kBot, [&](BotSession& s) {
      s.group_guid = 42;
      s.group_policy.not_tank = true;
      s.group_policy.stay = true;
      s.spell_miss = BotSession::SpellMiss{
          .seq = ++s.spell_miss_seq,
          .spell_id = 6770,
          .spell = "Sap",
          .target_guid = 0x999,
          .target = "Defias Cutpurse",
          .reason = "dodge",
          .map_id = 36,
          .ms = 1000,
      };
      s.cast_failure = BotSession::CastFailure{
          .seq = ++s.cast_failure_seq,
          .spell_id = 6770,
          .result = 38,
          .ms = 1000,
      };
    });
  };
  const auto expect_cleared = [&] {
    reg.InspectSession(kBot, [](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_FALSE(s->spell_miss.has_value());
      EXPECT_FALSE(s->cast_failure.has_value());
      EXPECT_EQ(s->group_guid, 42u);
      EXPECT_TRUE(s->group_policy.not_tank);
      EXPECT_FALSE(s->group_policy.stay);
    });
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_cleared();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_cleared();
  set();
  reg.ResetOnLogout(kBot);
  expect_cleared();
}

// Adversarial counter-test: human_group_guid survives map change, harness
// disconnect, and relog (the bot is still in that human's group), while
// human_offline_since_ms resets cleanly so zoning out of a dungeon or
// reconnecting never triggers an immediate stale-timer abandonment.
TEST(BotSessionRegistryTest, HumanGroupGraceResetsCleanly) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1005;
  const auto set = [&] {
    reg.WithSession(kBot, [](BotSession& s) {
      s.human_group_guid = 99;
      s.human_offline_since_ms = 123456;
    });
  };
  const auto expect_clean = [&] {
    reg.InspectSession(kBot, [](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_EQ(s->human_group_guid, 99u);
      EXPECT_FALSE(s->human_offline_since_ms.has_value());
    });
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_clean();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_clean();
  set();
  reg.ResetOnLogout(kBot);
  expect_clean();
}

// Adversarial counter-test: active "stop dps", transient "stay" / combat-hold,
// and skull RTI suppression clear on map change, harness disconnect, and logout
// so a bot never stays muted or frozen after reconnect, while role policy
// (`not_tank`) still survives.
TEST(BotSessionRegistryTest, StopDpsClearsOnMapChangeDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1006;
  const auto set = [&] {
    reg.WithSession(kBot, [](BotSession& s) {
      s.group_guid = 42;
      s.group_policy.not_tank = true;
      s.group_policy.stay = true;
      s.group_policy.stay_in_combat = true;
      s.stop_dps = true;
      s.skull_rti_suppressed = true;
    });
  };
  const auto expect_cleared = [&] {
    reg.InspectSession(kBot, [](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_FALSE(s->stop_dps);
      EXPECT_FALSE(s->skull_rti_suppressed);
      EXPECT_EQ(s->group_guid, 42u);
      EXPECT_TRUE(s->group_policy.not_tank);
      EXPECT_FALSE(s->group_policy.stay);
      EXPECT_FALSE(s->group_policy.stay_in_combat);
    });
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_cleared();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_cleared();
  set();
  reg.ResetOnLogout(kBot);
  expect_cleared();
}

// Adversarial counter-test: an active errand hold never outlives a map change,
// harness disconnect, or logout, while role policy (`not_tank`) still survives.
TEST(BotSessionRegistryTest, ErrandHoldClearsOnMapChangeDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1007;
  const auto set = [&] {
    reg.WithSession(kBot, [](BotSession& s) {
      s.group_guid = 42;
      s.group_policy.not_tank = true;
      s.group_policy.stay = true;
      s.errand_hold = ErrandHoldState{
          .active = true,
          .kind = ErrandKind::kLoot,
          .started_ms = 1000,
          .timeout_ms = kDefaultErrandTimeoutMs,
          .map_id = 36,
          .instance_id = 1,
      };
    });
  };
  const auto expect_cleared = [&] {
    reg.InspectSession(kBot, [](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_FALSE(s->errand_hold.active);
      EXPECT_EQ(s->errand_hold.kind, ErrandKind::kNone);
      EXPECT_EQ(s->group_guid, 42u);
      EXPECT_TRUE(s->group_policy.not_tank);
      EXPECT_FALSE(s->group_policy.stay);
    });
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_cleared();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_cleared();
  set();
  reg.ResetOnLogout(kBot);
  expect_cleared();
}

// Adversarial counter-test: combat_position_hold and TankSwapTracker claims
// clear on map change, harness disconnect, and logout so a healer never stays
// permanently unleashed and a tank never holds a stale swap claim across zones,
// while role policy (`not_tank`) still survives.
TEST(BotSessionRegistryTest,
     CombatPositionHoldAndTankSwapClearOnMapChangeDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1008;
  constexpr uint64_t kBoss = 0xF999;
  const auto set = [&] {
    reg.WithSession(kBot, [](BotSession& s) {
      s.group_guid = 42;
      s.group_policy.not_tank = true;
      s.group_policy.stay = true;
      s.combat_position_hold = true;
    });
    TankSwapTracker::Instance().RecordClaim({
        .target_guid = kBoss,
        .claiming_tank_guid = kBot,
        .group_guid = 42,
        .map_id = 631,
        .instance_id = 1,
        .claimed_at_ms = 5000,
    });
  };
  const auto expect_cleared = [&] {
    reg.InspectSession(kBot, [](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_FALSE(s->combat_position_hold);
      EXPECT_EQ(s->group_guid, 42u);
      EXPECT_TRUE(s->group_policy.not_tank);
      EXPECT_FALSE(s->group_policy.stay);
    });
    EXPECT_FALSE(
        TankSwapTracker::Instance().FindClaim(kBoss, 42, 631, 1, 5500));
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_cleared();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_cleared();
  set();
  reg.ResetOnLogout(kBot);
  expect_cleared();
}

// Adversarial counter-test: deferred auto-popup loot roll timestamps clear on
// map change, harness disconnect, and logout so stale roll timers never carry
// across zones or sessions, while `auto_popups` survives logout.
TEST(BotSessionRegistryTest,
     PendingLootRollTimersClearOnMapChangeDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x1009;
  const auto set = [&] {
    reg.WithSession(kBot, [](BotSession& s) {
      s.auto_popups = true;
      s.pending_rolls_seen = {PendingRollSeen{
          .item_guid = 0x55, .item_slot = 0, .first_seen_ms = 900}};
    });
  };
  const auto expect_cleared = [&] {
    reg.InspectSession(kBot, [](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_TRUE(s->pending_rolls_seen.empty());
      EXPECT_TRUE(s->auto_popups);
    });
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_cleared();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_cleared();
  set();
  reg.ResetOnLogout(kBot);
  expect_cleared();
}

// Adversarial counter-test: an active brain target pin never survives a map
// change, harness disconnect, or logout, while role policy (`not_tank`) still
// survives.
TEST(BotSessionRegistryTest,
     BrainTargetPinClearsOnMapChangeDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x100A;
  const auto set = [&] {
    reg.WithSession(kBot, [](BotSession& s) {
      s.group_guid = 42;
      s.group_policy.not_tank = true;
      PinBrainTarget(s.brain_target_pin, /*target_guid=*/0xB05E1,
                     /*in_combat=*/true);
    });
  };
  const auto expect_cleared = [&] {
    reg.InspectSession(kBot, [](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_FALSE(s->brain_target_pin.active);
      EXPECT_EQ(s->brain_target_pin.target_guid, 0u);
      EXPECT_FALSE(s->brain_target_pin.saw_combat);
      EXPECT_EQ(s->group_guid, 42u);
      EXPECT_TRUE(s->group_policy.not_tank);
    });
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_cleared();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_cleared();
  set();
  reg.ResetOnLogout(kBot);
  expect_cleared();
}

// Adversarial counter-test: human_brb_pause and leader_approvals clear on map
// change, harness disconnect, and logout so stale brb pauses or approvals never
// leak across zones or sessions, while `group_policy.not_tank` survives.
TEST(BotSessionRegistryTest,
     HumanBrbPauseAndLeaderApprovalsClearOnMapChangeDisconnectAndLogout) {
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x100B;
  const base::TimeTicks t0 = base::TimeTicks::Now();
  const auto set = [&] {
    reg.WithSession(kBot, [t0](BotSession& s) {
      s.group_guid = 42;
      s.group_policy.not_tank = true;
      RecordHumanBrbPause(s.human_brb_pause, 0x99, 36, 1, 1.0f, 2.0f, t0);
      RecordHumanLeaderApproval(s.leader_approvals, "queue rdf", t0);
      RecordHumanLeaderApproval(s.leader_approvals, "ready check", t0);
      RecordHumanLeaderApproval(s.leader_approvals, "invite Bob", t0);
    });
  };
  const auto expect_cleared = [&] {
    reg.InspectSession(kBot, [t0](const BotSession* s) {
      ASSERT_NE(s, nullptr);
      EXPECT_FALSE(s->human_brb_pause.paused);
      EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue,
                                             true, s->leader_approvals, t0));
      EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kReadyCheck,
                                             true, s->leader_approvals, t0));
      EXPECT_FALSE(CanBotLeaderPerformAction(BotLeaderActionKind::kInvite, true,
                                             s->leader_approvals, t0, "Bob"));
      EXPECT_EQ(s->group_guid, 42u);
      EXPECT_TRUE(s->group_policy.not_tank);
    });
  };
  set();
  reg.ResetOnMapChange(kBot);
  expect_cleared();
  set();
  reg.ResetOnDisconnect(kBot);
  expect_cleared();
  set();
  reg.ResetOnLogout(kBot);
  expect_cleared();
}

}  // namespace
}  // namespace llm_brain
