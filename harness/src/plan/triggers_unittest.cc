// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/triggers.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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

  pb::Event raid_at_me;
  pb::ChatEvent& r_at = raid_at_me.chat.emplace();
  r_at.channel = pb::RAID;
  r_at.from = "Hele";
  r_at.text = "healers drink";
  r_at.at_me = true;
  EXPECT_EQ(EvaluateEventTrigger(raid_at_me),
            (std::optional<WakeTrigger>{
                {Urgency::kP1Danger, "raid from Hele: healers drink"}}));

  pb::Event raid_silent;
  pb::ChatEvent& r_silent = raid_silent.chat.emplace();
  r_silent.channel = pb::RAID;
  r_silent.from = "Gorn";
  r_silent.text = "healers drink";
  r_silent.at_me = false;
  EXPECT_FALSE(EvaluateEventTrigger(raid_silent));

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

// 4e: a bot that tanks wakes when a mob goes for another, non-tank member
// (healing threat, a sheep opener, a patrol), once per mob; never for a
// crowd-controlled mob, a mob on another tank, or when it doesn't tank.
TEST(TriggersTest, LooseMobOnAMemberWakesTheTankOnce) {
  pb::Snapshot snap = BaseSnap();
  snap.bot = "Tanky";
  snap.base.self.in_combat = true;
  snap.base.self.unit.name = "Tanky";
  snap.base.self.unit.group.emplace().role = pb::TANK;
  pb::Party& party = snap.ext.party.emplace();
  for (const auto& [name, role] :
       {std::pair{"Hele", pb::HEALER}, std::pair{"Offtank", pb::TANK}}) {
    pb::Unit& m = party.members.emplace_back();
    m.name = name;
    m.health_pct = 100.0f;
    m.group.emplace().online = true;
    m.group->in_range = true;
    m.group->role = role;
  }
  pb::Unit* mine = AddNearbyUnit(&snap, "0x1", "Trogg", pb::Unit::NPC,
                                 pb::Unit::HOSTILE, 3.0f, true);
  mine->target = "Tanky";
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(snap));
  EXPECT_FALSE(eval.EvaluateSnapshot(snap));

  // On the other tank, or crowd-controlled on the healer: no wake.
  pb::Snapshot quiet = snap;
  AddNearbyUnit(&quiet, "0x2", "Brute", pb::Unit::NPC, pb::Unit::HOSTILE, 9.0f)
      ->target = "Offtank";
  pb::Unit* sheep = AddNearbyUnit(&quiet, "0x3", "Shaman", pb::Unit::NPC,
                                  pb::Unit::HOSTILE, 6.0f);
  sheep->target = "Hele";
  sheep->crowd_control.emplace().name = "Polymorph";
  EXPECT_FALSE(eval.EvaluateSnapshot(quiet));

  // The sheep breaks and runs at the healer: one P1 wake, then latched.
  pb::Snapshot broke = quiet;
  broke.base.nearby.units[2].crowd_control.reset();
  std::optional<WakeTrigger> t = eval.EvaluateSnapshot(broke);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP1Danger);
  EXPECT_EQ(t->reason, "loose mob Shaman on Hele");
  EXPECT_TRUE(t->kinds & kWakeParty);
  EXPECT_FALSE(eval.EvaluateSnapshot(broke));

  // Taken back by the tank, then lost again: a new wake.
  pb::Snapshot taken = broke;
  taken.base.nearby.units[2].target = "Tanky";
  EXPECT_FALSE(eval.EvaluateSnapshot(taken));
  EXPECT_TRUE(eval.EvaluateSnapshot(broke));

  // Out of combat too (the mage opened with a sheep; the rest run at Hele).
  TriggerEvaluator ooc_eval;
  pb::Snapshot ooc = snap;
  ooc.base.self.in_combat = false;
  ooc.base.nearby.units.clear();
  EXPECT_FALSE(ooc_eval.EvaluateSnapshot(ooc));
  AddNearbyUnit(&ooc, "0x4", "Cultist", pb::Unit::NPC, pb::Unit::HOSTILE, 14.0f)
      ->target = "Hele";
  t = ooc_eval.EvaluateSnapshot(ooc);
  ASSERT_TRUE(t);
  EXPECT_NE(t->reason.find("loose mob Cultist on Hele"), std::string::npos);

  // Not a tank: the same screens wake nothing new.
  TriggerEvaluator dps_eval;
  pb::Snapshot dps = snap;
  dps.base.self.unit.group->role = pb::DPS;
  EXPECT_FALSE(dps_eval.EvaluateSnapshot(dps));
  pb::Snapshot dps_broke = broke;
  dps_broke.base.self.unit.group->role = pb::DPS;
  EXPECT_FALSE(dps_eval.EvaluateSnapshot(dps_broke));
}

