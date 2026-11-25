// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/options.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

pb::Unit* AddUnit(pb::Snapshot* snapshot,
                  const std::string& name,
                  const std::string& guid,
                  float dist_yd,
                  pb::Unit::Reaction reaction = pb::Unit::HOSTILE) {
  pb::Unit& u = snapshot->base.nearby.units.emplace_back();
  u.name = name;
  u.guid = guid;
  u.kind = pb::Unit::NPC;
  u.reaction = reaction;
  u.rel.dist_yd = dist_yd;
  return &u;
}

std::vector<std::string> Texts(const pb::Snapshot& snapshot) {
  std::vector<std::string> texts;
  for (const pb::Option& option : snapshot.options) {
    texts.push_back(option.text);
  }
  return texts;
}

TEST(OptionsTest, OnlyKeepOnAnEmptyScreen) {
  pb::Snapshot snapshot;
  const std::vector<OptionTarget> targets = AddOptions(&snapshot);
  ASSERT_EQ(targets.size(), 1u);
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kKeep);
  EXPECT_EQ(Texts(snapshot),
            std::vector<std::string>{"Keep doing what I'm doing"});
}

TEST(OptionsTest, OneAttackPerNameTargetingTheNearest) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Scorpid Worker", "0x1", 20);
  AddUnit(&snapshot, "Vile Familiar", "0x2", 30);
  AddUnit(&snapshot, "Scorpid Worker", "0x3", 8);  // Nearer: the target.
  AddUnit(&snapshot, "Scorpid Worker", "0x4", 12);
  const std::vector<OptionTarget> targets = AddOptions(&snapshot);
  EXPECT_EQ(
      Texts(snapshot),
      (std::vector<std::string>{"Attack Scorpid Worker", "Attack Vile Familiar",
                                "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 3u);
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kAttack);
  EXPECT_EQ(targets[0].guid, "0x3");
  EXPECT_EQ(targets[0].name, "Scorpid Worker");
  EXPECT_EQ(targets[1].guid, "0x2");
  // The screen puts each number on that unit's own line.
  EXPECT_EQ(snapshot.options[0].guid, "0x3");
  EXPECT_EQ(snapshot.options[2].guid, "");
}

// Now #4: the leader's marks order the kills: skull first, then cross, and
// among mobs of one name the marked one is the target; the mark is shown.
TEST(OptionsTest, MarkedMobsComeFirstAndSayTheirMark) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Vile Familiar", "0x1", 5);
  AddUnit(&snapshot, "Scorpid Worker", "0x2", 8);
  AddUnit(&snapshot, "Scorpid Worker", "0x3", 25)->raid_marker = pb::SKULL;
  AddUnit(&snapshot, "Kobold", "0x4", 20)->raid_marker = pb::CROSS;
  AddUnit(&snapshot, "Kobold Miner", "0x5", 9)->raid_marker = pb::MOON;
  const std::vector<OptionTarget> targets = AddOptions(&snapshot);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{
                "Attack Scorpid Worker (skull)", "Attack Kobold (cross)",
                "Attack Vile Familiar", "Attack Kobold Miner (moon)",
                "Keep doing what I'm doing"}));
  EXPECT_EQ(targets[0].guid, "0x3");
}

// Now #4: when a human leads, they pull: a mob nobody in the party fights
// gets no Attack option (a gate, not a prompt rule).
TEST(OptionsTest, HumanLeaderPullsSoUnpulledMobsGetNoAttack) {
  pb::Snapshot snapshot;
  snapshot.bot = "Loada";
  snapshot.ext.party.emplace().members.emplace_back().name = "Hele";
  AddUnit(&snapshot, "Kobold Tunneler", "0x1", 10);
  AddUnit(&snapshot, "Kobold Miner", "0x2", 12)->target = "Hele";
  OptionFacts facts;
  facts.leave_pulls_to_leader = true;
  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Attack Kobold Miner",
                                      "Keep doing what I'm doing"}));
}

// The mob hitting the bot is the one a player hits back, even if another of
// the same name is nearer.
TEST(OptionsTest, AttackPrefersTheUnitAttackingTheBot) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Scorpid Worker", "0x1", 5);
  AddUnit(&snapshot, "Scorpid Worker", "0x2", 9)->targeting_me = true;
  const std::vector<OptionTarget> targets = AddOptions(&snapshot);
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_EQ(targets[0].guid, "0x2");
}

TEST(OptionsTest, OnlyKeepWhileDead) {
  for (pb::Self::LifeState state : {pb::Self::DEAD_BODY, pb::Self::GHOST}) {
    pb::Snapshot snapshot;
    snapshot.base.self.state = state;
    snapshot.base.self.health = 0;
    snapshot.base.self.health_max = 100;
    AddUnit(&snapshot, "Scorpid Worker", "0x1", 5);
    pb::Unit* corpse = AddUnit(&snapshot, "Vile Familiar", "0x2", 3);
    corpse->dead = true;
    corpse->lootable = true;
    OptionFacts facts;
    facts.spots.push_back(
        {"spot:1:0:0", pb::N, 200.0f, "boars, level 1-2", {0, 0, 0}});
    facts.places.push_back({"place:razor_hill",
                            "Razor Hill",
                            "innkeeper",
                            pb::N,
                            900.0f,
                            {0, 0, 0}});
    EXPECT_EQ(Texts(snapshot).size(), 0u);
    AddOptions(&snapshot, facts);
    if (state == pb::Self::DEAD_BODY) {
      EXPECT_EQ(Texts(snapshot),
                (std::vector<std::string>{"Release spirit",
                                          "Keep doing what I'm doing"}));
    } else {
      EXPECT_EQ(Texts(snapshot),
                std::vector<std::string>{"Keep doing what I'm doing"});
    }
  }
}

