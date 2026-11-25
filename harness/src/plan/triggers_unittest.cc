// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/triggers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "llm/scheduler.h"
#include "plan/plan.h"
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

// The brain was asked at 60% hp; hp fell below its watch while the call was in
// flight: the crossing fires once the plan is installed (review of f40b0b6,
// finding 4: latches seeded from the newest snapshot swallowed it).
TEST(TriggersTest, WatchCrossedDuringTheCallFires) {
  TriggerEvaluator eval;
  const pb::Snapshot asked = BaseSnap(60, 100);
  EXPECT_FALSE(eval.EvaluateSnapshot(asked));
  const TriggerEvaluator::WatchInputs seen = TriggerEvaluator::InputsOf(asked);
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(35, 100)));  // During the call.
  eval.SetPlanContext({{Watch::Kind::kHealthBelow, 40}}, {}, &seen);
  EXPECT_EQ(eval.EvaluateSnapshot(BaseSnap(34, 100)),
            (std::optional<WakeTrigger>{{Urgency::kUrgent, "hp<40"}}));
  EXPECT_FALSE(eval.EvaluateSnapshot(BaseSnap(33, 100)));  // Once.
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
      (std::optional<WakeTrigger>{{Urgency::kP1Danger, "whisper from Tess"}}));

  // Party `at_me` outranks routine P2 wakes (A-M7); ambient Say `at_me` stays
  // P3 (protected by scheduler aging).
  pb::Event party_at_me;
  pb::ChatEvent& p_at = party_at_me.chat.emplace();
  p_at.channel = pb::PARTY;
  p_at.from = "Hele";
  p_at.text = "pull?";
  p_at.at_me = true;
  EXPECT_EQ(EvaluateEventTrigger(party_at_me),
            (std::optional<WakeTrigger>{
                {Urgency::kP1Danger, "party from Hele: pull?"}}));

  pb::Event say_at_me;
  pb::ChatEvent& s_at = say_at_me.chat.emplace();
  s_at.channel = pb::SAY;
  s_at.from = "Stranger";
  s_at.text = "hey";
  s_at.at_me = true;
  EXPECT_EQ(EvaluateEventTrigger(say_at_me),
            (std::optional<WakeTrigger>{
                {Urgency::kP3Chat, "say from Stranger: hey"}}));

  // A boss emote is a mechanic (P0); a raid warning reaches every bot of the
  // raid at once (P1, harness #20: it must not cancel every in-flight call).
  pb::Event emote;
  pb::ChatEvent& e = emote.chat.emplace();
  e.channel = pb::BOSS_EMOTE;
  e.from = "Loken";
  e.text = "begins to cast Lightning Nova!";
  EXPECT_EQ(EvaluateEventTrigger(emote)->urgency, Urgency::kP0Combat);
  pb::Event warning;
  pb::ChatEvent& rw = warning.chat.emplace();
  rw.channel = pb::RAID_WARNING;
  rw.from = "Hele";
  rw.text = "stack on skull";
  rw.at_me = true;
  EXPECT_EQ(EvaluateEventTrigger(warning)->urgency, Urgency::kP1Danger);

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

TEST(TriggersTest, PartyStateWakesTheBot) {
  const auto member = [](pb::Snapshot* snap, const std::string& name,
                         pb::Role role) -> pb::Unit& {
    if (!snap->ext.party) {
      snap->ext.party.emplace();
    }
    pb::Unit& m = snap->ext.party->members.emplace_back();
    m.name = name;
    m.power_pct = 100.0f;
    pb::GroupPart& gp = m.group.emplace();
    gp.online = true;
    gp.in_range = true;
    gp.role = role;
    return m;
  };
  TriggerEvaluator eval;
  pb::Snapshot calm = BaseSnap();
  member(&calm, "Hele", pb::TANK);
  member(&calm, "Zenji", pb::HEALER);
  EXPECT_FALSE(eval.EvaluateSnapshot(calm));
  EXPECT_FALSE(eval.EvaluateSnapshot(calm));

  // The tank pulls: the party is fighting while this bot isn't yet.
  pb::Snapshot pulled = calm;
  pulled.ext.party->members[0].group->aggro = true;
  std::optional<WakeTrigger> t = eval.EvaluateSnapshot(pulled);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP1Danger);
  EXPECT_EQ(t->reason, "party in combat (Hele attacked)");
  EXPECT_FALSE(eval.EvaluateSnapshot(pulled));  // Once per pull.

  // The healer runs low on mana.
  pb::Snapshot oom = pulled;
  oom.ext.party->members[1].power_pct = 20.0f;
  t = eval.EvaluateSnapshot(oom);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "healer Zenji low on mana");
  EXPECT_FALSE(eval.EvaluateSnapshot(oom));

  // The tank dies.
  pb::Snapshot died = oom;
  died.ext.party->members[0].dead = true;
  t = eval.EvaluateSnapshot(died);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "party member Hele died");
  EXPECT_FALSE(eval.EvaluateSnapshot(died));

  // The healer logs out, then leaves the group (T11).
  pb::Snapshot offline = died;
  offline.ext.party->members[1].group->online = false;
  t = eval.EvaluateSnapshot(offline);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "party member Zenji went offline");
  pb::Snapshot back = died;
  EXPECT_TRUE(eval.EvaluateSnapshot(back));  // Healer oom again, on return.
  pb::Snapshot left = died;
  left.ext.party->members.pop_back();
  t = eval.EvaluateSnapshot(left);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "party member Zenji left the group");
}