// A mob fixated on a member ignores threat (a kite, a scripted fixate), so it
// is not loose: no wake; once the fixate ends and it stays on them, one wake.
TEST(TriggersTest, FixatedMobOnAMemberDoesNotWakeTheTank) {
  pb::Snapshot snap = BaseSnap();
  snap.bot = "Tanky";
  snap.base.self.in_combat = true;
  snap.base.self.unit.name = "Tanky";
  snap.base.self.unit.group.emplace().role = pb::TANK;
  pb::Unit& m = snap.ext.party.emplace().members.emplace_back();
  m.name = "Hunt";
  m.health_pct = 100.0f;
  m.group.emplace().online = true;
  m.group->role = pb::DPS;
  AddNearbyUnit(&snap, "0x1", "Trogg", pb::Unit::NPC, pb::Unit::HOSTILE, 3.0f,
                true)
      ->target = "Tanky";
  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(snap));

  pb::Snapshot fixed = snap;
  pb::Unit* chaser = AddNearbyUnit(&fixed, "0x2", "Spark", pb::Unit::NPC,
                                   pb::Unit::HOSTILE, 12.0f);
  chaser->target = "Hunt";
  chaser->fixated_on = "Hunt";
  chaser->taunt_immune = true;
  EXPECT_FALSE(eval.EvaluateSnapshot(fixed));
  EXPECT_FALSE(eval.EvaluateSnapshot(fixed));

  pb::Snapshot freed = fixed;
  freed.base.nearby.units[1].fixated_on.clear();
  std::optional<WakeTrigger> t = eval.EvaluateSnapshot(freed);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "loose mob Spark on Hunt");
}

// An out-of-combat spell miss wakes the caster once per seq at P2Routine; in
// combat, routine dodge/parry/resist/miss do not wake (avoiding wake storms),
// while immune/evade still wake at P2Routine; simultaneous P0 combat wakes
// always preserve P0 priority.
TEST(TriggersTest, SpellMissWakesOncePerMissAndPreservesP0CombatPriority) {
  // Solo out-of-combat caster (e.g. opener Sap): wakes at P2Routine once per
  // seq.
  pb::Snapshot solo = BaseSnap();
  TriggerEvaluator solo_eval;
  EXPECT_FALSE(solo_eval.EvaluateSnapshot(solo));

  pb::Snapshot miss1 = solo;
  miss1.base.self.spell_miss = pb::Self::SpellMiss{
      .spell = "Sap",
      .target = "Defias Cutpurse",
      .reason = "dodge",
      .seq = 1,
  };
  std::optional<WakeTrigger> t = solo_eval.EvaluateSnapshot(miss1);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP2Routine);
  EXPECT_EQ(t->kinds, kWakeSpellMiss);
  EXPECT_EQ(t->reason, "missed: Sap on Defias Cutpurse (dodge)");

  // Same miss still on screen on the next tick: no repeat wake.
  EXPECT_FALSE(solo_eval.EvaluateSnapshot(miss1));

  // A second consecutive miss of the exact same spell on the same target (new
  // seq): wakes again.
  pb::Snapshot miss2 = miss1;
  miss2.base.self.spell_miss->seq = 2;
  t = solo_eval.EvaluateSnapshot(miss2);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP2Routine);
  EXPECT_EQ(t->reason, "missed: Sap on Defias Cutpurse (dodge)");

  // In combat: routine dodge/parry/resist/miss do NOT wake (prevents wake
  // storms during playerbots' combat rotation).
  pb::Snapshot combat = BaseSnap();
  combat.base.self.in_combat = true;
  TriggerEvaluator combat_eval;
  EXPECT_FALSE(combat_eval.EvaluateSnapshot(combat));
  uint32_t seq = 1;
  for (const char* reason : {"dodge", "parry", "resist", "miss"}) {
    pb::Snapshot m = combat;
    m.base.self.spell_miss = pb::Self::SpellMiss{
        .spell = "Heroic Strike",
        .target = "Mottled Boar",
        .reason = reason,
        .seq = seq++,
    };
    EXPECT_FALSE(combat_eval.EvaluateSnapshot(m)) << reason;
  }

  // In combat: immune and evade DO wake at P2Routine.
  for (const char* reason : {"immune", "evade"}) {
    pb::Snapshot m = combat;
    m.base.self.spell_miss = pb::Self::SpellMiss{
        .spell = "Frostbolt",
        .target = "Water Elemental",
        .reason = reason,
        .seq = seq++,
    };
    t = combat_eval.EvaluateSnapshot(m);
    ASSERT_TRUE(t) << reason;
    EXPECT_EQ(t->urgency, Urgency::kP2Routine);
    EXPECT_EQ(t->kinds, kWakeSpellMiss);
  }

  // Adversarial counter-test: grouped caster with simultaneous P0 combat wake
  // (death) still gets P0 priority, never downgraded to P2.
  pb::Snapshot grouped = BaseSnap();
  grouped.base.self.unit.group.emplace().role = pb::DPS;
  pb::Unit& ally = grouped.ext.party.emplace().members.emplace_back();
  ally.name = "Aldric";
  ally.group.emplace().online = true;
  ally.group->role = pb::TANK;
  TriggerEvaluator grouped_eval;
  EXPECT_FALSE(grouped_eval.EvaluateSnapshot(grouped));

  pb::Snapshot miss_and_died = grouped;
  miss_and_died.base.self.state = pb::Self::DEAD_BODY;
  miss_and_died.base.self.spell_miss = pb::Self::SpellMiss{
      .spell = "Polymorph",
      .target = "Defias Pillager",
      .reason = "resist",
      .seq = 1,
  };
  t = grouped_eval.EvaluateSnapshot(miss_and_died);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP0Combat);
  EXPECT_NE(t->reason.find("missed: Polymorph on Defias Pillager (resist)"),
            std::string::npos);
}

