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

TEST(OptionsTest, NeutralCanBeAttackedFriendlyCannot) {
  pb::Snapshot snapshot;
  AddUnit(&snapshot, "Mottled Boar", "0x1", 10, pb::Unit::NEUTRAL);
  AddUnit(&snapshot, "Gornek", "0x2", 5, pb::Unit::FRIENDLY);
  AddOptions(&snapshot);
  EXPECT_EQ(Texts(snapshot),
            (std::vector<std::string>{"Attack Mottled Boar",
                                      "Keep doing what I'm doing"}));
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
