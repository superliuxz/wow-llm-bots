// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "proto/snapshot.pb.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

pb::Unit* AddUnit(pb::Snapshot* snapshot,
                  const std::string& name,
                  const std::string& guid,
                  float dist_yd,
                  pb::Unit::Reaction reaction = pb::Unit::HOSTILE) {
  pb::Unit* u = snapshot->mutable_base()->mutable_nearby()->add_units();
  u->set_name(name);
  u->set_guid(guid);
  u->set_kind(pb::Unit::NPC);
  u->set_reaction(reaction);
  u->mutable_rel()->set_dist_yd(dist_yd);
  return u;
}

std::vector<std::string> Texts(const pb::Snapshot& snapshot) {
  std::vector<std::string> texts;
  for (const pb::Option& option : snapshot.options()) {
    texts.push_back(std::string(option.text()));
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
  AddUnit(&snapshot, "Scorpid Worker", "0x1", 4)->set_dead(true);
  pb::Unit* lootable = AddUnit(&snapshot, "Scorpid Worker", "0x2", 6);
  lootable->set_dead(true);
  lootable->set_lootable(true);
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
  AddUnit(&snapshot, "Someone", "0x1", 10)->set_kind(pb::Unit::PLAYER);
  AddUnit(&snapshot, "", "0x2", 60);  // Model only: no name to click.
  AddUnit(&snapshot, "No Guid", "", 10);
  AddOptions(&snapshot);
  EXPECT_EQ(Texts(snapshot),
            std::vector<std::string>{"Keep doing what I'm doing"});
}

TEST(OptionHistoryTest, FindsAnOptionOfARecentSnapshot) {
  OptionHistory history(4);
  history.Add(7, 1000,
              {{OptionTarget::Kind::kAttack, "0x1", "Boar"},
               {OptionTarget::Kind::kKeep, "", ""}});
  history.Add(8, 2000, {{OptionTarget::Kind::kKeep, "", ""}});
  const OptionHistory::Lookup found = history.Find(7, 1);
  ASSERT_NE(found.target, nullptr);
  EXPECT_EQ(found.target->guid, "0x1");
  EXPECT_EQ(found.snapshot_time_ms, 1000);
  EXPECT_TRUE(found.error.empty());
  EXPECT_EQ(history.Find(8, 1).target->kind, OptionTarget::Kind::kKeep);
}

TEST(OptionHistoryTest, OptionNumbersAreOneBased) {
  OptionHistory history(4);
  history.Add(1, 0, {{OptionTarget::Kind::kKeep, "", ""}});
  EXPECT_EQ(history.Find(1, 0).target, nullptr);
  EXPECT_EQ(history.Find(1, 0).error, "no option 0 in snapshot 1");
  EXPECT_EQ(history.Find(1, 2).error, "no option 2 in snapshot 1");
}

TEST(OptionHistoryTest, ForgetsTheOldest) {
  OptionHistory history(2);
  for (uint32_t seq = 1; seq <= 3; ++seq) {
    history.Add(seq, seq, {{OptionTarget::Kind::kKeep, "", ""}});
  }
  EXPECT_EQ(history.Find(1, 1).target, nullptr);
  EXPECT_EQ(history.Find(1, 1).error, "snapshot 1 is too old or unknown");
  EXPECT_NE(history.Find(2, 1).target, nullptr);
  EXPECT_NE(history.Find(3, 1).target, nullptr);
}

}  // namespace
}  // namespace llm_brain