TEST(OptionsTest, NeutralCanBeAttackedFriendlyWithoutServiceOrQuestCannot) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Mottled Boar", "0x1", 10, pb::Unit::NEUTRAL);
  AddUnit(&snapshot, "Gornek", "0x2", 5, pb::Unit::FRIENDLY);
  AddOptions(&snapshot);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Attack Mottled Boar",
                                      "Keep doing what I'm doing"}));
}

TEST(OptionsTest, TalkOfferedForQuestGiversAndServiceNpcsOutOfCombat) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Kaltunk", "0x1", 6, pb::Unit::FRIENDLY)->quest_marker =
      pb::Unit::QUEST_AVAILABLE;
  AddUnit(&snapshot, "Gornek", "0x2", 8, pb::Unit::FRIENDLY)->quest_marker =
      pb::Unit::QUEST_INCOMPLETE;
  AddUnit(&snapshot, "Ken'jai", "0x3", 12, pb::Unit::FRIENDLY);
  AddUnit(&snapshot, "Grunt", "0x4", 10, pb::Unit::FRIENDLY);

  OptionFacts facts;
  facts.npcs.push_back({"0x3", kNpcFlagTrainer | kNpcFlagTrainerClass});
  facts.npcs.push_back({"0x4", kNpcFlagGossip});  // Plain gossip: not offered.

  const std::vector<OptionTarget> targets = AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Talk to Kaltunk", "Talk to Ken'jai",
                                      "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 3u);
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kTalk);
  EXPECT_EQ(targets[0].guid, "0x1");
  EXPECT_EQ(targets[1].kind, OptionTarget::Kind::kTalk);
  EXPECT_EQ(targets[1].guid, "0x3");

  // In combat: Talk is not offered.
  snapshot.options.clear();
  snapshot.base.self.in_combat = true;
  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            std::vector<std::string>{"Keep doing what I'm doing"});
}

TEST(OptionsTest, RestReflectsFoodAndDrinkInBagsWhenBelowFullOutOfCombat) {
  pb::Snapshot snapshot;
  snapshot.base.self.health = 50;
  snapshot.base.self.health_max = 100;
  snapshot.base.self.power_type = pb::Self::MANA;
  snapshot.base.self.power = 100;
  snapshot.base.self.power_max = 100;

  OptionFacts facts;
  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Rest (sit, no food)",
                                      "Keep doing what I'm doing"}));

  snapshot.options.clear();
  facts.has_food = true;
  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot), (std::vector<std::string>{
                                 "Rest (eat)", "Keep doing what I'm doing"}));

  snapshot.options.clear();
  facts.has_drink = true;
  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Rest (eat and drink)",
                                      "Keep doing what I'm doing"}));

  // Mounted or in combat: Rest is not offered.
  snapshot.options.clear();
  snapshot.base.self.mounted = true;
  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            std::vector<std::string>{"Keep doing what I'm doing"});
}

TEST(OptionsTest, ExploreAndGoToUseOptionFactsAndCarryDestinationPos) {
  pb::Snapshot snapshot;
  OptionFacts facts;
  facts.spots = {
      {"spot:1:-500:-4100",
       pb::NE,
       300.0f,
       "boars, scorpids, level 3-5",
       {-500, -4100, 40}},
      // Same direction (NE): skipped so each option has a distinct direction.
      {"spot:1:-450:-4050",
       pb::NE,
       380.0f,
       "boars, level 4",
       {-450, -4050, 40}},
      {"spot:1:-700:-4300",
       pb::S,
       220.0f,
       "imps, level 3-4",
       {-700, -4300, 50}},
  };
  facts.places = {
      {"place:razor_hill",
       "Razor Hill",
       "innkeeper, vendor, repair",
       pb::N,
       900.0f,
       {315, -4700, 10}},
  };

  const std::vector<OptionTarget> targets = AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{
                "Explore NE, 300 yd (boars, scorpids, level 3-5)",
                "Explore S, 220 yd (imps, level 3-4)",
                "Go to Razor Hill (innkeeper, vendor, repair), 900 yd N",
                "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 4u);
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kExplore);
  EXPECT_EQ(targets[0].guid, "spot:1:-500:-4100");
  EXPECT_EQ(targets[0].name, "boars, scorpids, level 3-5");
  ASSERT_TRUE(targets[0].pos.has_value());
  EXPECT_FLOAT_EQ(targets[0].pos->x, -500.0f);
  EXPECT_EQ(snapshot.options[0].guid, "spot:1:-500:-4100");
  EXPECT_EQ(snapshot.options[0].kind, pb::Option::EXPLORE);
  EXPECT_EQ(snapshot.options[0].name, "boars, scorpids, level 3-5");

  EXPECT_EQ(targets[2].kind, OptionTarget::Kind::kGoTo);
  EXPECT_EQ(targets[2].guid, "place:razor_hill");
  EXPECT_EQ(targets[2].name, "Razor Hill");
  ASSERT_TRUE(targets[2].pos.has_value());
  EXPECT_FLOAT_EQ(targets[2].pos->x, 315.0f);
  EXPECT_EQ(snapshot.options[2].guid, "place:razor_hill");
  EXPECT_EQ(snapshot.options[2].kind, pb::Option::GO_TO);
  EXPECT_EQ(snapshot.options[2].name, "Razor Hill");
  EXPECT_EQ(snapshot.options[3].kind, pb::Option::KEEP);
  EXPECT_EQ(snapshot.options[3].name, "");
}

