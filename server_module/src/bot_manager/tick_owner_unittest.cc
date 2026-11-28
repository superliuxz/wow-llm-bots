// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/tick_owner.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bot_session.h"
#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(TickOwnerTest, BrainLedOnlyWithAHarnessOutsideBattlegrounds) {
  EXPECT_EQ(TickOwnerFor(true, false), TickOwner::kBrain);
  EXPECT_EQ(TickOwnerFor(false, false), TickOwner::kPlayerbots);
  EXPECT_EQ(TickOwnerFor(true, true), TickOwner::kPlayerbots);
  EXPECT_EQ(TickOwnerFor(false, true), TickOwner::kPlayerbots);
  EXPECT_STREQ(TickOwnerName(TickOwner::kBrain), "brain-led");
  EXPECT_STREQ(TickOwnerName(TickOwner::kPlayerbots), "playerbots-led");
}

TEST(TickOwnerTest, UnhealthyLlmOrMissedPopupDeadlineHandsOffToPlayerbots) {
  // Healthy connected LLM outside BG keeps brain popup ownership.
  EXPECT_EQ(TickOwnerFor(/*harness_connected=*/true, /*in_battleground=*/false,
                         /*llm_healthy=*/true, /*popup_deadline_missed=*/false),
            TickOwner::kBrain);
  // Unhealthy LLM hands popups to playerbots even while harness is connected.
  EXPECT_EQ(TickOwnerFor(/*harness_connected=*/true, /*in_battleground=*/false,
                         /*llm_healthy=*/false,
                         /*popup_deadline_missed=*/false),
            TickOwner::kPlayerbots);
  // Missed popup deadline hands popups to playerbots even if health flag is
  // still true (e.g. wedged harness or slow queue).
  EXPECT_EQ(TickOwnerFor(/*harness_connected=*/true, /*in_battleground=*/false,
                         /*llm_healthy=*/true, /*popup_deadline_missed=*/true),
            TickOwner::kPlayerbots);
  // Both unhealthy and missed deadline -> playerbots.
  EXPECT_EQ(TickOwnerFor(/*harness_connected=*/true, /*in_battleground=*/false,
                         /*llm_healthy=*/false, /*popup_deadline_missed=*/true),
            TickOwner::kPlayerbots);
  // Adversarial counter-tests: disconnected or in battleground stays
  // playerbots-led even when LLM is healthy and no deadline was missed.
  EXPECT_EQ(TickOwnerFor(/*harness_connected=*/false, /*in_battleground=*/false,
                         /*llm_healthy=*/true, /*popup_deadline_missed=*/false),
            TickOwner::kPlayerbots);
  EXPECT_EQ(TickOwnerFor(/*harness_connected=*/true, /*in_battleground=*/true,
                         /*llm_healthy=*/true, /*popup_deadline_missed=*/false),
            TickOwner::kPlayerbots);
}

TEST(TickOwnerTest, PopupsAreTheBrainsOnlyWhileItLeads) {
  for (std::string_view name :
       {"accept invitation", "loot roll", "roll", "ready check",
        "accept quest share", "self resurrect", "lfg join", "lfg accept",
        "lfg role check", "lfg leave", "lfg teleport", "accept trade",
        "trade status extended"}) {
    const GatedAction* a = FindGatedAction(name);
    ASSERT_NE(a, nullptr) << name;
    EXPECT_TRUE(BlockedFor(*a, TickOwner::kBrain)) << name;
    // Counter-test (A-M9): with no harness, playerbots answers them so a
    // human's group never waits on a bot.
    EXPECT_FALSE(BlockedFor(*a, TickOwner::kPlayerbots)) << name;
  }
}

