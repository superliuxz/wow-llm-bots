// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/options.h"

#include <cstdint>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "proto/snapshot.h"

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
    EXPECT_EQ(Texts(snapshot),
              std::vector<std::string>{"Keep doing what I'm doing"});
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

  EXPECT_EQ(targets[2].kind, OptionTarget::Kind::kGoTo);
  EXPECT_EQ(targets[2].guid, "place:razor_hill");
  EXPECT_EQ(targets[2].name, "Razor Hill");
  ASSERT_TRUE(targets[2].pos.has_value());
  EXPECT_FLOAT_EQ(targets[2].pos->x, 315.0f);
  EXPECT_EQ(snapshot.options[2].guid, "place:razor_hill");
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

}  // namespace
}  // namespace llm_brain
