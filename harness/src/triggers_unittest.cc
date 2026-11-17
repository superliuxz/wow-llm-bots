// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "triggers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "plan.h"
#include "proto/snapshot.pb.h"
#include "scheduler.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

pb::Snapshot BaseSnap(uint32_t hp = 100, uint32_t max_hp = 100) {
  pb::Snapshot snap;
  pb::Self* self = snap.mutable_base()->mutable_self();
  self->set_state(pb::Self::ALIVE);
  self->set_health(hp);
  self->set_health_max(max_hp);
  self->set_power_type(pb::Self::MANA);
  self->set_power(100);
  self->set_power_max(100);
  return snap;
}

pb::Unit* AddNearbyUnit(pb::Snapshot* snap,
                        const std::string& guid,
                        const std::string& name,
                        pb::Unit::Kind kind,
                        pb::Unit::Reaction reaction,
                        float dist_yd,
                        bool targeting_me = false) {
  pb::Unit* u = snap->mutable_base()->mutable_nearby()->add_units();
  u->set_guid(guid);
  u->set_name(name);
  u->set_kind(kind);
  u->set_reaction(reaction);
  u->mutable_rel()->set_dist_yd(dist_yd);
  u->set_targeting_me(targeting_me);
  return u;
}

TEST(TriggersTest, ExpectedFightDoesNotWakeButUnexpectedAddDoes) {
  TriggerEvaluator eval;
  eval.SetPlanContext({}, {"Mottled Boar"});
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap()));

  // Entering combat with an expected Mottled Boar does not wake the brain.
  pb::Snapshot fighting_boar = BaseSnap(90, 100);
  fighting_boar.mutable_base()->mutable_self()->set_in_combat(true);
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
  combat.mutable_base()->mutable_self()->set_in_combat(true);
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
  low_mana.mutable_base()->mutable_self()->set_power(20);
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
                {Urgency::kUrgent, "enemy player Ganker within 40 yd"}}));
  // Staying in range does not spam wakes.
  EXPECT_FALSE(eval.EvaluateSnapshot(close));
}

TEST(TriggersTest, LossOfControlAndDeathWakeUrgently) {
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap()));

  pb::Snapshot stunned = BaseSnap();
  stunned.mutable_base()->mutable_self()->mutable_control()->set_stunned(true);
  EXPECT_EQ(eval.EvaluateSnapshot(stunned),
            (std::optional<WakeTrigger>{
                {Urgency::kUrgent, "lost control (stunned)"}}));

  pb::Snapshot dead = BaseSnap(0, 100);
  dead.mutable_base()->mutable_self()->set_state(pb::Self::DEAD_BODY);
  EXPECT_EQ(eval.EvaluateSnapshot(dead),
            (std::optional<WakeTrigger>{{Urgency::kUrgent, "died"}}));
}

TEST(TriggersTest, EventsAlertOnlyOnClientAttentionEvents) {
  pb::Event whisper;
  whisper.mutable_chat()->set_channel(pb::WHISPER);
  whisper.mutable_chat()->set_from("Tess");
  whisper.mutable_chat()->set_text("want to group?");
  EXPECT_EQ(
      EvaluateEventTrigger(whisper),
      (std::optional<WakeTrigger>{{Urgency::kNormal, "whisper from Tess"}}));

  pb::Event invite;
  invite.mutable_prompt()->set_kind(pb::PromptEvent::GROUP_INVITE);
  invite.mutable_prompt()->set_from("Tess");
  EXPECT_EQ(EvaluateEventTrigger(invite),
            (std::optional<WakeTrigger>{
                {Urgency::kUrgent, "prompt: group invite from Tess"}}));

  pb::Event cmd_err;
  cmd_err.mutable_error()->set_command_id(7);
  cmd_err.mutable_error()->set_text("nothing to loot on Mottled Boar");
  EXPECT_EQ(
      EvaluateEventTrigger(cmd_err),
      (std::optional<WakeTrigger>{
          {Urgency::kNormal, "step failed: nothing to loot on Mottled Boar"}}));

  // Routine loot and party chat are noted, not woken on.
  pb::Event loot;
  loot.mutable_item_received()->mutable_item()->set_name("Tough Jerky");
  loot.mutable_item_received()->mutable_item()->set_count(2);
  EXPECT_FALSE(EvaluateEventTrigger(loot));
  EXPECT_EQ(DescribeEvent(loot), "received 2x Tough Jerky");

  pb::Event party_chat;
  party_chat.mutable_chat()->set_channel(pb::PARTY);
  party_chat.mutable_chat()->set_from("Gorn");
  party_chat.mutable_chat()->set_text("pulling");
  EXPECT_FALSE(EvaluateEventTrigger(party_chat));
}

}  // namespace
}  // namespace harness