TEST(TriggersTest, PartyMemberNearDeathInAFightWakesOncePerDrop) {
  pb::Snapshot snap = BaseSnap();
  snap.base.self.in_combat = true;
  pb::Unit& m = snap.ext.party.emplace().members.emplace_back();
  m.name = "Hele";
  m.health_pct = 80.0f;
  pb::GroupPart& gp = m.group.emplace();
  gp.online = true;
  gp.in_range = true;
  gp.role = pb::TANK;
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(snap));

  const auto at = [&snap](float hp) {
    pb::Snapshot s = snap;
    s.ext.party->members[0].health_pct = hp;
    return s;
  };
  std::optional<WakeTrigger> t = eval.EvaluateSnapshot(at(30.0f));
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP1Danger);
  EXPECT_EQ(t->reason, "party member Hele is near death");
  EXPECT_TRUE(t->kinds & kWakeParty);
  // Latched while low, and through a small heal that doesn't clear 50%.
  EXPECT_FALSE(eval.EvaluateSnapshot(at(20.0f)));
  EXPECT_FALSE(eval.EvaluateSnapshot(at(40.0f)));
  EXPECT_FALSE(eval.EvaluateSnapshot(at(30.0f)));
  // Healed back up, then low again: a new wake.
  EXPECT_FALSE(eval.EvaluateSnapshot(at(60.0f)));
  EXPECT_TRUE(eval.EvaluateSnapshot(at(30.0f)));
}

TEST(TriggersTest, PartyMemberLowOutsideAFightDoesNotWake) {
  // Counter-tests: low health with nobody fighting (resting after a pull),
  // out of range, dead, or the bot's own health (its `hp<N` watch's job)
  // wakes nothing as "near death".
  pb::Snapshot snap = BaseSnap(/*hp=*/10);
  pb::Unit& m = snap.ext.party.emplace().members.emplace_back();
  m.name = "Hele";
  m.health_pct = 80.0f;
  pb::GroupPart& gp = m.group.emplace();
  gp.online = true;
  gp.in_range = true;
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(snap));
  snap.ext.party->members[0].health_pct = 20.0f;
  EXPECT_FALSE(eval.EvaluateSnapshot(snap));  // Out of combat, no aggro.

  TriggerEvaluator eval_far;
  pb::Snapshot far = snap;
  far.base.self.in_combat = true;
  far.ext.party->members[0].group->in_range = false;
  far.ext.party->members[0].health_pct = 80.0f;
  EXPECT_FALSE(eval_far.EvaluateSnapshot(far));
  far.ext.party->members[0].health_pct = 20.0f;
  EXPECT_FALSE(eval_far.EvaluateSnapshot(far));

  // The member's own fight counts while the bot isn't in one yet.
  pb::Snapshot hit = snap;
  hit.ext.party->members[0].group->aggro = true;
  std::optional<WakeTrigger> t = eval.EvaluateSnapshot(hit);
  ASSERT_TRUE(t);
  EXPECT_NE(t->reason.find("party member Hele is near death"),
            std::string::npos);

  TriggerEvaluator eval2;
  pb::Snapshot alive = snap;
  alive.base.self.in_combat = true;
  alive.ext.party->members[0].health_pct = 80.0f;
  EXPECT_FALSE(eval2.EvaluateSnapshot(alive));
  pb::Snapshot dead = alive;
  dead.ext.party->members[0].dead = true;
  dead.ext.party->members[0].health_pct = 0.0f;
  t = eval2.EvaluateSnapshot(dead);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "party member Hele died");  // Not "near death" too.
}

TEST(TriggersTest, NearDeathDoesNotOutrankCombatWakes) {
  // P0 combat priority: a new attacker in the same snapshot keeps P0 urgency
  // (kUrgent) and the near-death reason rides along.
  pb::Snapshot snap = BaseSnap();
  pb::Unit& m = snap.ext.party.emplace().members.emplace_back();
  m.name = "Hele";
  m.health_pct = 80.0f;
  pb::GroupPart& gp = m.group.emplace();
  gp.online = true;
  gp.in_range = true;
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(snap));
  pb::Snapshot hit = snap;
  hit.base.self.in_combat = true;
  hit.ext.party->members[0].health_pct = 20.0f;
  AddNearbyUnit(&hit, "0x1", "Kobold", pb::Unit::NPC, pb::Unit::HOSTILE, 3.0f,
                /*targeting_me=*/true);
  std::optional<WakeTrigger> t = eval.EvaluateSnapshot(hit);
  ASSERT_TRUE(t);
  EXPECT_LT(t->urgency, Urgency::kP1Danger);
  EXPECT_NE(t->reason.find("near death"), std::string::npos);
}

}  // namespace
}  // namespace harness