TEST(OptionsTest, LootOnlyLootableCorpses) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Scorpid Worker", "0x1", 4)->dead = true;
  pb::Unit* lootable = AddUnit(&snapshot, "Scorpid Worker", "0x2", 6);
  lootable->dead = true;
  lootable->lootable = true;
  AddUnit(&snapshot, "Scorpid Worker", "0x3", 15);  // Alive: attack.
  const std::vector<OptionTarget> targets = AddOptions(&snapshot);
  EXPECT_EQ(Texts(snapshot), (std::vector<std::string>{
                                 "Loot Scorpid Worker", "Attack Scorpid Worker",
                                 "Keep doing what I'm doing"}));
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kLoot);
  EXPECT_EQ(targets[0].guid, "0x2");
  EXPECT_EQ(targets[1].guid, "0x3");
  EXPECT_EQ(snapshot.options[0].kind, pb::Option::LOOT);
  EXPECT_EQ(snapshot.options[0].name, "Scorpid Worker");
  EXPECT_EQ(snapshot.options[1].kind, pb::Option::ATTACK);
  EXPECT_EQ(snapshot.options[1].name, "Scorpid Worker");
}

TEST(OptionsTest, NothingForPlayersOrUnitsTooFarToName) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Someone", "0x1", 10)->kind = pb::Unit::PLAYER;
  AddUnit(&snapshot, "", "0x2", 60);  // Model only: no name to click.
  AddUnit(&snapshot, "No Guid", "", 10);
  AddOptions(&snapshot);
  EXPECT_EQ(Texts(snapshot),
            std::vector<std::string>{"Keep doing what I'm doing"});
}

TEST(OptionsTest, GroupInviteAcceptDeclineAndLeaveOptions) {
  pb::Snapshot snapshot;
  OptionFacts facts;
  facts.pending_group_invite_from = "Hele";
  facts.invite_candidates = {"Kelra"};
  facts.can_leave_group = true;
  const std::vector<OptionTarget> targets = AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Accept group invite from Hele",
                                      "Decline group invite from Hele",
                                      "Invite Kelra to group", "Leave group",
                                      "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 5u);
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kAcceptInvite);
  EXPECT_EQ(targets[0].name, "Hele");
  EXPECT_EQ(targets[1].kind, OptionTarget::Kind::kDeclineInvite);
  EXPECT_EQ(targets[2].kind, OptionTarget::Kind::kInvite);
  EXPECT_EQ(targets[2].name, "Kelra");
  EXPECT_EQ(targets[3].kind, OptionTarget::Kind::kLeaveGroup);
}

pb::GameObject* AddObject(pb::Snapshot* snapshot,
                          const std::string& name,
                          const std::string& guid,
                          float dist_yd,
                          pb::GameObject::Kind kind,
                          bool usable = true) {
  pb::GameObject& go = snapshot->base.nearby.objects.emplace_back();
  go.name = name;
  go.guid = guid;
  go.kind = kind;
  go.usable = usable;
  go.rel.dist_yd = dist_yd;
  return &go;
}

TEST(OptionsTest, GatherHerbsAndOreUseChestsAndQuestObjects) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Mottled Boar", "0x1", 10);
  AddObject(&snapshot, "Peacebloom", "0xa", 25, pb::GameObject::HERB);
  AddObject(&snapshot, "Peacebloom", "0xb", 12, pb::GameObject::HERB);
  AddObject(&snapshot, "Copper Vein", "0xc", 30, pb::GameObject::ORE);
  AddObject(&snapshot, "Battered Chest", "0xd", 8, pb::GameObject::CHEST);
  AddObject(&snapshot, "Wanted Poster", "0xe", 5, pb::GameObject::QUEST_OBJECT);
  const std::vector<OptionTarget> targets = AddOptions(&snapshot);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Attack Mottled Boar",
                                      "Gather Peacebloom", "Gather Copper Vein",
                                      "Use Battered Chest", "Use Wanted Poster",
                                      "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 6u);
  EXPECT_EQ(targets[1].kind, OptionTarget::Kind::kGather);
  EXPECT_EQ(targets[1].guid, "0xb");  // The nearer Peacebloom.
  EXPECT_EQ(snapshot.options[1].guid, "0xb");
  EXPECT_EQ(targets[3].kind, OptionTarget::Kind::kUse);
}

TEST(OptionsTest, GatherGatedByRequiredSkillAgainstLearnedSkill) {
  pb::Snapshot snapshot;
  AddObject(&snapshot, "Cactus Apple", "0x1", 8, pb::GameObject::HERB)
      ->required_skill = 0;
  AddObject(&snapshot, "Peacebloom", "0x2", 10, pb::GameObject::HERB)
      ->required_skill = 1;
  AddObject(&snapshot, "Kingsblood", "0x3", 12, pb::GameObject::HERB)
      ->required_skill = 125;
  AddObject(&snapshot, "Copper Vein", "0x4", 15, pb::GameObject::ORE)
      ->required_skill = 1;

  OptionFacts no_skills;
  AddOptions(&snapshot, no_skills);
  // Only Cactus Apple (required_skill = 0) is offered without Herbalism/Mining.
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Gather Cactus Apple",
                                      "Keep doing what I'm doing"}));

  snapshot.options.clear();
  OptionFacts apprentice_herbalist;
  apprentice_herbalist.herbalism_skill = 50;
  AddOptions(&snapshot, apprentice_herbalist);
  EXPECT_EQ(Texts(snapshot), (std::vector<std::string>{
                                 "Gather Cactus Apple", "Gather Peacebloom",
                                 "Keep doing what I'm doing"}));
}

