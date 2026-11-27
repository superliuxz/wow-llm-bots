// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/options.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "options/action_types.h"
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

// Now #4: when pulls wait for the human (they lead, tank, or are AFK or
// behind), a mob nobody in the party fights gets no Attack option (a gate,
// not a prompt rule); one already on the party or the bot keeps it.
TEST(OptionsTest, HeldPullsLeaveNoAttackOnUnpulledMobs) {
  pb::Snapshot snapshot;
  snapshot.bot = "Loada";
  snapshot.ext.party.emplace().members.emplace_back().name = "Hele";
  AddUnit(&snapshot, "Kobold Tunneler", "0x1", 10);
  AddUnit(&snapshot, "Kobold Miner", "0x2", 12)->target = "Hele";
  AddUnit(&snapshot, "Kobold Worker", "0x3", 14)->targeting_me = true;
  OptionFacts facts;
  facts.hold_pulls = true;
  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot), (std::vector<std::string>{
                                 "Attack Kobold Miner", "Attack Kobold Worker",
                                 "Keep doing what I'm doing"}));
}

// Counter-test: when pulls aren't held (the group's bot tank under a human
// leader who doesn't tank), the unpulled mob is offered, saying so.
TEST(OptionsTest, UnheldPullsOfferUnpulledMobsMarkedNotPulled) {
  pb::Snapshot snapshot;
  snapshot.bot = "Torvin";
  snapshot.ext.party.emplace().members.emplace_back().name = "Hele";
  AddUnit(&snapshot, "Kobold Tunneler", "0x1", 10);
  AddOptions(&snapshot, OptionFacts{});
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Attack Kobold Tunneler (not pulled)",
                                      "Keep doing what I'm doing"}));
}