// RESURRECT_OFFER prompt does not redundantly wake the brain (playerbots
// accepts rez offers itself and `!was_alive_ && alive` wakes once alive),
// while other timed prompts still wake at P1Danger.
TEST(TriggersTest, ResurrectOfferPromptDoesNotWakeOtherPromptsStillDo) {
  pb::Event rez;
  rez.prompt.emplace().kind = pb::PromptEvent::RESURRECT_OFFER;
  EXPECT_EQ(EvaluateEventTrigger(rez), std::nullopt);
  EXPECT_EQ(DescribeEvent(rez), "prompt: resurrect offer");

  // Adversarial counter-tests: other prompts still wake at P1Danger.
  for (pb::PromptEvent::Kind kind :
       {pb::PromptEvent::READY_CHECK, pb::PromptEvent::GROUP_INVITE,
        pb::PromptEvent::SUMMON, pb::PromptEvent::LFG_PROPOSAL}) {
    pb::Event ev;
    ev.prompt.emplace().kind = kind;
    const std::optional<WakeTrigger> trig = EvaluateEventTrigger(ev);
    ASSERT_TRUE(trig);
    EXPECT_EQ(trig->urgency, Urgency::kP1Danger);
  }
}

// Tank debuffs on the other tank wake a tank bot on new debuff and stack
// increases, never repeat on the same stack, ignore non-tanks / solo /
// Dungeon Deserter, still wake on loose mobs, reset cleanly, and never
// downgrade a simultaneous P0 combat wake.
TEST(TriggersTest, TankDebuffsWakeOtherTankAndRespectCounterInvariants) {
  pb::Snapshot base = BaseSnap();
  base.bot = "Offtank";
  base.base.self.unit.name = "Offtank";
  base.base.self.unit.group.emplace().role = pb::TANK;
  base.base.self.in_combat = true;
  pb::Party& party = base.ext.party.emplace();
  pb::Unit& mt = party.members.emplace_back();
  mt.guid = "0x10";
  mt.name = "MainTank";
  mt.group.emplace().role = pb::TANK;
  mt.group->online = true;
  mt.group->in_range = true;
  pb::Unit& healer = party.members.emplace_back();
  healer.guid = "0x11";
  healer.name = "Hele";
  healer.group.emplace().role = pb::HEALER;
  healer.group->online = true;
  healer.group->in_range = true;

  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(base));

  auto make_aura = [](uint32_t id, std::string name, bool harmful,
                      uint32_t stacks = 0) {
    pb::Aura a;
    a.id = id;
    a.name = std::move(name);
    a.harmful = harmful;
    a.stacks = stacks;
    return a;
  };

  // Stack 1 arrives: wakes once.
  pb::Snapshot s1 = base;
  s1.ext.party->members[0].auras.push_back(
      make_aura(72451, "Mutated Plague", true, 0));
  std::optional<WakeTrigger> t = eval.EvaluateSnapshot(s1);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP1Danger);
  EXPECT_EQ(t->reason, "tank MainTank has Mutated Plague");

  // Same stack on next tick: no repeat wake.
  EXPECT_FALSE(eval.EvaluateSnapshot(s1));

  // Stack increases to 3: wakes with stack count.
  pb::Snapshot s3 = s1;
  s3.ext.party->members[0].auras[0].stacks = 3;
  t = eval.EvaluateSnapshot(s3);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "tank MainTank has Mutated Plague x3");
  EXPECT_FALSE(eval.EvaluateSnapshot(s3));

  // Debuff clears, then re-applies: wakes again.
  EXPECT_FALSE(eval.EvaluateSnapshot(base));
  t = eval.EvaluateSnapshot(s1);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "tank MainTank has Mutated Plague");

  // Adversarial counter-test 1: un-noted loose mob on healer still wakes tank.
  pb::Snapshot loose = s1;
  pb::Unit* add = AddNearbyUnit(&loose, "0x99", "Cultist", pb::Unit::NPC,
                                pb::Unit::HOSTILE, 10.0f);
  add->target = "Hele";
  t = eval.EvaluateSnapshot(loose);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "loose mob Cultist on Hele");

  // Adversarial counter-test 2: non-tank (DPS) does NOT wake on tank debuffs.
  pb::Snapshot dps_base = base;
  dps_base.base.self.unit.group->role = pb::DPS;
  TriggerEvaluator dps_eval;
  EXPECT_FALSE(dps_eval.EvaluateSnapshot(dps_base));
  pb::Snapshot dps_s1 = s1;
  dps_s1.base.self.unit.group->role = pb::DPS;
  EXPECT_FALSE(dps_eval.EvaluateSnapshot(dps_s1));

  // Adversarial counter-test 3: solo player does NOT wake.
  pb::Snapshot solo = base;
  solo.ext.party.reset();
  solo.base.self.unit.group.reset();
  TriggerEvaluator solo_eval;
  EXPECT_FALSE(solo_eval.EvaluateSnapshot(solo));

  // Adversarial counter-test 4: Dungeon Deserter and positive buffs don't wake.
  pb::Snapshot ignored = base;
  ignored.ext.party->members[0].auras.push_back(
      make_aura(71041, "Dungeon Deserter", true));
  ignored.ext.party->members[0].auras.push_back(
      make_aura(25898, "Blessing of Kings", false));
  TriggerEvaluator ign_eval;
  EXPECT_FALSE(ign_eval.EvaluateSnapshot(base));
  EXPECT_FALSE(ign_eval.EvaluateSnapshot(ignored));

  // Adversarial counter-test 5: disconnect/logout reset (fresh evaluator)
  // re-arms cleanly, and simultaneous P0 death is never downgraded.
  eval = TriggerEvaluator{};
  EXPECT_FALSE(eval.EvaluateSnapshot(base));
  pb::Snapshot death_and_debuff = s1;
  death_and_debuff.base.self.state = pb::Self::DEAD_BODY;
  t = eval.EvaluateSnapshot(death_and_debuff);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP0Combat);
}

