// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "triggers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "plan.h"
#include "scheduler.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

pb::Snapshot BaseSnap(uint32_t hp = 100, uint32_t max_hp = 100) {
  pb::Snapshot snap;
  pb::Self& self = snap.base.self;
  self.state = pb::Self::ALIVE;
  self.health = hp;
  self.health_max = max_hp;
  self.power_type = pb::Self::MANA;
  self.power = 100;
  self.power_max = 100;
  return snap;
}

pb::Unit* AddNearbyUnit(pb::Snapshot* snap,
                        const std::string& guid,
                        const std::string& name,
                        pb::Unit::Kind kind,
                        pb::Unit::Reaction reaction,
                        float dist_yd,
                        bool targeting_me = false) {
  pb::Unit& u = snap->base.nearby.units.emplace_back();
  u.guid = guid;
  u.name = name;
  u.kind = kind;
  u.reaction = reaction;
  u.rel.dist_yd = dist_yd;
  u.targeting_me = targeting_me;
  return &u;
}

TEST(TriggersTest, ExpectedFightDoesNotWakeButUnexpectedAddDoes) {
  TriggerEvaluator eval;
  eval.SetPlanContext({}, {"Mottled Boar"});
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap()));

  // Entering combat with an expected Mottled Boar does not wake the brain.
  pb::Snapshot fighting_boar = BaseSnap(90, 100);
  fighting_boar.base.self.in_combat = true;
  AddNearbyUnit(&fighting_boar, "0x1", "Mottled Boar", pb::Unit::NPC,
                pb::Unit::HOSTILE, 4.0f, /*targeting_me=*/true);
  EXPECT_FALSE(eval.EvaluateSnapshot(fighting_boar));

  // An unexpected Scorpid Worker joins the fight -> urgent wake once.
  pb::Snapshot add_joined = fighting_boar;
  AddNearbyUnit(&add_joined, "0x2", "Scorpid Worker", pb::Unit::NPC,
                pb::Unit::HOSTILE, 5.0f, /*targeting_me=*/true);
  const auto wake = eval.EvaluateSnapshot(add_joined);
  ASSERT_TRUE(wake);
  EXPECT_EQ(wake->urgency, Urgency::kUrgent);
  EXPECT_EQ(wake->reason, "attacked by Scorpid Worker");

  // Same two attackers on the next tick -> does not re-fire continuously.
  EXPECT_FALSE(eval.EvaluateSnapshot(add_joined));
}

TEST(TriggersTest, EnteringCombatWithoutExpectationsWakesUrgently) {
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap()));

  pb::Snapshot combat = BaseSnap();
  combat.base.self.in_combat = true;
  const auto wake = eval.EvaluateSnapshot(combat);
  ASSERT_TRUE(wake);
  EXPECT_EQ(wake->urgency, Urgency::kUrgent);
  EXPECT_EQ(wake->reason, "entered combat");
}

TEST(TriggersTest, HealthAndManaWatchesFireOncePerCrossingAndRearm) {
  TriggerEvaluator eval;
  eval.SetPlanContext(
      {{Watch::Kind::kHealthBelow, 40}, {Watch::Kind::kManaBelow, 25}}, {});
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(80, 100)));

  // Drop below 40% HP: fires once.
  EXPECT_EQ(eval.EvaluateSnapshot(BaseSnap(35, 100)),
            (std::optional<WakeTrigger>{{Urgency::kUrgent, "hp<40"}}));
  // Still below 40% HP: stays latched.
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(20, 100)));
  // Heal above 40%, then drop below again: rearms and fires again.
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(60, 100)));
  EXPECT_EQ(eval.EvaluateSnapshot(BaseSnap(39, 100)),
            (std::optional<WakeTrigger>{{Urgency::kUrgent, "hp<40"}}));

  // Mana watch out of combat uses kNormal urgency.
  pb::Snapshot low_mana = BaseSnap(80, 100);
  low_mana.base.self.power = 20;
  EXPECT_EQ(eval.EvaluateSnapshot(low_mana),
            (std::optional<WakeTrigger>{{Urgency::kNormal, "mana<25"}}));
}

TEST(TriggersTest, EnemyPlayerWatchFiresWhenHostilePlayerEntersRange) {
  TriggerEvaluator eval;
  eval.SetPlanContext({{Watch::Kind::kEnemyPlayerWithin, 40}}, {});

  pb::Snapshot far = BaseSnap();
  AddNearbyUnit(&far, "0x99", "Ganker", pb::Unit::PLAYER, pb::Unit::HOSTILE,
                55.0f);
  EXPECT_FALSE(eval.EvaluateSnapshot(far));

  pb::Snapshot close = BaseSnap();
  AddNearbyUnit(&close, "0x99", "Ganker", pb::Unit::PLAYER, pb::Unit::HOSTILE,
                32.0f);
  EXPECT_EQ(eval.EvaluateSnapshot(close),
            (std::optional<WakeTrigger>{
                {Urgency::kP1Danger, "enemy player Ganker within 40 yd"}}));
  // Staying in range does not spam wakes.
  EXPECT_FALSE(eval.EvaluateSnapshot(close));
}

// A watch already true when its plan is installed was on the screen the brain
// just read: it must not fire again on every re-install (a wake loop).
TEST(TriggersTest, WatchAlreadyTrueAtInstallDoesNotFireUntilItCrossesAgain) {
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(33, 100)));

  for (int plan = 0; plan < 3; ++plan) {
    eval.SetPlanContext({{Watch::Kind::kHealthBelow, 40}}, {});
    EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(33, 100))) << plan;
  }

  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(60, 100)));
  EXPECT_EQ(eval.EvaluateSnapshot(BaseSnap(35, 100)),
            (std::optional<WakeTrigger>{{Urgency::kUrgent, "hp<40"}}));
}