TEST(OptionsTest, OptionFactsJsonRoundTrip) {
  OptionFacts facts;
  facts.npcs.push_back({"0x123", kNpcFlagTrainer});
  facts.has_food = true;
  facts.herbalism_skill = 75;
  facts.spots.push_back({"spot:1:-600:-4200",
                         pb::NE,
                         300.0f,
                         "boars, level 1-2",
                         {-600, -4200, 40}});
  facts.places.push_back({"place:razor_hill",
                          "Razor Hill",
                          "innkeeper, vendor",
                          pb::N,
                          900.0f,
                          {300, -4700, 10}});
  facts.usable_items.push_back({.entry = 37860,
                                .name = "Ruby Essence",
                                .out_of_combat_only = true,
                                .equippable = false,
                                .detail = ""});
  facts.party_targets.push_back({"Loada", "0x20"});

  const std::string json = pb::WriteJson(facts, /*pretty=*/true);
  ASSERT_FALSE(json.empty());
  OptionFacts parsed;
  std::string err;
  ASSERT_TRUE(pb::ReadJson(json, &parsed, &err)) << err;
  EXPECT_EQ(parsed, facts);
}

TEST(OptionsTest, NothingForObjectsTheBodyCantUseOrClick) {
  pb::Snapshot snapshot;
  AddObject(&snapshot, "Mailbox", "0x1", 5, pb::GameObject::MAILBOX);
  AddObject(&snapshot, "Barber Chair", "0x2", 5, pb::GameObject::BARBER_CHAIR);
  AddObject(&snapshot, "Peacebloom", "0x3", 5, pb::GameObject::HERB,
            /*usable=*/false);
  AddObject(&snapshot, "", "0x4", 5, pb::GameObject::HERB);
  AddOptions(&snapshot);
  EXPECT_EQ(Texts(snapshot),
            std::vector<std::string>{"Keep doing what I'm doing"});
}

base::TimeTicks AtMs(int64_t ms) {
  return base::TimeTicks() + base::Milliseconds(ms);
}

TEST(OptionHistoryTest, FindsAnOptionOfARecentSnapshot) {
  OptionHistory history(base::Seconds(30));
  history.Add(7, AtMs(1000),
              {{OptionTarget::Kind::kAttack, "0x1", "Boar"},
               {OptionTarget::Kind::kKeep, "", ""}});
  history.Add(8, AtMs(2000), {{OptionTarget::Kind::kKeep, "", ""}});
  const OptionHistory::Lookup found = history.Find(7, 1);
  ASSERT_NE(found.target, nullptr);
  EXPECT_EQ(found.target->guid, "0x1");
  EXPECT_EQ(found.snapshot_built, AtMs(1000));
  EXPECT_TRUE(found.error.empty());
  EXPECT_EQ(history.Find(8, 1).target->kind, OptionTarget::Kind::kKeep);
}

TEST(OptionHistoryTest, OptionNumbersAreOneBased) {
  OptionHistory history(base::Seconds(30));
  history.Add(1, AtMs(0), {{OptionTarget::Kind::kKeep, "", ""}});
  EXPECT_EQ(history.Find(1, 0).target, nullptr);
  EXPECT_EQ(history.Find(1, 0).error, "no option 0 in snapshot 1");
  EXPECT_EQ(history.Find(1, 2).error, "no option 2 in snapshot 1");
}

TEST(OptionHistoryTest, KeepsAWindowOfTimeNotACount) {
  OptionHistory history(base::Seconds(30));
  // Combat: a snapshot every 250 ms for 40 s. The window holds every one of
  // the last 30 s, however many that is.
  for (uint32_t seq = 1; seq <= 161; ++seq) {
    history.Add(seq, AtMs((seq - 1) * 250),
                {{OptionTarget::Kind::kKeep, "", ""}});
  }
  // The newest is at 40 s: 10 s and later are kept, earlier ones forgotten.
  EXPECT_EQ(history.Find(40, 1).target, nullptr);  // At 9.75 s.
  EXPECT_EQ(history.Find(40, 1).error, "snapshot 40 is too old or unknown");
  EXPECT_NE(history.Find(41, 1).target, nullptr);  // At 10 s.
  EXPECT_NE(history.Find(161, 1).target, nullptr);
}

TEST(OptionsTest, NonAttackableNeutralAndHostileNpcsAreNotOfferedAsAttack) {
  pb::Snapshot snapshot;
  pb::Unit* officer =
      AddUnit(&snapshot, "Warsong Recruitment Officer", "0x10", 8);
  officer->reaction = pb::Unit::NEUTRAL;
  pb::Unit* immune_boss = AddUnit(&snapshot, "Scripted Boss", "0x11", 12);
  immune_boss->reaction = pb::Unit::HOSTILE;
  pb::Unit* boar = AddUnit(&snapshot, "Mottled Boar", "0x12", 15);
  boar->reaction = pb::Unit::NEUTRAL;

  OptionFacts facts;
  facts.npcs.push_back({"0x10", kNpcFlagNotAttackable});
  facts.npcs.push_back({"0x11", kNpcFlagNotAttackable});

  const std::vector<OptionTarget> targets = AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Attack Mottled Boar",
                                      "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_EQ(targets[0].guid, "0x12");
}

