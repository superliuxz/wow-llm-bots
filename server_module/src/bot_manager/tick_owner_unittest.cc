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

TEST(TickOwnerTest, AutoEquipIsBlockedInBothModes) {
  for (std::string_view name :
       {"equip upgrade", "equip upgrades packet action"}) {
    const GatedAction* a = FindGatedAction(name);
    ASSERT_NE(a, nullptr) << name;
    EXPECT_TRUE(BlockedFor(*a, TickOwner::kBrain)) << name;
    EXPECT_TRUE(BlockedFor(*a, TickOwner::kPlayerbots)) << name;
  }
}

TEST(TickOwnerTest, ConditionalAndUngatedActionsAreNotBlockedByMode) {
  for (std::string_view name :
       {"auto release", "flee", "stay", "return to stay position",
        "apply stone", "apply oil"}) {
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
  reg.WithSession(kBot,
                  [](BotSession& s) { s.tick_owner = TickOwner::kBrain; });
  reg.ResetOnMapChange(kBot);
  EXPECT_EQ(owner(), TickOwner::kBrain);
  reg.ResetOnDisconnect(kBot);
  EXPECT_EQ(owner(), TickOwner::kBrain);
  reg.ResetOnLogout(kBot);
  EXPECT_EQ(owner(), TickOwner::kPlayerbots);
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