TEST(TriggersTest, EnemyPlayerAlreadyInRangeAtInstallDoesNotFire) {
  TriggerEvaluator eval;
  pb::Snapshot close = BaseSnap();
  AddNearbyUnit(&close, "0x99", "Ganker", pb::Unit::PLAYER, pb::Unit::HOSTILE,
                20.0f);
  EXPECT_FALSE(eval.EvaluateSnapshot(close));

  eval.SetPlanContext({{Watch::Kind::kEnemyPlayerWithin, 40}}, {});
  EXPECT_FALSE(eval.EvaluateSnapshot(close));

  // A second player entering range still fires.
  pb::Snapshot two = close;
  AddNearbyUnit(&two, "0x98", "Other", pb::Unit::PLAYER, pb::Unit::HOSTILE,
                30.0f);
  EXPECT_EQ(eval.EvaluateSnapshot(two),
            (std::optional<WakeTrigger>{
                {Urgency::kP1Danger, "enemy player Other within 40 yd"}}));
}

TEST(TriggersTest, SetExpectedNamesKeepsWatchLatches) {
  TriggerEvaluator eval;
  eval.SetPlanContext({{Watch::Kind::kHealthBelow, 40}}, {});
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(80, 100)));
  EXPECT_TRUE(eval.EvaluateSnapshot(BaseSnap(30, 100)));

  eval.SetExpectedNames({"Mottled Boar"});
  EXPECT_EQ(eval.watches().size(), 1u);
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(30, 100)));
  EXPECT_EQ(eval.expected_names().count("Mottled Boar"), 1u);
}

TEST(TriggersTest, FriendlyUnitTargetingMeIsNotAnAttacker) {
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap()));

  pb::Snapshot targeted = BaseSnap();
  AddNearbyUnit(&targeted, "0x7", "Gatda", pb::Unit::PLAYER, pb::Unit::FRIENDLY,
                3.0f, /*targeting_me=*/true);
  EXPECT_FALSE(eval.EvaluateSnapshot(targeted));
}

TEST(TriggersTest, ComingBackToLifeWakes) {
  TriggerEvaluator eval;
  pb::Snapshot dead = BaseSnap();
  dead.base.self.state = pb::Self::DEAD_BODY;
  EXPECT_FALSE(eval.EvaluateSnapshot(dead));
  EXPECT_FALSE(eval.EvaluateSnapshot(dead));
  EXPECT_EQ(eval.EvaluateSnapshot(BaseSnap()),
            (std::optional<WakeTrigger>{{Urgency::kNormal, "alive again"}}));
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap()));
}

TEST(TriggersTest, LossOfControlAndDeathWakeUrgently) {
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap()));

  pb::Snapshot stunned = BaseSnap();
  stunned.base.self.control.stunned = true;
  EXPECT_EQ(eval.EvaluateSnapshot(stunned),
            (std::optional<WakeTrigger>{
                {Urgency::kUrgent, "lost control (stunned)"}}));

  pb::Snapshot dead = BaseSnap(0, 100);
  dead.base.self.state = pb::Self::DEAD_BODY;
  EXPECT_EQ(eval.EvaluateSnapshot(dead),
            (std::optional<WakeTrigger>{{Urgency::kUrgent, "died"}}));
}

TEST(TriggersTest, EventsAlertOnlyOnClientAttentionEvents) {
  pb::Event whisper;
  pb::ChatEvent& wchat = whisper.chat.emplace();
  wchat.channel = pb::WHISPER;
  wchat.from = "Tess";
  wchat.text = "want to group?";
  EXPECT_EQ(
      EvaluateEventTrigger(whisper),
      (std::optional<WakeTrigger>{{Urgency::kP3Chat, "whisper from Tess"}}));

  pb::Event invite;
  pb::PromptEvent& prompt = invite.prompt.emplace();
  prompt.kind = pb::PromptEvent::GROUP_INVITE;
  prompt.from = "Tess";
  EXPECT_EQ(EvaluateEventTrigger(invite),
            (std::optional<WakeTrigger>{
                {Urgency::kP1Danger, "prompt: group invite from Tess"}}));

  pb::Event cmd_err;
  pb::ErrorEvent& err = cmd_err.error.emplace();
  err.command_id = 7;
  err.text = "nothing to loot on Mottled Boar";
  EXPECT_EQ(
      EvaluateEventTrigger(cmd_err),
      (std::optional<WakeTrigger>{
          {Urgency::kNormal, "step failed: nothing to loot on Mottled Boar"}}));

  // Routine loot and party chat are noted, not woken on.
  pb::Event loot;
  pb::Item& item = loot.item_received.emplace().item.emplace();
  item.name = "Tough Jerky";
  item.count = 2;
  EXPECT_FALSE(EvaluateEventTrigger(loot));
  EXPECT_EQ(DescribeEvent(loot), "received 2x Tough Jerky");

  pb::Event party_chat;
  pb::ChatEvent& pchat = party_chat.chat.emplace();
  pchat.channel = pb::PARTY;
  pchat.from = "Gorn";
  pchat.text = "pulling";
  EXPECT_FALSE(EvaluateEventTrigger(party_chat));
}

}  // namespace
}  // namespace harness