TEST(OptionsTest, AssistOptionOfferedForMainAssistAndTankTargets) {
  pb::Snapshot snapshot;
  pb::Unit* egg = AddUnit(&snapshot, "Phoenix Egg", "0x20", 14);
  egg->reaction = pb::Unit::HOSTILE;
  pb::Unit* boss = AddUnit(&snapshot, "Kael'thas Sunstrider", "0x21", 20);
  boss->reaction = pb::Unit::HOSTILE;

  pb::Party& party = snapshot.ext.party.emplace();
  pb::Unit& tank = party.members.emplace_back();
  tank.name = "Loada";
  tank.target = "Kael'thas Sunstrider";
  tank.group.emplace().online = true;
  tank.group->role = pb::TANK;

  pb::Unit& ma = party.members.emplace_back();
  ma.name = "Loadc";
  ma.target = "Phoenix Egg";
  ma.group.emplace().online = true;
  ma.group->main_assist = true;

  // Duplicate target with tank: should deduplicate by guid.
  pb::Unit& ot = party.members.emplace_back();
  ot.name = "Loadf";
  ot.target = "Kael'thas Sunstrider";
  ot.group.emplace().online = true;
  ot.group->main_tank = true;

  const std::vector<OptionTarget> targets = AddOptions(&snapshot);
  // Expect: Attack Phoenix Egg, Attack Kael'thas, Attack main assist Loadc's
  // target (Phoenix Egg), Attack tank Loada's target (Kael'thas), Keep.
  ASSERT_EQ(targets.size(), 5u);
  EXPECT_EQ(snapshot.options[2].text,
            "Attack main assist Loadc's target (Phoenix Egg)");
  EXPECT_EQ(snapshot.options[2].guid, "assist:0x20");
  EXPECT_EQ(snapshot.options[2].name, "Phoenix Egg");
  EXPECT_EQ(targets[2].guid, "0x20");
  EXPECT_EQ(targets[2].name, "Phoenix Egg");
  EXPECT_EQ(snapshot.options[3].text,
            "Attack tank Loada's target (Kael'thas Sunstrider)");
  EXPECT_EQ(snapshot.options[3].guid, "assist:0x21");
  EXPECT_EQ(snapshot.options[3].name, "Kael'thas Sunstrider");
  EXPECT_EQ(targets[3].guid, "0x21");
  EXPECT_EQ(targets[3].name, "Kael'thas Sunstrider");
}

// RH-H2: in a hand-formed party (no tank flag), followers get the leader's
// target, and mobs nobody in the party fights yet are labelled.
TEST(OptionsTest, PartyFollowerSeesLeaderAssistAndNotPulledMobs) {
  pb::Snapshot snapshot;
  snapshot.bot = "Telda";
  pb::Unit* pulled = AddUnit(&snapshot, "Kobold Miner", "0x40", 10);
  pulled->reaction = pb::Unit::HOSTILE;
  pulled->target = "Hele";  // Fighting the leader.
  pb::Unit* idle = AddUnit(&snapshot, "Kobold Tunneler", "0x41", 20);
  idle->reaction = pb::Unit::HOSTILE;
  pb::Unit* on_me = AddUnit(&snapshot, "Kobold Laborer", "0x42", 5);
  on_me->reaction = pb::Unit::HOSTILE;
  on_me->targeting_me = true;

  pb::Party& party = snapshot.ext.party.emplace();
  pb::Unit& leader = party.members.emplace_back();
  leader.name = "Hele";
  leader.target = "Kobold Miner";
  leader.group.emplace().online = true;
  leader.group->leader = true;
  leader.group->role = pb::DPS;

  AddOptions(&snapshot);
  const std::vector<std::string> texts = Texts(snapshot);
  EXPECT_NE(std::find(texts.begin(), texts.end(), "Attack Kobold Miner"),
            texts.end());
  EXPECT_NE(std::find(texts.begin(), texts.end(),
                      "Attack Kobold Tunneler (not pulled)"),
            texts.end());
  EXPECT_NE(std::find(texts.begin(), texts.end(), "Attack Kobold Laborer"),
            texts.end());
  EXPECT_NE(std::find(texts.begin(), texts.end(),
                      "Attack leader Hele's target (Kobold Miner)"),
            texts.end());

  // Solo, nothing is labelled.
  pb::Snapshot solo = snapshot;
  solo.options.clear();
  solo.ext.party.reset();
  AddOptions(&solo);
  for (const std::string& t : Texts(solo)) {
    EXPECT_EQ(t.find("(not pulled)"), std::string::npos) << t;
  }
}