TEST(TriggersTest, TankWakesOnLooseMobAt30YdAndHealerManaRecoveryWakesOoc) {
  pb::Snapshot base = BaseSnap();
  base.base.self.unit.name = "Tank";
  base.base.self.unit.group.emplace().role = pb::TANK;
  pb::Party& party = base.ext.party.emplace();
  party.leader = "Tank";
  pb::Unit& healer = party.members.emplace_back();
  healer.guid = "0x11";
  healer.name = "Hele";
  healer.health_pct = 100.0f;
  healer.power_pct = 90.0f;
  pb::GroupPart& hgp = healer.group.emplace();
  hgp.role = pb::HEALER;
  hgp.online = true;
  hgp.in_range = true;

  TriggerEvaluator eval;
  EXPECT_FALSE(eval.EvaluateSnapshot(base));

  // 1. Loose mob hitting healer at 30 yd (past 20 yd nameplate range) wakes
  //    the tank once nameplate detail exposes its target.
  pb::Snapshot loose30 = base;
  pb::Unit* mob = AddNearbyUnit(&loose30, "0x30", "Defias Pillager",
                                pb::Unit::NPC, pb::Unit::HOSTILE, 30.0f);
  mob->detail = pb::Unit::NAMEPLATE;
  mob->target = "Hele";
  std::optional<WakeTrigger> t = eval.EvaluateSnapshot(loose30);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP1Danger);
  EXPECT_EQ(t->reason, "loose mob Defias Pillager on Hele");

  // 2. Healer drops below 30% mana -> wakes once; hysteresis prevents spam at
  //    40% -> 25%; recovering to >= 75% out of combat wakes with "mana ready".
  pb::Snapshot oom = base;
  oom.ext.party->members[0].power_pct = 20.0f;
  t = eval.EvaluateSnapshot(oom);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->reason, "healer Hele low on mana");

  pb::Snapshot mid = base;
  mid.ext.party->members[0].power_pct = 45.0f;
  EXPECT_FALSE(eval.EvaluateSnapshot(mid));
  EXPECT_FALSE(eval.EvaluateSnapshot(oom));  // Latched until >= 75%.

  pb::Snapshot ready = base;
  ready.ext.party->members[0].power_pct = 75.0f;
  t = eval.EvaluateSnapshot(ready);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kNormal);
  EXPECT_EQ(t->reason, "healer Hele mana ready");
  EXPECT_FALSE(eval.EvaluateSnapshot(ready));  // Once per recovery.

  // 3. Adversarial counter-test: recovering mana IN COMBAT does not emit
  //    "mana ready", and going offline/dead does not emit "mana ready".
  pb::Snapshot combat_oom = oom;
  combat_oom.base.self.in_combat = true;
  t = eval.EvaluateSnapshot(combat_oom);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP0Combat);  // Entered combat beats low mana.

  pb::Snapshot combat_ready = ready;
  combat_ready.base.self.in_combat = true;
  EXPECT_FALSE(eval.EvaluateSnapshot(combat_ready));
}