TEST(TickOwnerTest, CorpseRunActionsAreGatedOnlyWhileBrainLed) {
  for (std::string_view name :
       {"find corpse", "revive from corpse", "spirit healer", "repop"}) {
    const GatedAction* a = FindGatedAction(name);
    ASSERT_NE(a, nullptr) << name;
    EXPECT_EQ(a->gate, ActionGate::kBrainLed) << name;
    // Blocked while brain-led so playerbots cannot cheat-teleport or revive at
    // a spirit healer without rez sickness.
    EXPECT_TRUE(BlockedFor(*a, TickOwner::kBrain)) << name;
    // Adversarial counter-test: harness disconnect falls back cleanly to
    // playerbots.
    EXPECT_FALSE(BlockedFor(*a, TickOwner::kPlayerbots)) << name;
  }
}

TEST(TickOwnerTest, AutoEquipIsBlockedInBothModes) {
  const GatedAction* a = FindGatedAction("equip upgrade");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->gate, ActionGate::kAlways);
  EXPECT_TRUE(BlockedFor(*a, TickOwner::kBrain));
  EXPECT_TRUE(BlockedFor(*a, TickOwner::kPlayerbots));

  // `equip upgrades packet action` is conditional so BrainPolicyListener can
  // reload hunter ammo via SetAmmo when `no ammo` triggers while still
  // blocking playerbots from overwriting brain-chosen gear equips.
  const GatedAction* pkt = FindGatedAction("equip upgrades packet action");
  ASSERT_NE(pkt, nullptr);
  EXPECT_EQ(pkt->gate, ActionGate::kCondition);
}

TEST(TickOwnerTest, ConditionalAndUngatedActionsAreNotBlockedByMode) {
  for (std::string_view name :
       {"auto release", "equip upgrades packet action", "flee", "stay",
        "return to stay position", "apply stone", "apply oil",
        "toggle pet spell"}) {
    const GatedAction* a = FindGatedAction(name);
    ASSERT_NE(a, nullptr) << name;
    EXPECT_EQ(a->gate, ActionGate::kCondition) << name;
    EXPECT_FALSE(BlockedFor(*a, TickOwner::kBrain)) << name;
    EXPECT_FALSE(BlockedFor(*a, TickOwner::kPlayerbots)) << name;
  }
  // Combat, healing, following and looting stay playerbots' in both modes.
  for (std::string_view name :
       {"attack", "follow", "loot", "heal", "", "accept invitation ", "Roll",
        // "trade status" is playerbots' trigger name, not an action's; the
        // master's `t <item>` chat command stays playerbots'.
        "trade status", "trade"}) {
    EXPECT_EQ(FindGatedAction(name), nullptr) << "'" << name << "'";
  }
}

TEST(TickOwnerTest, TableNamesAreUnique) {
  const auto gated = GatedActions();
  for (size_t i = 0; i < gated.size(); ++i) {
    for (size_t j = i + 1; j < gated.size(); ++j) {
      EXPECT_NE(gated[i].name, gated[j].name);
    }
    EXPECT_EQ(FindGatedAction(gated[i].name), &gated[i]);
  }
}

TEST(TickOwnerTest, MissingActionNamesListsRenamedOnesInTableOrder) {
  const std::vector<GatedAction> gated = {
      {"accept invitation", ActionGate::kBrainLed},
      {"loot roll", ActionGate::kBrainLed},
      {"flee", ActionGate::kCondition},
  };
  EXPECT_TRUE(MissingActionNames(
                  gated, std::vector<std::string>{"flee", "attack", "loot roll",
                                                  "accept invitation"})
                  .empty());
  EXPECT_EQ(MissingActionNames(gated, std::vector<std::string>{"loot roll"}),
            (std::vector<std::string>{"accept invitation", "flee"}));
  EXPECT_EQ(
      MissingActionNames(gated, {}),
      (std::vector<std::string>{"accept invitation", "loot roll", "flee"}));
  EXPECT_TRUE(MissingActionNames({}, {}).empty());
}