TEST(OptionsTest, AssistResolvesExactGuidCapAndEdgeCases) {
  pb::Snapshot snapshot;
  snapshot.bot = "SelfTank";
  // Two identical-name adds: 0x30 is nearer (8 yd), 0x31 is farther (22 yd).
  pb::Unit* egg_near = AddUnit(&snapshot, "Phoenix Egg", "0x30", 8);
  egg_near->reaction = pb::Unit::HOSTILE;
  pb::Unit* egg_far = AddUnit(&snapshot, "Phoenix Egg", "0x31", 22);
  egg_far->reaction = pb::Unit::HOSTILE;
  pb::Unit* mob2 = AddUnit(&snapshot, "Blood Beast", "0x32", 15);
  mob2->reaction = pb::Unit::HOSTILE;
  pb::Unit* mob3 = AddUnit(&snapshot, "Val'kyr", "0x33", 18);
  mob3->reaction = pb::Unit::HOSTILE;
  pb::Unit* dead_mob = AddUnit(&snapshot, "Dead Add", "0x34", 10);
  dead_mob->reaction = pb::Unit::HOSTILE;
  dead_mob->dead = true;

  pb::Party& party = snapshot.ext.party.emplace();
  // Self in party.members must be skipped.
  pb::Unit& self_m = party.members.emplace_back();
  self_m.name = "SelfTank";
  self_m.target = "Val'kyr";
  self_m.group.emplace().online = true;
  self_m.group->main_assist = true;

  // Main assist targets the FARTHER Phoenix Egg (0x31) via party_targets.
  pb::Unit& ma = party.members.emplace_back();
  ma.name = "Loadc";
  ma.target = "Phoenix Egg";
  ma.group.emplace().online = true;
  ma.group->main_assist = true;

  // Tank 1 targets Blood Beast (0x32), Tank 2 targets Val'kyr (0x33) -> capped
  // at 2.
  pb::Unit& t1 = party.members.emplace_back();
  t1.name = "Loada";
  t1.target = "Blood Beast";
  t1.group.emplace().online = true;
  t1.group->role = pb::TANK;

  pb::Unit& t2 = party.members.emplace_back();
  t2.name = "Loadf";
  t2.target = "Val'kyr";
  t2.group.emplace().online = true;
  t2.group->role = pb::TANK;

  // Caller targeting dead mob must be skipped.
  pb::Unit& dead_caller = party.members.emplace_back();
  dead_caller.name = "Loadg";
  dead_caller.target = "Dead Add";
  dead_caller.group.emplace().online = true;
  dead_caller.group->main_assist = true;

  OptionFacts facts;
  facts.party_targets.push_back({"Loadc", "0x31"});
  facts.party_targets.push_back({"Loada", "0x32"});
  facts.party_targets.push_back({"Loadf", "0x33"});
  facts.party_targets.push_back({"Loadg", "0x34"});

  const std::vector<OptionTarget> targets = AddOptions(&snapshot, facts);
  // 3 unique nearby Attack options (Phoenix Egg, Blood Beast, Val'kyr) +
  // 2 capped assist options (0x31 far egg, 0x32 Blood Beast) + Keep = 6.
  ASSERT_EQ(targets.size(), 6u);
  EXPECT_EQ(targets[3].guid, "0x31");  // Farther egg, NOT nearer 0x30!
  EXPECT_EQ(snapshot.options[3].guid, "assist:0x31");
  EXPECT_EQ(targets[4].guid, "0x32");
  EXPECT_EQ(snapshot.options[4].guid, "assist:0x32");

  // Dead self gets no attack or assist options (only Release spirit, the
  // party-only Wait for a resurrection (RH-M3), and Keep).
  pb::Snapshot dead_snap = snapshot;
  dead_snap.options.clear();
  dead_snap.base.self.state = pb::Self::DEAD_BODY;
  const std::vector<OptionTarget> dead_targets = AddOptions(&dead_snap, facts);
  EXPECT_EQ(Texts(dead_snap),
            (std::vector<std::string>{"Release spirit",
                                      "Wait for a resurrection (up to 5 min)",
                                      "Keep doing what I'm doing"}));
  EXPECT_EQ(dead_targets.size(), 3u);
  // With a rez already offered there is nothing to wait for.
  facts.has_pending_resurrect = true;
  dead_snap.options.clear();
  AddOptions(&dead_snap, facts);
  EXPECT_EQ(Texts(dead_snap),
            (std::vector<std::string>{"Accept resurrection", "Release spirit",
                                      "Keep doing what I'm doing"}));
}

TEST(OptionsTest, UsableBagItemsOfferedAsUseOptionsAndFilteredInCombat) {
  OptionFacts facts;
  facts.usable_items.push_back({.entry = 37860,
                                .name = "Ruby Essence",
                                .out_of_combat_only = true,
                                .equippable = false,
                                .detail = ""});
  facts.usable_items.push_back({.entry = 33447,
                                .name = "Runic Healing Potion",
                                .out_of_combat_only = false,
                                .equippable = false,
                                .detail = ""});
  facts.usable_items.push_back({.entry = 6948,
                                .name = "Hearthstone",
                                .out_of_combat_only = true,
                                .equippable = false,
                                .detail = "teleports home to Orgrimmar"});

  pb::Snapshot ooc;
  ooc.base.self.in_combat = false;
  const std::vector<OptionTarget> ooc_targets = AddOptions(&ooc, facts);
  EXPECT_EQ(Texts(ooc), (std::vector<std::string>{
                            "Use Ruby Essence", "Use Runic Healing Potion",
                            "Use Hearthstone (teleports home to Orgrimmar)",
                            "Keep doing what I'm doing"}));
  ASSERT_EQ(ooc_targets.size(), 4u);
  EXPECT_EQ(ooc_targets[0].kind, OptionTarget::Kind::kUse);
  EXPECT_EQ(ooc_targets[0].guid, "item:37860");
  EXPECT_EQ(ooc_targets[1].guid, "item:33447");
  EXPECT_EQ(ooc_targets[2].guid, "item:6948");

  pb::Snapshot ic;
  ic.base.self.in_combat = true;
  const std::vector<OptionTarget> ic_targets = AddOptions(&ic, facts);
  EXPECT_EQ(Texts(ic), (std::vector<std::string>{"Use Runic Healing Potion",
                                                 "Keep doing what I'm doing"}));
  ASSERT_EQ(ic_targets.size(), 2u);
  EXPECT_EQ(ic_targets[0].guid, "item:33447");
}