TEST(TriggersTest, RaidScaleFiltersNonUrgentStatusForDpsOnly) {
  auto make_raid = [](const std::string& bot, pb::Role self_role,
                      size_t total_members) {
    pb::Snapshot snap = BaseSnap();
    snap.bot = bot;
    snap.base.self.in_combat = true;
    snap.base.self.unit.name = bot;
    snap.base.self.unit.group.emplace().role = self_role;
    pb::Party& p = snap.ext.party.emplace();
    for (size_t i = 0; i + 1 < total_members; ++i) {
      pb::Unit& m = p.members.emplace_back();
      m.name = "Raider" + std::to_string(i);
      m.health_pct = 100.0f;
      m.power_pct = 100.0f;
      pb::GroupPart& gp = m.group.emplace();
      gp.online = true;
      gp.in_range = true;
      gp.role = (i == 0) ? pb::TANK : (i == 1 ? pb::HEALER : pb::DPS);
    }
    if (total_members > 5) {
      snap.ext.raid.emplace();
    }
    return snap;
  };

  // 25-man raid: Tank, Healer, and DPS all start with a clean baseline.
  pb::Snapshot tank_base = make_raid("TankBot", pb::TANK, 25);
  pb::Snapshot heal_base = make_raid("HealBot", pb::HEALER, 25);
  pb::Snapshot dps_base = make_raid("DpsBot", pb::DPS, 25);
  TriggerEvaluator tank_eval, heal_eval, dps_eval;
  EXPECT_FALSE(tank_eval.EvaluateSnapshot(tank_base));
  EXPECT_FALSE(heal_eval.EvaluateSnapshot(heal_base));
  EXPECT_FALSE(dps_eval.EvaluateSnapshot(dps_base));

  // Raider2 (DPS) dies, Raider3 drops to 20% HP, Raider1 (healer) hits 20%
  // mana: Tank and Healer wake at P1Danger; DPS does NOT wake.
  auto apply_status = [](pb::Snapshot s) {
    s.ext.party->members[2].dead = true;
    s.ext.party->members[3].health_pct = 20.0f;
    s.ext.party->members[1].power_pct = 20.0f;
    return s;
  };
  const std::optional<WakeTrigger> tank_w =
      tank_eval.EvaluateSnapshot(apply_status(tank_base));
  const std::optional<WakeTrigger> heal_w =
      heal_eval.EvaluateSnapshot(apply_status(heal_base));
  const std::optional<WakeTrigger> dps_w =
      dps_eval.EvaluateSnapshot(apply_status(dps_base));
  ASSERT_TRUE(tank_w);
  EXPECT_EQ(tank_w->urgency, Urgency::kP1Danger);
  EXPECT_NE(tank_w->reason.find("party member Raider2 died"),
            std::string::npos);
  ASSERT_TRUE(heal_w);
  EXPECT_EQ(heal_w->urgency, Urgency::kP1Danger);
  EXPECT_NE(heal_w->reason.find("near death"), std::string::npos);
  EXPECT_FALSE(dps_w);

  // Counter-test 1: P0 personal combat wake STILL wakes DPS immediately in a
  // 25-man raid.
  pb::Snapshot dps_attacked = apply_status(dps_base);
  AddNearbyUnit(&dps_attacked, "0x99", "BossAdd", pb::Unit::NPC,
                pb::Unit::HOSTILE, 5.0f, /*targeting_me=*/true);
  const std::optional<WakeTrigger> dps_p0 =
      dps_eval.EvaluateSnapshot(dps_attacked);
  ASSERT_TRUE(dps_p0);
  EXPECT_EQ(dps_p0->urgency, Urgency::kP0Combat);
  EXPECT_EQ(dps_p0->reason, "attacked by BossAdd");

  // Counter-test 2: 5-man party responsiveness is preserved — DPS STILL wakes
  // on party member death, near-death, and healer low mana.
  pb::Snapshot five_man_dps = make_raid("DpsFive", pb::DPS, 5);
  TriggerEvaluator five_eval;
  EXPECT_FALSE(five_eval.EvaluateSnapshot(five_man_dps));
  const std::optional<WakeTrigger> five_w =
      five_eval.EvaluateSnapshot(apply_status(five_man_dps));
  ASSERT_TRUE(five_w);
  EXPECT_EQ(five_w->urgency, Urgency::kP1Danger);
  EXPECT_NE(five_w->reason.find("party member Raider2 died"),
            std::string::npos);
}