// A fresh login, a logout and the registry's shutdown clear leave a bot
// playerbots-led until the module sees a harness again; a harness
// disconnect or map change doesn't touch the mode (BotManager::Update sets
// it from the connection every world tick).
TEST(TickOwnerTest, SessionModeResets) {
  BotSessionRegistry& reg = BotSessionRegistry::Instance();
  reg.ClearAll();
  constexpr uint64_t kBot = 0x77;
  auto owner = [&] {
    return reg.InspectSession(kBot, [](const BotSession* s) {
      return s ? s->tick_owner : TickOwner::kPlayerbots;
    });
  };
  EXPECT_EQ(owner(), TickOwner::kPlayerbots);
  reg.WithSession(kBot, [](BotSession& s) {
    s.tick_owner = TickOwner::kBrain;
    s.popup_open_since_ms = 12345;
  });
  reg.ResetOnMapChange(kBot);
  EXPECT_EQ(owner(), TickOwner::kBrain);
  EXPECT_FALSE(reg.InspectSession(kBot, [](const BotSession* s) {
    return s && s->popup_open_since_ms.has_value();
  }));
  reg.WithSession(kBot, [](BotSession& s) { s.popup_open_since_ms = 23456; });
  reg.ResetOnDisconnect(kBot);
  EXPECT_EQ(owner(), TickOwner::kBrain);
  EXPECT_FALSE(reg.InspectSession(kBot, [](const BotSession* s) {
    return s && s->popup_open_since_ms.has_value();
  }));
  reg.WithSession(kBot, [](BotSession& s) { s.popup_open_since_ms = 34567; });
  reg.ResetOnLogout(kBot);
  EXPECT_EQ(owner(), TickOwner::kPlayerbots);
  EXPECT_FALSE(reg.InspectSession(kBot, [](const BotSession* s) {
    return s && s->popup_open_since_ms.has_value();
  }));
  // A relog that keeps its group keeps the group's state, not the mode.
  reg.WithSession(kBot, [](BotSession& s) {
    s.tick_owner = TickOwner::kBrain;
    s.group_guid = 5;
  });
  reg.ResetOnLogout(kBot);
  EXPECT_EQ(owner(), TickOwner::kPlayerbots);
  reg.WithSession(kBot,
                  [](BotSession& s) { s.tick_owner = TickOwner::kBrain; });
  reg.ClearAll();
  EXPECT_EQ(owner(), TickOwner::kPlayerbots);
}

TEST(TickOwnerTest, HealerCombatStrategyDeltaRestoresHealingInGroup) {
  // Solo healers get DPS extras; grouped healers strip them AND restore the
  // exclusive primary healing strategy (e.g. `+holy heal`, `+disc`, `+resto`,
  // `+holy`).
  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kPriestHoly, false),
            ",+holy dps,+shadow debuff,+shadow aoe");
  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kPriestHoly, true),
            ",-holy dps,-shadow debuff,-shadow aoe,+holy heal");

  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kPriestDiscipline, false),
            ",+holy dps,+shadow debuff,+shadow aoe");
  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kPriestDiscipline, true),
            ",-holy dps,-shadow debuff,-shadow aoe,+disc");

  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kDruidRestoration, false),
            ",+aoe");
  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kDruidRestoration, true),
            ",-aoe,+resto");

  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kShamanRestoration, false),
            ",+ele,+caster aoe");
  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kShamanRestoration, true),
            ",-ele,-caster aoe,+resto");

  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kPaladinHoly, false),
            ",+ret,+baoe");
  EXPECT_EQ(HealerCombatStrategyDelta(HealerSpec::kPaladinHoly, true),
            ",-ret,-baoe,+holy");

  // Adversarial counter-test: non-healer specs get no strategy delta solo or
  // grouped.
  EXPECT_TRUE(HealerCombatStrategyDelta(HealerSpec::kNone, false).empty());
  EXPECT_TRUE(HealerCombatStrategyDelta(HealerSpec::kNone, true).empty());
}

}  // namespace
}  // namespace llm_brain
