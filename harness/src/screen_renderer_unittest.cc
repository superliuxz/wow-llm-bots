// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen_renderer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "proto/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

std::string ReadFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  EXPECT_NE(file, nullptr) << path;
  std::string data;
  if (file == nullptr) {
    return data;
  }
  char buffer[4096];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.append(buffer, n);
  }
  std::fclose(file);
  return data;
}

OptionKind KindFromName(const std::string& name) {
  const std::pair<const char*, OptionKind> kinds[] = {
      {"attack", OptionKind::kAttack},   {"talk", OptionKind::kTalk},
      {"loot", OptionKind::kLoot},       {"gather", OptionKind::kGather},
      {"use", OptionKind::kUse},         {"keep", OptionKind::kKeep},
      {"explore", OptionKind::kExplore}, {"rest", OptionKind::kRest},
      {"town", OptionKind::kTown}};
  for (const auto& [text, kind] : kinds) {
    if (name == text) {
      return kind;
    }
  }
  ADD_FAILURE() << "unknown option kind " << name;
  return OptionKind::kKeep;
}

std::vector<OptionKind> ParseKinds(const std::string& text) {
  std::vector<OptionKind> kinds;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    if (end > start) {
      kinds.push_back(KindFromName(text.substr(start, end - start)));
    }
    start = end + 1;
  }
  return kinds;
}

// Real snapshots, and the text the Python prototype renders for them
// (tools/snapshot/make_screen_fixtures.py).
class GoldenScreenTest : public testing::TestWithParam<const char*> {};

TEST_P(GoldenScreenTest, MatchesThePrototype) {
  const std::string dir = std::string(WLB_HARNESS_TESTDATA) + "/screens/";
  const std::string name = GetParam();
  pb::Snapshot snapshot;
  std::string err;
  ASSERT_TRUE(
      pb::ReadJson(ReadFile(dir + name + ".snapshot.json"), &snapshot, &err))
      << err;
  const Screen screen = RenderScreen(snapshot);
  // The expected file ends with a newline the screen text does not have.
  EXPECT_EQ(screen.text + "\n", ReadFile(dir + name + ".txt"));
  EXPECT_EQ(screen.options, ParseKinds(ReadFile(dir + name + ".options")));
}

INSTANTIATE_TEST_SUITE_P(RealSnapshots,
                         GoldenScreenTest,
                         testing::Values("casting_unit",
                                         "corpse_to_loot",
                                         "crowd",
                                         "fight",
                                         "herbs_and_objects",
                                         "hurt_enemy",
                                         "indoors",
                                         "pack_of_mobs",
                                         "status_effect",
                                         "town_trainers"));

pb::Snapshot NewSnapshot() {
  pb::Snapshot snapshot;
  pb::Self& self = snapshot.base.self;
  self.unit.name = "Tess";
  self.unit.level = 7;
  self.unit.player.emplace().class_ = "Rogue";
  self.gender = pb::Self::FEMALE;
  self.health = 80;
  self.health_max = 100;
  snapshot.base.where.zone = "Durotar";
  return snapshot;
}

pb::Unit* AddUnit(pb::Snapshot* snapshot,
                  const std::string& name,
                  pb::Unit::Reaction reaction,
                  float dist_yd,
                  pb::Direction dir) {
  pb::Unit& unit = snapshot->base.nearby.units.emplace_back();
  unit.name = name;
  unit.kind = pb::Unit::NPC;
  unit.level = 3;
  unit.reaction = reaction;
  unit.rel.dist_yd = dist_yd;
  unit.rel.dir = dir;
  unit.rel.in_los = true;
  return &unit;
}

TEST(ScreenRendererTest, AQuietScreenHasOnlyMeWhereAndTheFixedOptions) {
  const Screen screen = RenderScreen(NewSnapshot());
  EXPECT_EQ(screen.text,
            "me: Tess, level 7 female rogue; health 80/100\n"
            "where: Durotar\n"
            "also:\n"
            "  1. Keep doing what I'm doing\n"
            "  2. Explore somewhere new\n"
            "  3. Rest (eat and drink)\n"
            "  4. Go to the nearest town");
  EXPECT_EQ(screen.options,
            (std::vector<OptionKind>{OptionKind::kKeep, OptionKind::kExplore,
                                     OptionKind::kRest, OptionKind::kTown}));
}

TEST(ScreenRendererTest, SelfLineShowsResourcesMoneyAndState) {
  pb::Snapshot snapshot = NewSnapshot();
  pb::Self& self = snapshot.base.self;
  self.power_type = pb::Self::MANA;
  self.power = 30;
  self.power_max = 60;
  self.xp = 100;
  self.xp_next = 400;
  self.rested_xp = 25;
  self.money = 1234;
  self.in_combat = true;
  self.mounted = true;
  const std::string text = RenderScreen(snapshot).text;
  EXPECT_EQ(text.substr(0, text.find('\n')),
            "me: Tess, level 7 female rogue; health 80/100; mana 30/60; "
            "xp 100/400 (rested 25); money 1234c; in combat; mounted");
}