TEST(OptionsTest, EquippableBagUpgradesOfferedOutOfCombatWithDetail) {
  OptionFacts facts;
  facts.usable_items.push_back(
      {1234, "Robes of the Arugal", true, true, "ilvl 29 vs 14"});

  pb::Snapshot ooc;
  ooc.base.self.in_combat = false;
  const std::vector<OptionTarget> ooc_targets = AddOptions(&ooc, facts);
  EXPECT_EQ(Texts(ooc), (std::vector<std::string>{
                            "Equip Robes of the Arugal (ilvl 29 vs 14)",
                            "Keep doing what I'm doing"}));
  ASSERT_EQ(ooc_targets.size(), 2u);
  EXPECT_EQ(ooc_targets[0].kind, OptionTarget::Kind::kUse);
  EXPECT_EQ(ooc_targets[0].guid, "equip:1234");
  EXPECT_EQ(ooc_targets[0].name, "Robes of the Arugal");
  EXPECT_EQ(ooc.options[0].guid, "equip:1234");

  pb::Snapshot ic;
  ic.base.self.in_combat = true;
  const std::vector<OptionTarget> ic_targets = AddOptions(&ic, facts);
  EXPECT_EQ(Texts(ic), (std::vector<std::string>{"Keep doing what I'm doing"}));
  ASSERT_EQ(ic_targets.size(), 1u);
}

TEST(OptionsTest, QuestTurnInPlaceFormattedAsTurnInOption) {
  OptionFacts facts;
  facts.places.push_back({"place:quest:7:turnin",
                          "Northshire Abbey",
                          "turn in: Kobold Camp Cleanup",
                          pb::Direction::N,
                          120.0f,
                          {10.0f, 20.0f, 0.0f}});
  pb::Snapshot snap;
  snap.base.self.in_combat = false;
  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_EQ(snap.options[0].text,
            "Turn in Kobold Camp Cleanup at Northshire Abbey, 120 yd N");
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kGoTo);
}