// A bot leading a dungeon group doesn't explore ahead while the human is
// held up; it can still go to a place, and Explore comes back after.
TEST(OptionsTest, HeldExploreDropsOnlyExplore) {
  OptionFacts facts;
  facts.spots = {{"spot:36:1:2", pb::N, 60.0f, "Defias Overseer", {1, 2, 3}}};
  facts.places = {{"place:exit", "Exit", "", pb::S, 80.0f, {4, 5, 6}}};
  facts.hold_explore = true;
  pb::Snapshot held;
  AddOptions(&held, facts);
  EXPECT_EQ(Texts(held),
            (std::vector<std::string>{"Go to Exit, 80 yd S",
                                      "Keep doing what I'm doing"}));
  facts.hold_explore = false;
  pb::Snapshot free;
  AddOptions(&free, facts);
  EXPECT_EQ(Texts(free),
            (std::vector<std::string>{"Explore N, 60 yd (Defias Overseer)",
                                      "Go to Exit, 80 yd S",
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

// Now #6: a soulstone or ankh up is offered while lying dead.
TEST(OptionsTest, SelfResurrectionIsOfferedWhileLyingDead) {
  pb::Snapshot snapshot;
  snapshot.base.self.state = pb::Self::DEAD_BODY;
  OptionFacts facts;
  facts.self_resurrect = "Soulstone Resurrection";
  const std::vector<OptionTarget> targets = AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot)[0],
            "Use Soulstone Resurrection (resurrect yourself)");
  EXPECT_EQ(targets[0].guid, "rez:self");
  pb::Snapshot stone;
  stone.base.self.state = pb::Self::DEAD_BODY;
  facts.self_resurrect = "Use Soulstone";
  AddOptions(&stone, facts);
  EXPECT_EQ(Texts(stone)[0], "Use Soulstone (resurrect yourself)");
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
  egg->target = "Loadc";
  pb::Unit* boss = AddUnit(&snapshot, "Kael'thas Sunstrider", "0x21", 20);
  boss->reaction = pb::Unit::HOSTILE;
  boss->target = "Loada";

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

// Finding #3: party-assist options obey hold_pulls and (not pulled) just like
// normal Attack options, so a human clicking a pack to inspect it doesn't make
// bots pull it.
TEST(OptionsTest, HeldPullsGateUnpulledPartyAssistTargets) {
  pb::Snapshot snapshot;
  snapshot.bot = "Telda";
  AddUnit(&snapshot, "Kobold Vermin", "0x50", 12);
  AddUnit(&snapshot, "Kobold Tunneler", "0x51", 15);
  AddUnit(&snapshot, "Kobold Geomancer", "0x52", 18);

  pb::Party& party = snapshot.ext.party.emplace();
  pb::Unit& ma = party.members.emplace_back();
  ma.name = "Loadc";
  ma.target = "Kobold Vermin";
  ma.group.emplace().online = true;
  ma.group->main_assist = true;

  pb::Unit& tank = party.members.emplace_back();
  tank.name = "Loada";
  tank.target = "Kobold Tunneler";
  tank.group.emplace().online = true;
  tank.group->role = pb::TANK;

  pb::Unit& leader = party.members.emplace_back();
  leader.name = "Hele";
  leader.target = "Kobold Geomancer";
  leader.group.emplace().online = true;
  leader.group->leader = true;
  leader.group->role = pb::DPS;

  OptionFacts facts;
  facts.hold_pulls = true;
  facts.party_targets = {
      {"Loadc", "0x50"}, {"Loada", "0x51"}, {"Hele", "0x52"}};

  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            std::vector<std::string>{"Keep doing what I'm doing"});
}

// When pulls are not held, unpulled party-assist targets are still offered but
// tagged `(not pulled)` consistently with normal Attack options.
TEST(OptionsTest, UnheldPullsTagUnpulledPartyAssistTargetsNotPulled) {
  pb::Snapshot snapshot;
  snapshot.bot = "Torvin";
  AddUnit(&snapshot, "Kobold Tunneler", "0x53", 12);
  AddUnit(&snapshot, "Kobold Geomancer", "0x54", 16);

  pb::Party& party = snapshot.ext.party.emplace();
  pb::Unit& tank = party.members.emplace_back();
  tank.name = "Loada";
  tank.target = "Kobold Tunneler";
  tank.group.emplace().online = true;
  tank.group->role = pb::TANK;

  pb::Unit& leader = party.members.emplace_back();
  leader.name = "Hele";
  leader.target = "Kobold Geomancer";
  leader.group.emplace().online = true;
  leader.group->leader = true;
  leader.group->role = pb::DPS;

  OptionFacts facts;
  facts.hold_pulls = false;
  facts.party_targets = {{"Loada", "0x53"}, {"Hele", "0x54"}};

  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{
                "Attack Kobold Tunneler (not pulled)",
                "Attack Kobold Geomancer (not pulled)",
                "Attack tank Loada's target (Kobold Tunneler) (not pulled)",
                "Attack leader Hele's target (Kobold Geomancer) (not pulled)",
                "Keep doing what I'm doing"}));
}

// Counter-test: when hold_pulls is on, already-pulled assist targets are still
// offered (without `(not pulled)`), and a gated unpulled caller does not
// consume one of the two assist slots.
TEST(OptionsTest, HeldPullsStillAllowAssistOnAlreadyPulledTargets) {
  pb::Snapshot snapshot;
  snapshot.bot = "Telda";
  AddUnit(&snapshot, "Idle Patrol", "0x60", 18);  // Unpulled.
  AddUnit(&snapshot, "Kobold Miner", "0x61", 10)->target = "Loada";
  AddUnit(&snapshot, "Kobold Laborer", "0x62", 8)->targeting_me = true;

  pb::Party& party = snapshot.ext.party.emplace();
  pb::Unit& ma = party.members.emplace_back();
  ma.name = "Loadc";
  ma.target = "Idle Patrol";
  ma.group.emplace().online = true;
  ma.group->main_assist = true;

  pb::Unit& tank = party.members.emplace_back();
  tank.name = "Loada";
  tank.target = "Kobold Miner";
  tank.group.emplace().online = true;
  tank.group->role = pb::TANK;

  pb::Unit& leader = party.members.emplace_back();
  leader.name = "Hele";
  leader.target = "Kobold Laborer";
  leader.group.emplace().online = true;
  leader.group->leader = true;
  leader.group->role = pb::DPS;

  OptionFacts facts;
  facts.hold_pulls = true;
  facts.party_targets = {
      {"Loadc", "0x60"}, {"Loada", "0x61"}, {"Hele", "0x62"}};

  AddOptions(&snapshot, facts);
  EXPECT_EQ(Texts(snapshot), (std::vector<std::string>{
                                 "Attack Kobold Miner", "Attack Kobold Laborer",
                                 "Attack tank Loada's target (Kobold Miner)",
                                 "Attack leader Hele's target (Kobold Laborer)",
                                 "Keep doing what I'm doing"}));
}