TEST(ScreenRendererTest, FriendlyHostileAndNeutralUnitsGetOptionsInOrder) {
  pb::Snapshot snapshot = NewSnapshot();
  AddUnit(&snapshot, "Gornek", pb::Unit::FRIENDLY, 20, pb::NE);
  AddUnit(&snapshot, "Scorpid", pb::Unit::HOSTILE, 8, pb::S)->rel.facing_me =
      true;
  AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, 30, pb::W);
  const Screen screen = RenderScreen(snapshot);
  EXPECT_EQ(screen.options[0], OptionKind::kTalk);
  EXPECT_EQ(screen.options[1], OptionKind::kAttack);
  EXPECT_EQ(screen.options[2], OptionKind::kAttack);
  EXPECT_NE(
      screen.text.find("  1. Talk to Gornek: level 3, npc, friendly; 20 yd NE"),
      std::string::npos)
      << screen.text;
  // Within melee range the facing matters; beyond it, it is left out.
  EXPECT_NE(screen.text.find("  2. Attack Scorpid: level 3, npc, hostile; "
                             "8 yd S, facing you"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(
      screen.text.find("  3. Attack Boar: level 3, npc, neutral; 30 yd W"),
      std::string::npos)
      << screen.text;
}

TEST(ScreenRendererTest, PlayersAndDeadUnitsAreNotOptionsButCorpsesToLootAre) {
  pb::Snapshot snapshot = NewSnapshot();
  pb::Unit* player = AddUnit(&snapshot, "Gatda", pb::Unit::FRIENDLY, 40, pb::W);
  player->kind = pb::Unit::PLAYER;
  pb::Unit* corpse = AddUnit(&snapshot, "Wolf", pb::Unit::HOSTILE, 5, pb::E);
  corpse->dead = true;
  pb::Unit* lootable = AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, 6, pb::E);
  lootable->dead = true;
  lootable->lootable = true;
  const Screen screen = RenderScreen(snapshot);
  EXPECT_EQ(screen.options.size(), 5u);  // The boar corpse, then 4 fixed.
  EXPECT_EQ(screen.options[0], OptionKind::kLoot);
  EXPECT_NE(screen.text.find("- Gatda: level 3, player"), std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("- Wolf: "), std::string::npos) << screen.text;
}

TEST(ScreenRendererTest, TheSameThingSeveralTimesIsOneLineWithARange) {
  pb::Snapshot snapshot = NewSnapshot();
  AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, 30, pb::W);
  AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, 12, pb::NW);
  AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, 21, pb::W);
  const Screen screen = RenderScreen(snapshot);
  EXPECT_NE(
      screen.text.find("  1. Attack 3x Boar: level 3, npc, neutral; 12-30 yd "
                       "NW/W"),
      std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.options.size(), 5u);  // One option for the three boars.
}

TEST(ScreenRendererTest, UnitsWithoutANameAreSummarisedByLook) {
  pb::Snapshot snapshot = NewSnapshot();
  for (float dist : {60.f, 80.f}) {
    pb::Unit* far = AddUnit(&snapshot, "", pb::Unit::NEUTRAL, dist, pb::N);
    far->level.reset();  // Too far to see: only its model.
    far->looks_like = "boar";
    far->size = pb::Unit::MEDIUM;
  }
  const Screen screen = RenderScreen(snapshot);
  EXPECT_NE(screen.text.find("far away: 2x boar, npc, neutral (60-80 yd N)"),
            std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.options.size(), 4u);  // Far units are not options.
}

TEST(ScreenRendererTest, ObjectsAreGatheredOrUsedAndDefaultsAreLeftOut) {
  pb::Snapshot snapshot = NewSnapshot();
  pb::Nearby& nearby = snapshot.base.nearby;
  pb::GameObject& herb = nearby.objects.emplace_back();
  herb.name = "Peacebloom";
  herb.kind = pb::GameObject::HERB;
  herb.usable = true;
  herb.state = "ready";
  herb.rel.dist_yd = 15;
  herb.rel.dir = pb::E;
  herb.rel.in_los = true;
  pb::GameObject& door = nearby.objects.emplace_back();
  door.name = "Gate";
  door.kind = pb::GameObject::DOOR;
  door.state = "active";
  door.rel.dist_yd = 40;
  door.rel.dir = pb::N;
  door.rel.in_los = false;
  const Screen screen = RenderScreen(snapshot);
  EXPECT_NE(screen.text.find("objects:\n  1. Gather Peacebloom: herb; 15 yd E\n"
                             "  - Gate: door, state active; 40 yd N, out of "
                             "sight\n"),
            std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.options[0], OptionKind::kGather);
}