TEST(OptionsTest, MovementAndRestAppearBeforeBagUtilityItems) {
  OptionFacts facts;
  facts.has_food = true;
  facts.spots.push_back({"spot:619:460:-1035",
                         pb::Direction::N,
                         187.0f,
                         "Deep Crawler (3)",
                         {460.0f, -1035.0f, 30.3f}});
  facts.places.push_back({"place:leader",
                          "Loadf",
                          "party leader",
                          pb::Direction::NW,
                          175.0f,
                          {450.0f, -1030.0f, 31.0f}});
  facts.usable_items.push_back({.entry = 6948,
                                .name = "Hearthstone",
                                .out_of_combat_only = true,
                                .equippable = false,
                                .detail = "teleports home to Sunstrider Isle"});
  facts.usable_items.push_back({.entry = 33447,
                                .name = "Runic Healing Potion",
                                .out_of_combat_only = false,
                                .equippable = false,
                                .detail = ""});

  pb::Snapshot snap;
  snap.base.self.in_combat = false;
  snap.base.self.health = 90;
  snap.base.self.health_max = 100;
  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  EXPECT_EQ(Texts(snap),
            (std::vector<std::string>{
                "Explore N, 187 yd (Deep Crawler (3))",
                "Go to Loadf (party leader), 175 yd NW", "Rest (eat)",
                "Use Hearthstone (teleports home to Sunstrider Isle)",
                "Use Runic Healing Potion", "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 6u);
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kExplore);
  EXPECT_EQ(targets[1].kind, OptionTarget::Kind::kGoTo);
  EXPECT_EQ(targets[2].kind, OptionTarget::Kind::kRest);
  EXPECT_EQ(targets[3].kind, OptionTarget::Kind::kUse);
}

TEST(OptionsTest, TalentTreeOptionsOfferedOutOfCombatWhenPointsUnspent) {
  OptionFacts facts;
  facts.talent_trees.push_back({0, "Holy", 0, 2});
  facts.talent_trees.push_back({1, "Protection", 14, 2});
  facts.talent_trees.push_back({2, "Retribution", 0, 2});

  pb::Snapshot ooc;
  ooc.base.self.in_combat = false;
  const std::vector<OptionTarget> ooc_targets = AddOptions(&ooc, facts);
  EXPECT_EQ(Texts(ooc), (std::vector<std::string>{
                            "Spend talent points in Holy (0 pts, 2 unspent)",
                            "Spend talent points in Protection (14 pts, 2 "
                            "unspent)",
                            "Spend talent points in Retribution (0 pts, 2 "
                            "unspent)",
                            "Keep doing what I'm doing"}));
  ASSERT_EQ(ooc_targets.size(), 4u);
  EXPECT_EQ(ooc_targets[1].guid, "talent:1");

  pb::Snapshot ic;
  ic.base.self.in_combat = true;
  AddOptions(&ic, facts);
  EXPECT_EQ(Texts(ic), (std::vector<std::string>{"Keep doing what I'm doing"}));
}

TEST(OptionsTest, DeadBodyOffersAcceptResurrectionAndReleaseSpirit) {
  OptionFacts facts;
  facts.has_pending_resurrect = true;

  pb::Snapshot dead;
  dead.base.self.state = pb::Self::DEAD_BODY;
  const std::vector<OptionTarget> targets = AddOptions(&dead, facts);
  EXPECT_EQ(Texts(dead),
            (std::vector<std::string>{"Accept resurrection", "Release spirit",
                                      "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 3u);
  EXPECT_EQ(targets[0].guid, "rez:accept");
  EXPECT_EQ(targets[1].guid, "rez:release");

  pb::Snapshot ghost;
  ghost.base.self.state = pb::Self::GHOST;
  AddOptions(&ghost, facts);
  EXPECT_EQ(Texts(ghost),
            (std::vector<std::string>{"Keep doing what I'm doing"}));
}

TEST(OptionsTest, ExplicitNpcActionsReplaceGenericTalkToOption) {
  pb::Snapshot snap;
  snap.base.self.in_combat = false;
  pb::Unit& npc = snap.base.nearby.units.emplace_back();
  npc.guid = "0x0000000000001234";
  npc.name = "Kaltunk";
  npc.kind = pb::Unit::NPC;
  npc.reaction = pb::Unit::FRIENDLY;
  npc.quest_marker = pb::Unit::QUEST_AVAILABLE;

  OptionFacts facts;
  facts.npcs.push_back({npc.guid, kNpcFlagQuestGiver});
  facts.npc_actions.push_back(
      {"npc_qaccept:0x0000000000001234:4641", npc.guid,
       "Accept quest \"Your Place In The World\" from Kaltunk"});
  facts.npc_actions.push_back(
      {"npc_qturnin:0x0000000000001234:788:0", npc.guid,
       "Turn in \"Cutting Teeth\" to Kaltunk (pick Rugged Boots, ilvl 5)"});
  facts.npc_actions.push_back(
      {"npc_qturnin:0x0000000000001234:788:1", npc.guid,
       "Turn in \"Cutting Teeth\" to Kaltunk (pick Cloth Gloves, ilvl 5)"});

  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  EXPECT_EQ(
      Texts(snap),
      (std::vector<std::string>{
          "Accept quest \"Your Place In The World\" from Kaltunk",
          "Turn in \"Cutting Teeth\" to Kaltunk (pick Rugged Boots, ilvl 5)",
          "Turn in \"Cutting Teeth\" to Kaltunk (pick Cloth Gloves, ilvl 5)",
          "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 4u);
  EXPECT_EQ(targets[0].guid, "npc_qaccept:0x0000000000001234:4641");
  EXPECT_EQ(targets[2].guid, "npc_qturnin:0x0000000000001234:788:1");
}

TEST(OptionsTest, GroupAndUpkeepActionsAreExposedToTheBrain) {
  OptionFacts facts;
  facts.group_and_upkeep_actions.push_back(
      {"roll:0x100:0:1", "Need Dal'Rend's Sacred Charge (ilvl 63)"});
  facts.group_and_upkeep_actions.push_back(
      {"roll:0x100:0:2", "Greed Dal'Rend's Sacred Charge (ilvl 63)"});
  facts.group_and_upkeep_actions.push_back(
      {"roll:0x100:0:0", "Pass Dal'Rend's Sacred Charge"});
  facts.group_and_upkeep_actions.push_back(
      {"ready:yes", "Ready (confirm ready check)"});
  facts.group_and_upkeep_actions.push_back(
      {"lfg:accept:42", "Accept Dungeon Finder proposal"});

  pb::Snapshot snap;
  snap.base.self.in_combat = true;
  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  EXPECT_EQ(
      Texts(snap),
      (std::vector<std::string>{
          "Need Dal'Rend's Sacred Charge (ilvl 63)",
          "Greed Dal'Rend's Sacred Charge (ilvl 63)",
          "Pass Dal'Rend's Sacred Charge", "Ready (confirm ready check)",
          "Accept Dungeon Finder proposal", "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 6u);
  EXPECT_EQ(targets[0].guid, "roll:0x100:0:1");
  EXPECT_EQ(targets[4].guid, "lfg:accept:42");
}

TEST(OptionsTest, BindHearthstoneReplacesGenericTalkAtInnkeeper) {
  pb::Snapshot snap;
  snap.base.self.in_combat = false;
  pb::Unit& inn = snap.base.nearby.units.emplace_back();
  inn.guid = "0x0000000000006928";
  inn.name = "Innkeeper Grosk";
  inn.kind = pb::Unit::NPC;
  inn.reaction = pb::Unit::FRIENDLY;

  OptionFacts facts;
  facts.npcs.push_back({inn.guid, kNpcFlagInnkeeper});
  facts.npc_actions.push_back(
      {"npc_bind:0x0000000000006928", inn.guid,
       "Bind hearthstone at Innkeeper Grosk (Razor Hill)"});

  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  EXPECT_EQ(Texts(snap), (std::vector<std::string>{
                             "Bind hearthstone at Innkeeper Grosk (Razor Hill)",
                             "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kUse);
  EXPECT_EQ(targets[0].guid, "npc_bind:0x0000000000006928");
}

}  // namespace
}  // namespace llm_brain