// Counter-test: in combat with hold_pulls on, active combat targets stay
// offered while an unpulled pack the leader inspects is still gated, and solo
// play is unaffected.
TEST(OptionsTest, HeldPullsInCombatAndSoloPlayCounterTests) {
  pb::Snapshot combat;
  combat.bot = "Telda";
  combat.base.self.in_combat = true;
  AddUnit(&combat, "Kobold Miner", "0x70", 6)->targeting_me = true;
  AddUnit(&combat, "Unpulled Pack", "0x71", 20);

  pb::Party& party = combat.ext.party.emplace();
  pb::Unit& leader = party.members.emplace_back();
  leader.name = "Hele";
  leader.target = "Unpulled Pack";
  leader.group.emplace().online = true;
  leader.group->leader = true;
  leader.group->role = pb::DPS;

  OptionFacts facts;
  facts.hold_pulls = true;
  facts.party_targets = {{"Hele", "0x71"}};

  AddOptions(&combat, facts);
  EXPECT_EQ(Texts(combat),
            (std::vector<std::string>{"Attack Kobold Miner",
                                      "Keep doing what I'm doing"}));

  // Solo play: no party means no pull gating and no `(not pulled)` tag.
  pb::Snapshot solo;
  solo.bot = "Telda";
  AddUnit(&solo, "Kobold Tunneler", "0x72", 12);
  AddOptions(&solo, facts);
  EXPECT_EQ(Texts(solo),
            (std::vector<std::string>{"Attack Kobold Tunneler",
                                      "Keep doing what I'm doing"}));
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

  // Adversarial counter-test: full party wipe (every member dead or offline)
  // must not offer `Wait for a resurrection` because nobody alive can rez.
  facts.has_pending_resurrect = false;
  pb::Snapshot wipe_snap = dead_snap;
  wipe_snap.options.clear();
  for (pb::Unit& m : wipe_snap.ext.party->members) {
    m.dead = true;
  }
  AddOptions(&wipe_snap, facts);
  EXPECT_EQ(Texts(wipe_snap),
            (std::vector<std::string>{"Release spirit",
                                      "Keep doing what I'm doing"}));

  // Offline-only party members also cannot resurrect.
  pb::Snapshot offline_snap = dead_snap;
  offline_snap.options.clear();
  for (pb::Unit& m : offline_snap.ext.party->members) {
    m.dead = false;
    m.group->online = false;
  }
  AddOptions(&offline_snap, facts);
  EXPECT_EQ(Texts(offline_snap),
            (std::vector<std::string>{"Release spirit",
                                      "Keep doing what I'm doing"}));

  // At least one online living teammate (regardless of role) restores Wait for
  // a resurrection so the brain decides.
  offline_snap.ext.party->members[0].group->online = true;
  offline_snap.options.clear();
  AddOptions(&offline_snap, facts);
  EXPECT_EQ(Texts(offline_snap),
            (std::vector<std::string>{"Release spirit",
                                      "Wait for a resurrection (up to 5 min)",
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

TEST(OptionsTest, RidingTrainingAndMountPurchaseSuppressedInCombat) {
  pb::Snapshot snap;
  snap.base.self.in_combat = false;
  pb::Unit& trainer = snap.base.nearby.units.emplace_back();
  trainer.guid = "0x0000000000004732";
  trainer.name = "Randal Hunter";
  trainer.kind = pb::Unit::NPC;
  trainer.reaction = pb::Unit::FRIENDLY;

  OptionFacts facts;
  facts.npcs.push_back({trainer.guid, kNpcFlagTrainer | kNpcFlagVendor});
  facts.npc_actions.push_back(
      {"npc_train_riding:0x0000000000004732:33388", trainer.guid,
       "Train Apprentice Riding (4g) at Randal Hunter"});
  facts.npc_actions.push_back(
      {"npc_buy:0x0000000000004732:5655", trainer.guid,
       "Buy Chestnut Mare Bridle (60% mount, 90s) from Randal Hunter"});
  facts.group_and_upkeep_actions.push_back({"dismount", "Dismount"});

  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  EXPECT_EQ(Texts(snap),
            (std::vector<std::string>{
                "Train Apprentice Riding (4g) at Randal Hunter",
                "Buy Chestnut Mare Bridle (60% mount, 90s) from Randal Hunter",
                "Dismount", "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 4u);
  const auto* train = std::get_if<NpcAction>(&targets[0].action);
  ASSERT_NE(train, nullptr);
  EXPECT_EQ(train->op, NpcAction::Op::kTrainRiding);
  EXPECT_EQ(train->arg1, 33388u);
  EXPECT_TRUE(std::holds_alternative<DismountAction>(targets[2].action));

  // Counter-test (P0 combat priority): in combat, NPC riding training and mount
  // purchase are suppressed, while Dismount remains available.
  pb::Snapshot combat_snap;
  combat_snap.base.self.in_combat = true;
  combat_snap.base.nearby.units.push_back(trainer);
  AddOptions(&combat_snap, facts);
  EXPECT_EQ(Texts(combat_snap), (std::vector<std::string>{
                                    "Dismount", "Keep doing what I'm doing"}));
}

TEST(OptionsTest, VehicleSpellAndExitOptionsAvailableInAndOutOfCombat) {
  pb::Snapshot snap;
  snap.base.self.state = pb::Self::ALIVE;
  OptionFacts facts;
  facts.group_and_upkeep_actions.push_back(
      {"vehicle:spell:50240", "Use Flame Spike (vehicle)"});
  facts.group_and_upkeep_actions.push_back({"vehicle:leave", "Leave vehicle"});

  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  EXPECT_EQ(Texts(snap), (std::vector<std::string>{
                             "Use Flame Spike (vehicle)", "Leave vehicle",
                             "Keep doing what I'm doing"}));
  ASSERT_EQ(targets.size(), 3u);
  const auto* vs = std::get_if<VehicleSpellAction>(&targets[0].action);
  ASSERT_NE(vs, nullptr);
  EXPECT_EQ(vs->spell_id, 50240u);
  EXPECT_TRUE(std::holds_alternative<LeaveVehicleAction>(targets[1].action));

  // Counter-test (P0 combat priority): vehicle controls remain available in
  // combat alongside hostile targets.
  pb::Snapshot combat_snap;
  combat_snap.base.self.in_combat = true;
  pb::Unit mob;
  mob.guid = "0xF130001000000001";
  mob.name = "Azure Drake";
  mob.kind = pb::Unit::NPC;
  mob.reaction = pb::Unit::HOSTILE;
  mob.rel.dist_yd = 25.0f;
  combat_snap.base.nearby.units.push_back(mob);
  AddOptions(&combat_snap, facts);
  EXPECT_EQ(Texts(combat_snap),
            (std::vector<std::string>{
                "Attack Azure Drake", "Use Flame Spike (vehicle)",
                "Leave vehicle", "Keep doing what I'm doing"}));

  // Counter-test (vehicle exit/disconnect reset): when vehicle actions are
  // absent, only normal options appear.
  pb::Snapshot exited_snap;
  AddOptions(&exited_snap, OptionFacts{});
  EXPECT_EQ(Texts(exited_snap),
            (std::vector<std::string>{"Keep doing what I'm doing"}));
}

TEST(OptionsTest, EscortQuestWaitsUntilPartyIsReadyOrSomeoneHasIt) {
  // Solo: no wait.
  EXPECT_EQ(EscortStartHoldReason(/*self_is_leader=*/false, {}), "");

  // Human in party who hasn't started it yet: bots never start ahead of them.
  EXPECT_NE(EscortStartHoldReason(/*self_is_leader=*/true,
                                  {{.is_human = true, .dist_to_npc_yd = 5.0f}}),
            "");

  // Once the human (or any party member) has the quest, bots accept at once.
  EXPECT_EQ(EscortStartHoldReason(
                /*self_is_leader=*/false,
                {{.is_human = true, .dist_to_npc_yd = 5.0f, .has_quest = true},
                 {.dist_to_npc_yd = 50.0f}}),
            "");

  // All-bot party: follower waits for the leader.
  EXPECT_NE(EscortStartHoldReason(/*self_is_leader=*/false,
                                  {{.dist_to_npc_yd = 4.0f}}),
            "");

  // All-bot party leader waits if a member is far away, dead, or missing the
  // prerequisite turn-in, and starts once all online members are ready.
  EXPECT_NE(EscortStartHoldReason(/*self_is_leader=*/true,
                                  {{.dist_to_npc_yd = 45.0f}}),
            "");
  EXPECT_NE(EscortStartHoldReason(/*self_is_leader=*/true,
                                  {{.alive = false, .dist_to_npc_yd = 5.0f}}),
            "");
  EXPECT_NE(EscortStartHoldReason(
                /*self_is_leader=*/true,
                {{.dist_to_npc_yd = 5.0f, .missing_prereq = true}}),
            "");
  EXPECT_EQ(
      EscortStartHoldReason(/*self_is_leader=*/true,
                            {{.dist_to_npc_yd = 8.0f},
                             {.online = false, .dist_to_npc_yd = 999.0f}}),
      "");
}

TEST(OptionsTest, SpecLfgRoleChoicesMatchTalentSpec) {
  // Shadow priest (class 5, tab 2) -> DPS only; Holy (tab 1) -> Healer only.
  const auto spriest = SpecLfgRoleChoices(5, 2, /*has_talents=*/true, 20);
  ASSERT_EQ(spriest.size(), 1u);
  EXPECT_EQ(spriest[0].mask, kLfgRoleDamage);
  const auto hpriest = SpecLfgRoleChoices(5, 1, /*has_talents=*/true, 20);
  ASSERT_EQ(hpriest.size(), 1u);
  EXPECT_EQ(hpriest[0].mask, kLfgRoleHealer);

  // Prot warrior (class 1, tab 2) -> Tank; Arms (tab 0) -> DPS.
  EXPECT_EQ(SpecLfgRoleChoices(1, 2, true, 20)[0].mask, kLfgRoleTank);
  EXPECT_EQ(SpecLfgRoleChoices(1, 0, true, 20)[0].mask, kLfgRoleDamage);

  // Ret paladin (class 2, tab 2) never queues as tank or healer.
  const auto ret = SpecLfgRoleChoices(2, 2, true, 40);
  ASSERT_EQ(ret.size(), 1u);
  EXPECT_EQ(ret[0].mask, kLfgRoleDamage);

  // Untalented paladin falls back to all three class roles.
  EXPECT_EQ(SpecLfgRoleChoices(2, 0, /*has_talents=*/false, 20).size(), 3u);
}

TEST(OptionsTest, CollectTauntActionsOffersValidTargetsForGroupedTanksOnly) {
  pb::Snapshot snap;
  snap.bot = "Offtank";
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.self.unit.name = "Offtank";
  snap.base.self.unit.group.emplace().role = pb::TANK;
  snap.ext.party.emplace().leader = "Hele";

  pb::Unit* boss = AddUnit(&snap, "Deathbringer Saurfang", "0x1", 15.0f);
  boss->classification = pb::Unit::WORLD_BOSS;
  boss->target = "MainTank";

  pb::Unit* loose_near = AddUnit(&snap, "Blood Beast", "0x2", 10.0f);
  loose_near->target = "Hele";

  pb::Unit* loose_far = AddUnit(&snap, "Ghoul", "0x3", 25.0f);
  loose_far->target = "Mage1";

  // Invalid targets that must never be offered:
  pb::Unit* on_me = AddUnit(&snap, "My Mob", "0x4", 4.0f);
  on_me->target = "Offtank";
  on_me->targeting_me = true;

  pb::Unit* immune = AddUnit(&snap, "Immune Add", "0x5", 5.0f);
  immune->target = "Hele";
  immune->taunt_immune = true;

  pb::Unit* sheep = AddUnit(&snap, "Sheeped Add", "0x6", 6.0f);
  sheep->target = "Hele";
  sheep->crowd_control.emplace().name = "Polymorph";

  AddUnit(&snap, "Idle Mob", "0x7", 7.0f);  // Empty target (not in combat).

  pb::Unit* too_far = AddUnit(&snap, "Distant Add", "0x8", 45.0f);
  too_far->target = "Hele";

  std::vector<pb::OptionFacts_Action> out;
  CollectTauntActions(snap, /*taunt_spell_id=*/355, /*not_tank=*/false, &out);
  ASSERT_EQ(out.size(), 2u);
  // Boss comes first, then nearest valid non-boss; capped at 2.
  EXPECT_EQ(out[0].guid, "taunt:355:0x1");
  EXPECT_EQ(out[0].label, "Taunt Deathbringer Saurfang");
  EXPECT_EQ(out[1].guid, "taunt:355:0x2");
  EXPECT_EQ(out[1].label, "Taunt Blood Beast");

  // Adversarial counter-test 1: non-tank (DPS / Healer) never gets Taunt.
  pb::Snapshot dps = snap;
  dps.base.self.unit.group->role = pb::DPS;
  out.clear();
  CollectTauntActions(dps, 355, false, &out);
  EXPECT_TRUE(out.empty());

  // Adversarial counter-test 2: solo player never gets Taunt.
  pb::Snapshot solo = snap;
  solo.ext.party.reset();
  solo.base.self.unit.group.reset();
  out.clear();
  CollectTauntActions(solo, 355, false, &out);
  EXPECT_TRUE(out.empty());

  // Adversarial counter-test 3: `not_tank` policy, missing spell, or dead bot
  // never gets Taunt.
  out.clear();
  CollectTauntActions(snap, 355, /*not_tank=*/true, &out);
  EXPECT_TRUE(out.empty());
  CollectTauntActions(snap, /*taunt_spell_id=*/0, false, &out);
  EXPECT_TRUE(out.empty());
  pb::Snapshot dead = snap;
  dead.base.self.state = pb::Self::DEAD_BODY;
  CollectTauntActions(dead, 355, false, &out);
  EXPECT_TRUE(out.empty());

  // Mob hitting healer at 30 yd (between 20 yd nameplate range and 40 yd
  // group-combat detail range) gets a Taunt option once nameplate detail is
  // populated.
  pb::Snapshot mid_range;
  mid_range.base.self.unit.name = "Tank";
  mid_range.base.self.unit.group.emplace().role = pb::TANK;
  mid_range.ext.party.emplace().leader = "Hele";
  pb::Unit* on_healer = AddUnit(&mid_range, "Defias Pillager", "0x30", 30.0f);
  on_healer->detail = pb::Unit::NAMEPLATE;
  on_healer->target = "Hele";
  out.clear();
  CollectTauntActions(mid_range, 355, false, &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].guid, "taunt:355:0x30");
  EXPECT_EQ(out[0].label, "Taunt Defias Pillager");
}

TEST(OptionsTest, LighterCombatSnapshotPreservesP0CombatFactsAndOptions) {
  // Build a 25-player raid combat snapshot with non-combat fields omitted
  // (empty ext.quest_tracker, empty member gp.quests, no food/drink, no
  // talent_trees, no npc_actions) and verify visibility + P0 combat options.
  pb::Snapshot snap;
  snap.base.self.unit.name = "Offtank";
  snap.base.self.unit.guid = "0x100";
  snap.base.self.unit.kind = pb::Unit::PLAYER;
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.self.in_combat = true;
  snap.base.self.health = 18000;
  snap.base.self.health_max = 30000;
  pb::GroupPart& self_gp = snap.base.self.unit.group.emplace();
  self_gp.role = pb::TANK;
  self_gp.main_tank = true;
  self_gp.online = true;
  self_gp.in_range = true;

  pb::Party& party = snap.ext.party.emplace();
  party.leader = "MainTank";
  pb::Raid& raid = snap.ext.raid.emplace();
  raid.main_tanks.push_back("MainTank");
  for (int i = 0; i < 24; ++i) {
    pb::Unit& m = party.members.emplace_back();
    m.guid = "0x2" + std::to_string(i);
    m.name = i == 0 ? "MainTank" : ("Raider" + std::to_string(i));
    m.kind = pb::Unit::PLAYER;
    m.detail = pb::Unit::NAMEPLATE;
    m.health_pct = 85.0f;
    pb::GroupPart& gp = m.group.emplace();
    gp.subgroup = static_cast<uint32_t>(i / 5) + 1;
    gp.online = true;
    gp.in_range = true;
    gp.role = i == 0 ? pb::TANK : pb::DPS;
    // Omitted in lighter combat snapshots:
    EXPECT_TRUE(gp.quests.empty());
  }
  // Tank debuff (P0 combat fact for tank swaps) is still present.
  pb::Aura& debuff = party.members[0].auras.emplace_back();
  debuff.id = 72410;
  debuff.name = "Rune of Blood";
  debuff.harmful = true;
  debuff.remaining_s = 15.0f;

  // Boss + add + ground hazard + action bar spells.
  {
    pb::Unit* boss = AddUnit(&snap, "Deathbringer Saurfang", "0x300", 12.0f);
    boss->classification = pb::Unit::WORLD_BOSS;
    boss->target = "MainTank";
  }
  {
    pb::Unit* add = AddUnit(&snap, "Blood Beast", "0x301", 8.0f);
    add->target = "Raider3";
  }

  pb::Area& hazard = snap.base.nearby.areas.emplace_back();
  hazard.guid = "trigger:0x400";
  hazard.name = "Defile";
  hazard.radius_yd = 10.0f;
  hazard.inside_me = true;
  hazard.hostile = true;

  pb::Combat& combat = snap.ext.combat.emplace();
  combat.attackers.push_back(snap.base.nearby.units[0]);
  snap.ext.target.emplace().target = snap.base.nearby.units[0];
  pb::Spell& sp = snap.ext.action_bars.emplace().buttons.emplace_back();
  sp.id = 355;
  sp.name = "Taunt";
  sp.ability.emplace().usable = true;
  // Non-combat quest tracker omitted at 4 Hz in combat.
  EXPECT_TRUE(snap.ext.quest_tracker.empty());

  EXPECT_TRUE(pb::ValidateSnapshotVisibility(snap).empty());

  OptionFacts facts;
  facts.usable_items.push_back({.entry = 33447,
                                .name = "Runic Healing Potion",
                                .out_of_combat_only = false,
                                .detail = "restores health"});
  facts.usable_items.push_back({.entry = 6948,
                                .name = "Hearthstone",
                                .out_of_combat_only = true,
                                .detail = "out of combat"});
  CollectTauntActions(snap, /*taunt_spell_id=*/355, /*not_tank=*/false,
                      &facts.group_and_upkeep_actions);

  AddOptions(&snap, facts);
  auto has = [&](std::string_view sub) {
    return std::any_of(snap.options.begin(), snap.options.end(),
                       [&](const pb::Option& o) {
                         return o.text.find(sub) != std::string::npos;
                       });
  };
  // All P0 combat options remain present:
  EXPECT_TRUE(has("Attack Deathbringer Saurfang"));
  EXPECT_TRUE(has("Attack Blood Beast"));
  EXPECT_TRUE(has("Use Runic Healing Potion"));
  EXPECT_TRUE(has("Taunt Deathbringer Saurfang"));
  EXPECT_TRUE(has("Keep doing what I'm doing"));
  // Non-combat options stay absent:
  EXPECT_FALSE(has("Hearthstone"));
  EXPECT_FALSE(has("Rest"));
  EXPECT_FALSE(has("Leave group"));
}

// Dialogue NPCs with `kNpcFlagHasDialogue` offer `Talk to` out of combat
// (unlike plain flavor-only `kNpcFlagGossip`), and explicit `npc_gossip:*`
// actions replace the generic `Talk to` once open.
TEST(OptionsTest, DialogueFlagOffersTalkAndGossipActionsReplaceItOutOfCombat) {
  pb::Snapshot snap;
  AddUnit(&snap, "Brann Bronzebeard", "0x10", 4, pb::Unit::FRIENDLY);
  AddUnit(&snap, "Stormwind Citizen", "0x11", 4, pb::Unit::FRIENDLY);

  OptionFacts facts;
  facts.npcs.push_back({"0x10", kNpcFlagGossip | kNpcFlagHasDialogue});
  facts.npcs.push_back({"0x11", kNpcFlagGossip});  // Plain gossip: skipped.

  AddOptions(&snap, facts);
  EXPECT_EQ(Texts(snap),
            (std::vector<std::string>{"Talk to Brann Bronzebeard",
                                      "Keep doing what I'm doing"}));

  // Once gossip is open, explicit dialogue choices replace `Talk to`.
  facts.npc_actions.push_back({"npc_gossip:0x10:99:0", "0x10",
                               "Say to Brann Bronzebeard: \"We are ready.\""});
  snap.options.clear();
  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  EXPECT_EQ(Texts(snap), (std::vector<std::string>{
                             "Say to Brann Bronzebeard: \"We are ready.\"",
                             "Keep doing what I'm doing"}));
  const auto* npc_act = std::get_if<NpcAction>(&targets[0].action);
  ASSERT_NE(npc_act, nullptr);
  EXPECT_EQ(npc_act->op, NpcAction::Op::kGossipSelect);
  EXPECT_EQ(npc_act->arg1, 99u);
  EXPECT_EQ(npc_act->arg2, 0u);

  // Adversarial counter-test: P0 combat suppresses both Talk and gossip
  // options.
  snap.options.clear();
  snap.base.self.in_combat = true;
  AddOptions(&snap, facts);
  EXPECT_EQ(Texts(snap), std::vector<std::string>{"Keep doing what I'm doing"});
}

// Adversarial counter-test: full-bag destroy options are offered alive out of
// combat, and suppressed in combat or while dead (P0 combat priority).
TEST(OptionsTest, DestroyItemOptionsSuppressedInCombatAndWhenDead) {
  OptionFacts facts;
  facts.group_and_upkeep_actions.push_back(
      {"destroy:255:23:4865", "Destroy Ruined Pelt (grey, 5c)"});

  pb::Snapshot alive_ooc;
  const std::vector<OptionTarget> ooc_targets = AddOptions(&alive_ooc, facts);
  EXPECT_EQ(Texts(alive_ooc),
            (std::vector<std::string>{"Destroy Ruined Pelt (grey, 5c)",
                                      "Keep doing what I'm doing"}));
  EXPECT_TRUE(std::holds_alternative<DestroyItemAction>(ooc_targets[0].action));

  pb::Snapshot in_combat;
  in_combat.base.self.in_combat = true;
  AddOptions(&in_combat, facts);
  EXPECT_EQ(Texts(in_combat),
            std::vector<std::string>{"Keep doing what I'm doing"});

  pb::Snapshot dead;
  dead.base.self.state = pb::Self::DEAD_BODY;
  AddOptions(&dead, facts);
  EXPECT_EQ(Texts(dead), (std::vector<std::string>{
                             "Release spirit", "Keep doing what I'm doing"}));
}

}  // namespace
}  // namespace llm_brain