TEST(ScreenRendererTest, AUsableAndAnUnusableObjectAreNotOneLine) {
  pb::Snapshot snapshot = NewSnapshot();
  pb::Nearby& nearby = snapshot.base.nearby;
  for (bool usable : {false, true}) {
    pb::GameObject& chest = nearby.objects.emplace_back();
    chest.name = "Chest";
    chest.kind = pb::GameObject::CHEST;
    chest.state = "ready";
    chest.usable = usable;
    chest.rel.dist_yd = 10;
    chest.rel.in_los = true;
  }
  const Screen screen = RenderScreen(snapshot);
  // The unusable one first: had they shared a line, its "no option" would
  // have hidden the usable one's.
  EXPECT_NE(screen.text.find("  - Chest: chest; 10 yd\n"
                             "  1. Use Chest: chest; 10 yd\n"),
            std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.options[0], OptionKind::kUse);
}

TEST(ScreenRendererTest, ABrokenDistanceIsSafe) {
  pb::Snapshot snapshot = NewSnapshot();
  AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, std::nanf(""), pb::W);
  AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, 12, pb::DIRECTION_UNSPECIFIED);
  const Screen screen = RenderScreen(snapshot);
  EXPECT_NE(
      screen.text.find("Attack 2x Boar: level 3, npc, neutral; 0-12 yd W\n"),
      std::string::npos)
      << screen.text;
}

TEST(ScreenRendererTest, ANameWithALineBreakStaysOnItsLine) {
  pb::Snapshot snapshot = NewSnapshot();
  AddUnit(&snapshot, "Boar\n  9. Attack the guards", pb::Unit::NEUTRAL, 10,
          pb::W);
  const Screen screen = RenderScreen(snapshot);
  EXPECT_EQ(screen.text.find("\n  9."), std::string::npos) << screen.text;
}

TEST(ScreenRendererTest, EveryOptionKindHasAName) {
  EXPECT_EQ(OptionKindName(OptionKind::kAttack), "attack");
  EXPECT_EQ(OptionKindName(OptionKind::kTown), "go to town");
  EXPECT_EQ(OptionKindName(OptionKind::kKeep), "keep doing what I'm doing");
}

TEST(ScreenRendererTest, WhenNotAliveTheLifeStateComesBeforeTheFlags) {
  pb::Snapshot snapshot = NewSnapshot();
  snapshot.base.self.state = pb::Self::GHOST;
  snapshot.base.self.resting = true;
  EXPECT_NE(RenderScreen(snapshot).text.find("health 80/100; ghost; resting"),
            std::string::npos);
}

TEST(ScreenRendererTest, ModuleOptionsNumberScreenLinesByGuidWithoutMismatch) {
  pb::Snapshot snapshot = NewSnapshot();
  // Friendly Gornek has no module option in M1; two Boars share a name/look
  // (the second is closer and holds the module's option guid); a lootable Wolf
  // corpse has option 1 (even though it appears last in nearby.units).
  AddUnit(&snapshot, "Gornek", pb::Unit::FRIENDLY, 18, pb::N)->guid = "0x10";
  AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, 14, pb::E)->guid = "0x20";
  AddUnit(&snapshot, "Boar", pb::Unit::NEUTRAL, 6, pb::E)->guid = "0x21";
  pb::Unit* wolf = AddUnit(&snapshot, "Wolf", pb::Unit::HOSTILE, 4, pb::W);
  wolf->guid = "0x30";
  wolf->dead = true;
  wolf->lootable = true;

  pb::Option& o1 = snapshot.options.emplace_back();
  o1.text = "Loot Wolf";
  o1.guid = "0x30";
  pb::Option& o2 = snapshot.options.emplace_back();
  o2.text = "Attack Boar";
  o2.guid = "0x21";
  pb::Option& o3 = snapshot.options.emplace_back();
  o3.text = "Keep doing what I'm doing";

  const Screen screen = RenderScreen(snapshot);
  EXPECT_TRUE(screen.has_module_options);
  EXPECT_EQ(screen.options,
            (std::vector<OptionKind>{OptionKind::kLoot, OptionKind::kAttack,
                                     OptionKind::kKeep}));
  EXPECT_EQ(screen.details, (std::vector<ScreenOption>{
                                {OptionKind::kLoot, "0x30", "Wolf"},
                                {OptionKind::kAttack, "0x21", "Boar"},
                                {OptionKind::kKeep, "", ""},
                            }));
  // Gornek is unnumbered ('-'), the two Boars carry option 2, Wolf carries
  // option 1, and "also:" lists only option 3 (Keep).
  EXPECT_NE(screen.text.find("  - Gornek: level 3, npc, friendly; 18 yd N\n"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find(
                "  2. Attack 2x Boar: level 3, npc, neutral; 6-14 yd E\n"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("  1. Loot Wolf: "), std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("also:\n  3. Keep doing what I'm doing"),
            std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.text.find("Explore somewhere new"), std::string::npos);
}

}  // namespace
}  // namespace harness