TEST(TriggersTest, HostileGroundAreaWakesP0AndIgnoresFriendlyOrDead) {
  TriggerEvaluator eval;
  pb::Snapshot base = BaseSnap();
  EXPECT_FALSE(eval.EvaluateSnapshot(base));

  // Friendly ground area (e.g. friendly Consecration) never wakes.
  pb::Snapshot friendly = base;
  pb::Area& f = friendly.base.nearby.areas.emplace_back();
  f.guid = "Area-Friendly";
  f.name = "Consecration";
  f.radius_yd = 8.0f;
  f.inside_me = true;
  f.hostile = false;
  EXPECT_FALSE(eval.EvaluateSnapshot(friendly));

  // Stepping into hostile ground fire wakes at P0Combat with kWakeHazard.
  pb::Snapshot fire = base;
  pb::Area& h = fire.base.nearby.areas.emplace_back();
  h.guid = "Area-Hostile";
  h.name = "Flamestrike";
  h.radius_yd = 6.0f;
  h.inside_me = true;
  h.hostile = true;
  const std::optional<WakeTrigger> t = eval.EvaluateSnapshot(fire);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->urgency, Urgency::kP0Combat);
  EXPECT_EQ(t->kinds, kWakeHazard);
  EXPECT_EQ(t->reason, "standing in hostile Flamestrike");
  // Edge-latched while remaining in the same hazard.
  EXPECT_FALSE(eval.EvaluateSnapshot(fire));

  // Counter-test: dead bot in a new hostile area does not emit hazard wake.
  pb::Snapshot dead_fire = base;
  dead_fire.base.self.state = pb::Self::DEAD_BODY;
  dead_fire.base.self.unit.dead = true;
  pb::Area& h2 = dead_fire.base.nearby.areas.emplace_back();
  h2.guid = "Area-Hostile-2";
  h2.name = "Rain of Fire";
  h2.radius_yd = 6.0f;
  h2.inside_me = true;
  h2.hostile = true;
  const std::optional<WakeTrigger> dead_t = eval.EvaluateSnapshot(dead_fire);
  ASSERT_TRUE(dead_t);
  EXPECT_EQ(dead_t->kinds, kWakeDied);
}

}  // namespace
}  // namespace harness
