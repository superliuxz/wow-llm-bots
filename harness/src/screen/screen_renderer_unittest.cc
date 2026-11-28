// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/screen_renderer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

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

// Real snapshots and their rendered text. The Python prototype these were
// first checked against is retired: the goldens are now regression goldens,
// written by C++ itself (`harness render-screen --rewrite`), so they catch
// changes, not mistakes. Read every golden diff before committing it.
class GoldenScreenTest : public testing::TestWithParam<const char*> {};

TEST_P(GoldenScreenTest, MatchesTheReviewedGolden) {
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
  o1.kind = pb::Option::LOOT;
  o1.name = "Wolf";
  pb::Option& o2 = snapshot.options.emplace_back();
  o2.text = "Attack Boar";
  o2.guid = "0x21";
  o2.kind = pb::Option::ATTACK;
  o2.name = "Boar";
  pb::Option& o3 = snapshot.options.emplace_back();
  o3.text = "Keep doing what I'm doing";
  o3.kind = pb::Option::KEEP;

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

// A snapshot with one named boar around me.
pb::Snapshot BoarAround(const std::string& guid) {
  pb::Snapshot snap;
  snap.bot = "Testpriest";
  snap.base.self.state = pb::Self::ALIVE;
  pb::Unit& boar = snap.base.nearby.units.emplace_back();
  boar.guid = guid;
  boar.name = "Mottled Boar";
  boar.reaction = pb::Unit::HOSTILE;
  boar.rel.dist_yd = 8.0f;
  return snap;
}

TEST(ScreenRendererTest, TargetAroundMeAddsOnlyHealthTargetOfTargetAndThreat) {
  pb::Snapshot snap = BoarAround("0x1");
  pb::Target& frames = snap.ext.target.emplace();
  pb::Unit& target = frames.target.emplace(snap.base.nearby.units[0]);
  target.health_pct = 45.0f;
  frames.target_of_target = "Testpriest";
  frames.my_threat_pct = 100.0f;
  const Screen screen = RenderScreen(snap);
  EXPECT_NE(
      screen.text.find(
          "target: Mottled Boar: health 45%; targets you; my threat 100%"),
      std::string::npos)
      << screen.text;
}

TEST(ScreenRendererTest, TargetAndFocusNotAroundMeAreWrittenInFull) {
  pb::Snapshot snap = BoarAround("0x1");
  pb::Target& frames = snap.ext.target.emplace();
  pb::Unit& focus = frames.focus.emplace();
  focus.guid = "0x9";
  focus.name = "Hogger";
  focus.level = 11;
  focus.rel.dist_yd = 60.0f;
  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("focus: Hogger: level 11"), std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.text.find("target:"), std::string::npos) << screen.text;
}

TEST(ScreenRendererTest, CombatCountsAttackersAndSkipsThreatOnTheTargetLine) {
  pb::Snapshot snap = BoarAround("0x1");
  pb::Target& frames = snap.ext.target.emplace();
  frames.target.emplace(snap.base.nearby.units[0]);
  frames.my_threat_pct = 80.0f;
  pb::Combat& combat = snap.ext.combat.emplace();
  combat.attackers.emplace_back().name = "Kobold Vermin";
  combat.attackers.emplace_back().name = "Kobold Vermin";
  combat.attackers.emplace_back().name = "Mottled Boar";
  pb::Combat::Threat& on_boar = combat.threat.emplace_back();
  on_boar.unit = "Mottled Boar";
  on_boar.my_threat_pct = 80.0f;
  pb::Combat::Threat& on_kobold = combat.threat.emplace_back();
  on_kobold.unit = "Kobold Vermin";
  on_kobold.my_threat_pct = 40.0f;
  on_kobold.top = "Gatda";
  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("combat: attacked by 2x Kobold Vermin, Mottled "
                             "Boar; my threat Kobold Vermin 40% (top Gatda)"),
            std::string::npos)
      << screen.text;
}

TEST(ScreenRendererTest, PartyListsMembersAndQuestTrackerGroupsObjectives) {
  pb::Snapshot snap = BoarAround("0x1");
  pb::Party& party = snap.ext.party.emplace();
  party.leader = "Testpriest";
  party.loot_method = "GROUP_LOOT";
  pb::Unit& tank = party.members.emplace_back();
  tank.name = "Gatda";
  tank.health_pct = 70.0f;
  tank.group.emplace().role = pb::TANK;
  tank.group->online = true;
  tank.group->in_range = true;

  for (const char* title : {"Kobold Vermin slain", "Kobold Worker slain"}) {
    pb::Objective& o = snap.ext.quest_tracker.emplace_back();
    o.title = title;
    o.progress.emplace().have = 3;
    o.progress->need = 8;
    pb::QuestPart& q = o.quest.emplace();
    q.quest_id = 7;
    q.quest = "Kobold Camp Cleanup";
    q.level = 2;
  }
  snap.ext.quest_tracker.emplace_back().title = "sell junk";

  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("party: leader you; loot group loot\n"
                             "  - Gatda: tank, health 70%"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("quests:\n"
                             "  Kobold Camp Cleanup (level 2)\n"
                             "    - Kobold Vermin slain 3/8\n"
                             "    - Kobold Worker slain 3/8\n"
                             "goals:\n"
                             "  - sell junk"),
            std::string::npos)
      << screen.text;
}

TEST(ScreenRendererTest, QuestTrackerShowsPartyQuestOverlap) {
  pb::Snapshot snap = BoarAround("0x1");
  for (const char* title : {"Kobold Vermin slain", "Kobold Worker slain"}) {
    pb::Objective& o = snap.ext.quest_tracker.emplace_back();
    o.title = title;
    o.progress.emplace().have = 1;
    o.progress->need = 8;
    pb::QuestPart& q = o.quest.emplace();
    q.quest_id = 7;
    q.quest = "Kobold Camp Cleanup";
    q.level = 2;
    q.party_members = {"Gatda", "Hele"};
  }
  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find(
                "quests:\n"
                "  Kobold Camp Cleanup (level 2, also on quest: Gatda, Hele)\n"
                "    - Kobold Vermin slain 1/8\n"
                "    - Kobold Worker slain 1/8"),
            std::string::npos)
      << screen.text;

  // Raid-sized overlap caps displayed names while reporting the exact unclipped
  // remainder from `party_member_count` (or `party_members.size()` when 0).
  pb::Snapshot raid_snap = BoarAround("0x1");
  pb::Objective& ro = raid_snap.ext.quest_tracker.emplace_back();
  ro.title = "Boss slain";
  ro.progress.emplace().have = 0;
  ro.progress->need = 1;
  pb::QuestPart& rq = ro.quest.emplace();
  rq.quest_id = 99;
  rq.quest = "Raid Weekly";
  rq.party_members = {"A", "B", "C", "D", "E", "F", "G"};
  const Screen raid_screen = RenderScreen(raid_snap);
  EXPECT_NE(raid_screen.text.find(
                "  Raid Weekly (also on quest: A, B, C, D, E +2 more)\n"),
            std::string::npos)
      << raid_screen.text;

  rq.party_members = {"A", "B", "C", "D", "E"};
  rq.party_member_count = 20;
  const Screen large_raid_screen = RenderScreen(raid_snap);
  EXPECT_NE(large_raid_screen.text.find(
                "  Raid Weekly (also on quest: A, B, C, D, E +15 more)\n"),
            std::string::npos)
      << large_raid_screen.text;
}

TEST(ScreenRendererTest, BagSpaceShowsOnlyWhenLowOrFull) {
  pb::Snapshot snap = BoarAround("0x1");
  snap.base.self.indicators.bag_free_slots = 12;
  EXPECT_EQ(RenderScreen(snap).text.find("bags"), std::string::npos);
  snap.base.self.indicators.bag_free_slots = 3;
  EXPECT_NE(RenderScreen(snap).text.find("bags 3 free"), std::string::npos);
  snap.base.self.indicators.bag_free_slots = 0;
  EXPECT_NE(RenderScreen(snap).text.find("bags full"), std::string::npos);
  snap.base.self.indicators.bag_free_slots.reset();  // Unknown: nothing.
  EXPECT_EQ(RenderScreen(snap).text.find("bags"), std::string::npos);
}

TEST(ScreenRendererTest, ExploreAndGoToModuleOptionsCarryStructuredFields) {
  pb::Snapshot snap = NewSnapshot();
  pb::Option& e1 = snap.options.emplace_back();
  e1.text = "Explore NE, 300 yd (boars, scorpids, level 3-5)";
  e1.guid = "spot:1:-500:-4100";
  e1.kind = pb::Option::EXPLORE;
  e1.name = "boars, scorpids, level 3-5";
  pb::Option& e2 = snap.options.emplace_back();
  e2.text = "Explore S, 80 yd";
  e2.guid = "spot:1:-700:-4200";
  e2.kind = pb::Option::EXPLORE;
  e2.name = "S";
  pb::Option& g1 = snap.options.emplace_back();
  g1.text = "Go to Razor Hill (innkeeper, vendor, repair), 900 yd N";
  g1.guid = "place:razor_hill";
  g1.kind = pb::Option::GO_TO;
  g1.name = "Razor Hill";
  pb::Option& g2 = snap.options.emplace_back();
  g2.text = "Go to Sen'jin Village Graveyard, 250 yd S";
  g2.guid = "place:grave_1";
  g2.kind = pb::Option::GO_TO;
  g2.name = "Sen'jin Village Graveyard";

  const Screen screen = RenderScreen(snap);
  ASSERT_EQ(screen.details.size(), 4u);
  EXPECT_EQ(screen.details[0],
            (ScreenOption{OptionKind::kExplore, "spot:1:-500:-4100",
                          "boars, scorpids, level 3-5"}));
  EXPECT_EQ(screen.details[1],
            (ScreenOption{OptionKind::kExplore, "spot:1:-700:-4200", "S"}));
  EXPECT_EQ(
      screen.details[2],
      (ScreenOption{OptionKind::kTown, "place:razor_hill", "Razor Hill"}));
  EXPECT_EQ(screen.details[3], (ScreenOption{OptionKind::kTown, "place:grave_1",
                                             "Sen'jin Village Graveyard"}));
}

TEST(ScreenRendererTest, OpenPanelsTooltipsAndWhatCanBePointedAt) {
  pb::Snapshot snap;
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.ui.open_panels = {pb::BAGS, pb::QUEST_LOG};
  pb::Bags& bags = snap.ext.bags.emplace();
  bags.free_slots = 13;
  bags.total_slots = 16;
  bags.hearthstone.emplace().ready = true;
  bags.hearthstone->bind = "Razor Hill";
  pb::Item& bread = bags.items.emplace_back();
  bread.id = 4540;
  bread.name = "Tough Hunk of Bread";
  bread.count = 4;
  bread.kind = pb::Item::CONSUMABLE;
  bread.usable = true;
  pb::Item& meat = bags.items.emplace_back();
  meat.id = 769;
  meat.name = "Chunk of Boar Meat";
  meat.quality = pb::Item::POOR;
  // The character sheet isn't open: not shown even if sent.
  snap.ext.character.emplace().equipped.emplace_back().name = "Robe";
  pb::QuestLog::Entry& q = snap.ext.quest_log.emplace().quests.emplace_back();
  q.objective.emplace().quest.emplace().quest_id = 4641;
  q.objective->quest->quest = "Your Place In The World";
  q.objective->quest->level = 1;
  q.description = "Speak with Gornek.";
  q.money = 120;
  pb::Tooltipped& tip = snap.base.tooltips.emplace_back();
  tip.ref.emplace().name = "Tough Hunk of Bread";
  tip.tooltip.emplace().lines = {"Tough Hunk of Bread",
                                 "Use: Restores 61 health over 18 sec.",
                                 "Sell Price: 1c"};
  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("bags (13 of 16 free; hearthstone ready, home "
                             "Razor Hill):\n  Tough Hunk of Bread x4 "
                             "(consumable, usable), Chunk of Boar Meat (gray)"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("quest log:\n  Your Place In The World (level "
                             "1): Speak with Gornek.; money 1s 20c"),
            std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.text.find("Robe"), std::string::npos);
  EXPECT_NE(screen.text.find("pointing at Tough Hunk of Bread: Use: Restores "
                             "61 health over 18 sec.; Sell Price: 1c"),
            std::string::npos)
      << screen.text;
  ASSERT_EQ(screen.hoverables.size(), 3u);
  EXPECT_EQ(screen.hoverables[0].name, "Tough Hunk of Bread");
  EXPECT_EQ(screen.hoverables[0].ref.kind, pb::HoverRef::ITEM);
  EXPECT_EQ(screen.hoverables[0].ref.id, "4540");
  EXPECT_EQ(screen.hoverables[2].name, "Your Place In The World");
  EXPECT_EQ(screen.hoverables[2].ref.kind, pb::HoverRef::QUEST);
}

// Panels come after every option, and quest text is cut (review of f40b0b6,
// finding 7: a full quest log pushed "also:" out of a 4,096-token slot).
TEST(ScreenRendererTest, OptionsComeBeforePanelsAndQuestTextIsCut) {
  pb::Snapshot snap;
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.ui.open_panels = {pb::QUEST_LOG};
  pb::QuestLog::Entry& q = snap.ext.quest_log.emplace().quests.emplace_back();
  q.objective.emplace().quest.emplace().quest_id = 1;
  q.objective->quest->quest = "Long";
  q.description = std::string(500, 'a') + "\xC3\xA9";
  const Screen screen = RenderScreen(snap);
  const size_t also = screen.text.find("also:");
  const size_t log = screen.text.find("quest log:");
  ASSERT_NE(also, std::string::npos) << screen.text;
  ASSERT_NE(log, std::string::npos) << screen.text;
  EXPECT_LT(also, log);
  EXPECT_NE(screen.text.find(std::string(240, 'a') + "..."), std::string::npos);
  EXPECT_EQ(screen.text.find(std::string(241, 'a')), std::string::npos);
}

TEST(ScreenRendererTest, RendersSelfAurasAsBuffsAndDebuffs) {
  pb::Snapshot snap;
  snap.base.self.state = pb::Self::ALIVE;
  pb::Aura& fort = snap.base.self.unit.auras.emplace_back();
  fort.name = "Power Word: Fortitude";
  fort.remaining_s = 1700.0f;
  pb::Aura& food = snap.base.self.unit.auras.emplace_back();
  food.name = "Food";
  food.remaining_s = 15.2f;
  pb::Aura& ms = snap.base.self.unit.auras.emplace_back();
  ms.name = "Mortal Strike";
  ms.harmful = true;
  ms.stacks = 3;
  ms.remaining_s = 8.0f;
  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("buffs: Power Word: Fortitude (28m), Food (15s)"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("debuffs: Mortal Strike x3 (8s)"),
            std::string::npos)
      << screen.text;
}

TEST(ScreenRendererTest, RendersMainAssistHurtAndLooseMobsInParty) {
  pb::Snapshot snap;
  snap.bot = "Loadb";
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.self.in_combat = true;

  pb::Party& party = snap.ext.party.emplace();
  pb::Unit& tank = party.members.emplace_back();
  tank.name = "Loada";
  tank.guid = "0x100";
  tank.health_pct = 62.0f;
  tank.rel.dist_yd = 18.0f;
  tank.group.emplace().online = true;
  tank.group->in_range = true;
  tank.group->role = pb::TANK;
  tank.group->main_tank = true;

  pb::Unit& dps = party.members.emplace_back();
  dps.name = "Loadc";
  dps.guid = "0x101";
  dps.health_pct = 85.0f;
  dps.rel.dist_yd = 46.0f;
  dps.rel.in_los = false;
  dps.group.emplace().online = true;
  dps.group->in_range = true;
  dps.group->role = pb::DPS;
  dps.group->main_assist = true;

  pb::Unit& far_ally = party.members.emplace_back();
  far_ally.name = "Loadd";
  far_ally.guid = "0x102";
  far_ally.health_pct = 90.0f;
  far_ally.group.emplace().online = true;
  far_ally.group->in_range = false;
  far_ally.group->role = pb::DPS;

  pb::Unit& mob = snap.base.nearby.units.emplace_back();
  mob.guid = "0x200";
  mob.name = "Phoenix";
  mob.kind = pb::Unit::NPC;
  mob.reaction = pb::Unit::HOSTILE;
  mob.target = "Loadc";
  mob.rel.dist_yd = 14.0f;

  pb::Unit& boss = snap.base.nearby.units.emplace_back();
  boss.guid = "0x201";
  boss.name = "Kael'thas Sunstrider";
  boss.kind = pb::Unit::NPC;
  boss.reaction = pb::Unit::HOSTILE;
  boss.target = "Loada";
  boss.rel.dist_yd = 20.0f;

  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("main tank"), std::string::npos) << screen.text;
  EXPECT_NE(screen.text.find("main assist"), std::string::npos) << screen.text;
  EXPECT_NE(screen.text.find("party hurt: Loada 62% (18 yd), "
                             "Loadc 85% (46 yd, out of sight), "
                             "Loadd 90% (out of range)"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("loose mobs: Phoenix (targeting Loadc, 14 yd)"),
            std::string::npos)
      << screen.text;
  // Boss targeting tank Loada must not be listed as a loose mob.
  EXPECT_EQ(screen.text.find("Kael'thas Sunstrider (targeting Loada"),
            std::string::npos)
      << screen.text;
}

TEST(ScreenRendererTest, LooseMobsAndPartyHurtEdgeCases) {
  // 1. Two-tank raid where self is Tank 1: mob targeting self is NOT loose,
  //    pulled neutral mob targeting healer IS loose, 99.6% HP is NOT hurt.
  pb::Snapshot two_tank;
  two_tank.bot = "Tank1";
  two_tank.base.self.state = pb::Self::ALIVE;
  two_tank.base.self.in_combat = true;
  two_tank.base.self.unit.group.emplace().role = pb::TANK;
  two_tank.base.self.unit.group->main_tank = true;

  pb::Party& party = two_tank.ext.party.emplace();
  pb::Unit& tank2 = party.members.emplace_back();
  tank2.name = "Tank2";
  tank2.guid = "0x100";
  tank2.health_pct = 99.6f;  // Rounds to 100%: must not appear in party hurt.
  tank2.group.emplace().online = true;
  tank2.group->in_range = true;
  tank2.group->role = pb::TANK;

  pb::Unit& healer = party.members.emplace_back();
  healer.name = "Heal1";
  healer.guid = "0x101";
  healer.health_pct = 100.0f;
  healer.group.emplace().online = true;
  healer.group->in_range = true;
  healer.group->role = pb::HEALER;

  pb::Unit& boss = two_tank.base.nearby.units.emplace_back();
  boss.guid = "0x200";
  boss.name = "Patchwerk";
  boss.kind = pb::Unit::NPC;
  boss.reaction = pb::Unit::HOSTILE;
  boss.target = "Tank1";
  boss.rel.dist_yd = 5.0f;

  pb::Unit& pulled_neutral = two_tank.base.nearby.units.emplace_back();
  pulled_neutral.guid = "0x201";
  pulled_neutral.name = "Sludge Belcher";
  pulled_neutral.kind = pb::Unit::NPC;
  pulled_neutral.reaction = pb::Unit::NEUTRAL;
  pulled_neutral.target = "Heal1";
  pulled_neutral.rel.dist_yd = 12.0f;

  const Screen s_two = RenderScreen(two_tank);
  EXPECT_EQ(s_two.text.find("party hurt:"), std::string::npos) << s_two.text;
  EXPECT_EQ(s_two.text.find("Patchwerk (targeting Tank1"), std::string::npos)
      << s_two.text;
  EXPECT_NE(
      s_two.text.find("loose mobs: Sludge Belcher (targeting Heal1, 12 yd)"),
      std::string::npos)
      << s_two.text;

  // 2. Out of combat: a tank still sees them (a mob that opened on the healer
  // is the tank's to take, whoever pulled); anyone else doesn't.
  two_tank.base.self.in_combat = false;
  const Screen s_ooc = RenderScreen(two_tank);
  EXPECT_NE(s_ooc.text.find("loose mobs: Sludge Belcher"), std::string::npos)
      << s_ooc.text;
  pb::Snapshot ooc_dps = two_tank;
  ooc_dps.base.self.unit.group->role = pb::DPS;
  ooc_dps.base.self.unit.group->main_tank = false;
  const Screen s_ooc_dps = RenderScreen(ooc_dps);
  EXPECT_EQ(s_ooc_dps.text.find("loose mobs:"), std::string::npos)
      << s_ooc_dps.text;
  EXPECT_EQ(s_ooc_dps.text.find("(loose, on"), std::string::npos)
      << s_ooc_dps.text;

  // 3. Group with no tank role/flag assigned: loose mobs suppressed (no
  // inversion).
  pb::Snapshot no_tank = two_tank;
  no_tank.base.self.in_combat = true;
  no_tank.base.self.unit.group.reset();
  no_tank.ext.party->members[0].group->role = pb::ROLE_UNSPECIFIED;
  const Screen s_no_tank = RenderScreen(no_tank);
  EXPECT_EQ(s_no_tank.text.find("loose mobs:"), std::string::npos)
      << s_no_tank.text;
}

TEST(ScreenRendererTest, RendersNearbyAreasNearestFirst) {
  pb::Snapshot snap;
  snap.bot = "Loada";
  snap.base.self.unit.name = "Loada";
  snap.base.self.in_combat = true;

  // Nameless area is skipped.
  pb::Area nameless;
  nameless.guid = "trigger:0x0";
  nameless.hostile = true;
  nameless.rel.emplace().dist_yd = 1.0f;
  snap.base.nearby.areas.push_back(nameless);

  // Far hostile area with lands_in_s (> 999 yd to pin the infinity() sentinel).
  pb::Area far_area;
  far_area.guid = "trigger:0x1";
  far_area.name = "Flame Strike";
  far_area.hostile = true;
  far_area.radius_yd = 8.0f;
  far_area.lands_in_s = 2.5f;
  far_area.rel.emplace().dist_yd = 1200.0f;
  far_area.rel->dir = pb::N;
  snap.base.nearby.areas.push_back(far_area);

  // Near hostile area with inside_me.
  pb::Area near_area;
  near_area.guid = "trigger:0x2";
  near_area.name = "Arcane Sphere";
  near_area.hostile = true;
  near_area.radius_yd = 5.0f;
  near_area.inside_me = true;
  near_area.rel.emplace().dist_yd = 4.0f;
  near_area.rel->dir = pb::E;
  snap.base.nearby.areas.push_back(near_area);

  // Friendly area using spell->name fallback when area.name is empty.
  pb::Area friendly;
  friendly.guid = "dyn:0x3";
  friendly.spell.emplace().name = "Consecration";
  friendly.hostile = false;
  friendly.rel.emplace().dist_yd = 6.0f;
  friendly.rel->dir = pb::S;
  snap.base.nearby.areas.push_back(friendly);

  // Area with NaN dist_yd and NaN lands_in_s must not break sorting or output.
  pb::Area broken;
  broken.guid = "trigger:0x4";
  broken.name = "Broken Pool";
  broken.hostile = true;
  broken.lands_in_s = std::numeric_limits<float>::quiet_NaN();
  broken.rel.emplace().dist_yd = std::numeric_limits<float>::quiet_NaN();
  broken.rel->dir = pb::W;
  snap.base.nearby.areas.push_back(broken);

  // Area with missing rel (sorts after finite distances).
  pb::Area no_rel;
  no_rel.guid = "trigger:0x5";
  no_rel.name = "Distant Pool";
  no_rel.hostile = true;
  snap.base.nearby.areas.push_back(no_rel);

  // Extra areas to verify the 6-area cap.
  pb::Area extra1;
  extra1.guid = "trigger:0x6";
  extra1.name = "Mid Pool A";
  extra1.hostile = true;
  extra1.rel.emplace().dist_yd = 8.0f;
  extra1.rel->dir = pb::N;
  snap.base.nearby.areas.push_back(extra1);

  pb::Area extra2;
  extra2.guid = "trigger:0x7";
  extra2.name = "Mid Pool B";
  extra2.hostile = true;
  extra2.rel.emplace().dist_yd = 10.0f;
  extra2.rel->dir = pb::N;
  snap.base.nearby.areas.push_back(extra2);

  const Screen s = RenderScreen(snap);
  const size_t areas_pos = s.text.find("areas:\n");
  ASSERT_NE(areas_pos, std::string::npos) << s.text;
  const size_t sphere_pos =
      s.text.find("  Arcane Sphere (hostile, 4 yd E, inside)");
  const size_t consec_pos = s.text.find("  Consecration (friendly, 6 yd S)");
  const size_t flame_pos =
      s.text.find("  Flame Strike (hostile, 1200 yd N, lands in 2.5s)");
  ASSERT_NE(sphere_pos, std::string::npos) << s.text;
  ASSERT_NE(consec_pos, std::string::npos) << s.text;
  ASSERT_NE(flame_pos, std::string::npos) << s.text;
  EXPECT_LT(sphere_pos, consec_pos) << s.text;
  EXPECT_LT(consec_pos, flame_pos) << s.text;
  // Broken Pool is the 6th entry (after all finite distances, no NaN
  // lands_in_s); Distant Pool is capped.
  const size_t broken_pos = s.text.find("  Broken Pool (hostile, 0 yd W)");
  ASSERT_NE(broken_pos, std::string::npos) << s.text;
  EXPECT_LT(flame_pos, broken_pos) << s.text;
  EXPECT_EQ(s.text.find("lands in nan"), std::string::npos) << s.text;
  EXPECT_EQ(s.text.find("Distant Pool"), std::string::npos) << s.text;
}

TEST(ScreenRendererTest, RendersPartyLeaderTagOnSelfOnlyWhenSelfIsLeader) {
  // 1. Self is party leader via self.unit.group->leader and party.leader.
  pb::Snapshot leader_snap = NewSnapshot();
  leader_snap.bot = "Tess";
  leader_snap.base.self.unit.guid = "0x10";
  leader_snap.base.self.unit.group.emplace().leader = true;
  pb::Party& leader_party = leader_snap.ext.party.emplace();
  leader_party.leader = "Tess";
  leader_party.loot_method = "GROUP_LOOT";
  pb::Unit& mate = leader_party.members.emplace_back();
  mate.guid = "0x11";
  mate.name = "Gatda";
  mate.group.emplace().online = true;
  mate.group->in_range = true;
  mate.group->leader = false;

  const Screen leader_screen = RenderScreen(leader_snap);
  const std::string leader_me =
      leader_screen.text.substr(0, leader_screen.text.find('\n'));
  EXPECT_NE(leader_me.find("; party leader"), std::string::npos) << leader_me;
  EXPECT_EQ(leader_me.find("party member"), std::string::npos) << leader_me;
  EXPECT_NE(leader_screen.text.find("party: leader you; loot group loot\n"
                                    "  - Gatda"),
            std::string::npos)
      << leader_screen.text;

  // 2. Self is party leader via self.unit.group->leader alone (empty
  //    party.leader).
  pb::Snapshot group_only = leader_snap;
  group_only.ext.party->leader.clear();
  const Screen group_only_screen = RenderScreen(group_only);
  EXPECT_NE(group_only_screen.text.find("; party leader"), std::string::npos)
      << group_only_screen.text;
  EXPECT_EQ(group_only_screen.text.find("party member"), std::string::npos)
      << group_only_screen.text;
  EXPECT_NE(group_only_screen.text.find("party: leader you; loot group loot"),
            std::string::npos)
      << group_only_screen.text;

  // 3. Self is a follower: me: must say party member (not party leader), and
  //    the leader member line carries the "leader" tag.
  pb::Snapshot follower_snap = NewSnapshot();
  follower_snap.bot = "Tess";
  follower_snap.base.self.unit.guid = "0x10";
  follower_snap.base.self.unit.group.emplace().leader = false;
  pb::Party& follower_party = follower_snap.ext.party.emplace();
  follower_party.leader = "Gatda";
  follower_party.loot_method = "GROUP_LOOT";
  pb::Unit& other_leader = follower_party.members.emplace_back();
  other_leader.guid = "0x11";
  other_leader.name = "Gatda";
  other_leader.group.emplace().online = true;
  other_leader.group->in_range = true;
  other_leader.group->leader = true;

  const Screen follower_screen = RenderScreen(follower_snap);
  const std::string follower_me =
      follower_screen.text.substr(0, follower_screen.text.find('\n'));
  EXPECT_EQ(follower_me.find("party leader"), std::string::npos) << follower_me;
  EXPECT_NE(follower_me.find("; party member"), std::string::npos)
      << follower_me;
  EXPECT_EQ(follower_screen.text.find("leader you"), std::string::npos)
      << follower_screen.text;
  EXPECT_NE(follower_screen.text.find("party: loot group loot\n"
                                      "  - Gatda: leader"),
            std::string::npos)
      << follower_screen.text;

  // 4. Solo player (and empty ext.party{}): me: has neither party leader nor
  //    party member.
  const Screen solo_screen = RenderScreen(NewSnapshot());
  const std::string solo_me =
      solo_screen.text.substr(0, solo_screen.text.find('\n'));
  EXPECT_EQ(solo_me.find("party leader"), std::string::npos) << solo_me;
  EXPECT_EQ(solo_me.find("party member"), std::string::npos) << solo_me;

  pb::Snapshot empty_party_snap = NewSnapshot();
  empty_party_snap.ext.party.emplace();
  const Screen empty_party_screen = RenderScreen(empty_party_snap);
  const std::string empty_party_me =
      empty_party_screen.text.substr(0, empty_party_screen.text.find('\n'));
  EXPECT_EQ(empty_party_me.find("party leader"), std::string::npos)
      << empty_party_me;
  EXPECT_EQ(empty_party_me.find("party member"), std::string::npos)
      << empty_party_me;
}

// Now #5: the party's pace on one line: who fights, who is dead or gone, and
// a healer low on mana.
TEST(ScreenRendererTest, PartyStateLineSummarizesThePace) {
  pb::Snapshot snap;
  snap.bot = "Loada";
  pb::Party& party = snap.ext.party.emplace();
  party.leader = "Hele";
  const auto add = [&](const char* name, pb::Role role) -> pb::Unit& {
    pb::Unit& m = party.members.emplace_back();
    m.name = name;
    m.guid = std::string("0x") + name;
    m.group.emplace().online = true;
    m.group->role = role;
    return m;
  };
  add("Hele", pb::TANK).group->aggro = true;
  add("Zenji", pb::HEALER).power_pct = 20.0f;
  add("Rokzul", pb::DPS).dead = true;
  add("Mabaku", pb::DPS).group->online = false;
  party.members[0].group->quests = {
      "Kobold Camp Cleanup: Kobold Vermin slain 3/8"};
  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("party_state: fighting (Hele); healer Zenji 20% "
                             "mana; Rokzul dead; Mabaku offline"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("help Hele with Kobold Camp Cleanup (Kobold "
                             "Vermin slain 3/8)"),
            std::string::npos)
      << screen.text;

  pb::Snapshot calm;
  calm.bot = "Loada";
  calm.ext.party.emplace().members.emplace_back().name = "Hele";
  calm.ext.party->members.back().group.emplace().online = true;
  EXPECT_EQ(RenderScreen(calm).text.find("party_state:"), std::string::npos);
  // Milestone 3a: a human's /afk shows, while they're online.
  calm.ext.party->members.back().group->afk = true;
  EXPECT_NE(RenderScreen(calm).text.find("  party_state: afk Hele\n"),
            std::string::npos)
      << RenderScreen(calm).text;
  calm.ext.party->members.back().group->online = false;
  EXPECT_EQ(RenderScreen(calm).text.find("afk Hele"), std::string::npos);
  calm.ext.party->members.back().group->online = true;
  calm.ext.party->members.back().group->afk = false;

  // Now #9: Luck of the Draw and Dungeon Deserter facts on the party lines.
  pb::Aura& luck = calm.base.self.unit.auras.emplace_back();
  luck.id = 72221;
  luck.name = "Luck of the Draw";
  luck.stacks = 3;
  pb::Aura& deserter = calm.ext.party->members.back().auras.emplace_back();
  deserter.id = 71041;
  deserter.name = "Dungeon Deserter";
  deserter.harmful = true;
  deserter.remaining_s = 1740.0f;
  const Screen lfg_screen = RenderScreen(calm);
  EXPECT_NE(lfg_screen.text.find("luck of the draw (+15%)"), std::string::npos)
      << lfg_screen.text;
  EXPECT_NE(lfg_screen.text.find("party_state: deserter Hele (29m)"),
            std::string::npos)
      << lfg_screen.text;
}

pb::Spell* AddSpell(pb::Snapshot* snapshot,
                    const std::string& name,
                    float cooldown_left_s) {
  pb::Spell& spell = snapshot->ext.action_bars->buttons.emplace_back();
  spell.name = name;
  spell.ability.emplace().known = true;
  spell.ability->cooldown_remaining_s = cooldown_left_s;
  return &spell;
}

TEST(ScreenRendererTest, SpellsLineListsKnownSpellsWithCooldownsLeft) {
  pb::Snapshot snapshot = NewSnapshot();
  snapshot.ext.action_bars.emplace();
  AddSpell(&snapshot, "Sinister Strike", 0.0f);
  AddSpell(&snapshot, "Sprint", 42.4f);
  AddSpell(&snapshot, "Evasion", 0.4f);  // Under a second: shown as ready.
  AddSpell(&snapshot, "Sprint", 42.4f);  // Same name twice: written once.
  snapshot.ext.action_bars->stance_bar.emplace_back().name = "Stealth";
  const Screen screen = RenderScreen(snapshot);
  EXPECT_NE(screen.text.find("\nspells: Sinister Strike, Sprint (cd 42s), "
                             "Evasion, Stealth\nalso:\n"),
            std::string::npos)
      << screen.text;
  // The spells line is not an option: the fixed options are unchanged.
  EXPECT_EQ(screen.options,
            (std::vector<OptionKind>{OptionKind::kKeep, OptionKind::kExplore,
                                     OptionKind::kRest, OptionKind::kTown}));
}

TEST(ScreenRendererTest, NoSpellsLineWithoutActionBarsOrNames) {
  pb::Snapshot snapshot = NewSnapshot();
  EXPECT_EQ(RenderScreen(snapshot).text.find("spells:"), std::string::npos);
  snapshot.ext.action_bars.emplace();
  EXPECT_EQ(RenderScreen(snapshot).text.find("spells:"), std::string::npos);
  AddSpell(&snapshot, "", 0.0f);
  EXPECT_EQ(RenderScreen(snapshot).text.find("spells:"), std::string::npos);
  // A broken cooldown is safe and reads as ready.
  AddSpell(&snapshot, "Kick", std::numeric_limits<float>::quiet_NaN());
  EXPECT_NE(RenderScreen(snapshot).text.find("\nspells: Kick\n"),
            std::string::npos);
}

TEST(ScreenRendererTest, TradeWindowShowsBothOffersAndWhoAccepted) {
  pb::Snapshot snapshot = NewSnapshot();
  EXPECT_EQ(RenderScreen(snapshot).text.find("trade with"), std::string::npos);
  pb::Trade& trade = snapshot.ext.trade.emplace();
  // No trader name: nothing to show.
  EXPECT_EQ(RenderScreen(snapshot).text.find("trade with"), std::string::npos);
  trade.with = "Hele";
  const Screen empty = RenderScreen(snapshot);
  EXPECT_NE(empty.text.find("\ntrade with Hele: they give nothing; I give "
                            "nothing\n"),
            std::string::npos)
      << empty.text;
  // Facts only, never an option of its own.
  EXPECT_EQ(empty.options,
            (std::vector<OptionKind>{OptionKind::kKeep, OptionKind::kExplore,
                                     OptionKind::kRest, OptionKind::kTown}));
  pb::Item& water = trade.giving.emplace_back();
  water.name = "Conjured Water";
  water.count = 20;
  pb::Item& stone = trade.giving.emplace_back();
  stone.name = "Minor Healthstone";
  stone.count = 1;
  trade.getting_money = 50500;
  trade.they_accepted = true;
  EXPECT_NE(RenderScreen(snapshot).text.find(
                "\ntrade with Hele: they give 5g 5s (accepted); I give "
                "Conjured Water x20, Minor Healthstone\n"),
            std::string::npos)
      << RenderScreen(snapshot).text;
  trade.getting.emplace_back().name = "Linen Cloth";
  trade.they_accepted = false;
  trade.i_accepted = true;
  EXPECT_NE(RenderScreen(snapshot).text.find(
                "\ntrade with Hele: they give Linen Cloth and 5g 5s; I give "
                "Conjured Water x20, Minor Healthstone (accepted)\n"),
            std::string::npos)
      << RenderScreen(snapshot).text;
  // When the bot has money, the trade line shows it alongside the trade gold
  // act hint so the brain knows how much it can offer.
  snapshot.base.self.money = 120500;
  trade.giving_money = 10000;
  EXPECT_NE(RenderScreen(snapshot).text.find(
                "\ntrade with Hele: they give Linen Cloth and 5g 5s; I give "
                "Conjured Water x20, Minor Healthstone and 1g (accepted); "
                "my money 12g 5s (act: trade gold <amount>)\n"),
            std::string::npos)
      << RenderScreen(snapshot).text;
}

TEST(ScreenRendererTest, PartyPingShowsWhoAndWhereWithAgeWhenOld) {
  pb::Snapshot snapshot = NewSnapshot();
  EXPECT_EQ(RenderScreen(snapshot).text.find("ping:"), std::string::npos);
  pb::Minimap& minimap = snapshot.ext.minimap.emplace();
  EXPECT_EQ(RenderScreen(snapshot).text.find("ping:"), std::string::npos);
  pb::Minimap::Ping& ping = minimap.ping.emplace();
  ping.from = "Hele";
  ping.rel.dist_yd = 18.2f;
  ping.rel.dir = pb::NE;
  ping.age_s = 1.5f;
  const Screen fresh = RenderScreen(snapshot);
  EXPECT_NE(fresh.text.find(
                "\nping: Hele pinged 18 yd NE (go there -> act: to ping)\n"),
            std::string::npos)
      << fresh.text;
  // Not an option: the fixed options are unchanged.
  EXPECT_EQ(fresh.options,
            (std::vector<OptionKind>{OptionKind::kKeep, OptionKind::kExplore,
                                     OptionKind::kRest, OptionKind::kTown}));
  ping.age_s = 12.4f;
  ping.rel.height = pb::Relative::BELOW;
  EXPECT_NE(RenderScreen(snapshot).text.find(
                "\nping: Hele pinged 18 yd NE, below (12s ago) (go there -> "
                "act: to ping)\n"),
            std::string::npos)
      << RenderScreen(snapshot).text;
  // Broken numbers are safe; a ping at the bot's feet reads "here".
  ping.age_s = std::numeric_limits<float>::quiet_NaN();
  ping.rel = {};
  ping.rel.dir = pb::HERE;
  EXPECT_NE(RenderScreen(snapshot).text.find(
                "\nping: Hele pinged here (go there -> act: to ping)\n"),
            std::string::npos)
      << RenderScreen(snapshot).text;
  // A ping with no sender name is never rendered.
  ping.from.clear();
  EXPECT_EQ(RenderScreen(snapshot).text.find("ping:"), std::string::npos);
}

TEST(ScreenRendererTest, AHugeSpellbookIsCutWithACountAndKeepsOptions) {
  pb::Snapshot snapshot = NewSnapshot();
  snapshot.ext.action_bars.emplace();
  for (int i = 0; i < 120; ++i) {
    AddSpell(&snapshot, "Spell Number " + std::to_string(i), 0.0f);
  }
  const Screen screen = RenderScreen(snapshot);
  const size_t start = screen.text.find("\nspells: ");
  ASSERT_NE(start, std::string::npos);
  const size_t end = screen.text.find('\n', start + 1);
  const std::string line = screen.text.substr(start + 1, end - start - 1);
  EXPECT_LE(line.size(), 620u) << line;
  EXPECT_TRUE(line.ends_with(" more")) << line;
  EXPECT_NE(line.find("Spell Number 0, "), std::string::npos);
  EXPECT_NE(screen.text.find("  4. Go to the nearest town"), std::string::npos);
}

// 4e: what a bot tank needs on screen to hold aggro, whoever opened the fight:
// its own role, the module's "(not pulled)" on numbered lines, which mobs are
// on another member, and which are crowd-controlled (never "loose").
TEST(ScreenRendererTest, TankSeesRoleLooseMobsAndCrowdControl) {
  pb::Snapshot snap;
  snap.bot = "Tank1";
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.self.unit.name = "Tank1";
  snap.base.self.unit.group.emplace().role = pb::TANK;
  pb::Party& party = snap.ext.party.emplace();
  party.leader = "Heal1";
  pb::Unit& healer = party.members.emplace_back();
  healer.name = "Heal1";
  healer.group.emplace().online = true;
  healer.group->leader = true;
  healer.group->role = pb::HEALER;
  pb::Unit& mage = party.members.emplace_back();
  mage.name = "Mage1";
  mage.group.emplace().online = true;
  mage.group->role = pb::DPS;

  const auto add_mob = [&](const char* guid, const char* name, float dist) {
    pb::Unit& u = snap.base.nearby.units.emplace_back();
    u.guid = guid;
    u.name = name;
    u.kind = pb::Unit::NPC;
    u.reaction = pb::Unit::HOSTILE;
    u.rel.dist_yd = dist;
    pb::Option& o = snap.options.emplace_back();
    o.text = std::string("Attack ") + name;
    o.guid = guid;
    o.kind = pb::Option::ATTACK;
    o.name = name;
    return &u;
  };
  pb::Unit* sheep = add_mob("0x1", "Sheeped Shaman", 6.0f);
  sheep->target = "Mage1";  // A sheep keeps its victim: still not loose.
  sheep->crowd_control.emplace();
  sheep->crowd_control->name = "Polymorph";
  sheep->crowd_control->caster = "Mage1";
  sheep->crowd_control->remaining_s = 31.2f;
  snap.options.back().text += " (not pulled)";
  pb::Unit* on_mage = add_mob("0x2", "Trogg", 10.0f);
  on_mage->target = "Mage1";
  pb::Unit* idle = add_mob("0x3", "Idle Cultist", 14.0f);
  snap.options.back().text += " (not pulled)";
  (void)idle;
  snap.options.emplace_back().text = "Keep doing what I'm doing";
  snap.options.back().kind = pb::Option::KEEP;

  // Out of combat (the mage opened): the tank sees it all.
  Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("party member; role tank"), std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("1. Attack Sheeped Shaman (not pulled): "),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("crowd-controlled (Polymorph by Mage1, 31s; "
                             "damage breaks it)"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("2. Attack Trogg (loose, on Mage1): "),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("3. Attack Idle Cultist (not pulled): "),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("loose mobs: Trogg (targeting Mage1, 10 yd)"),
            std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.text.find("Sheeped Shaman (loose"), std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.text.find("Sheeped Shaman (targeting"), std::string::npos)
      << screen.text;
  // The options keep their numbers and targets.
  ASSERT_EQ(screen.details.size(), 4u);
  EXPECT_EQ(screen.details[1].guid, "0x2");
  EXPECT_EQ(screen.details[1].name, "Trogg");

  // A healer in the same spot: its role, no loose tags out of combat.
  pb::Snapshot as_healer = snap;
  as_healer.base.self.unit.group->role = pb::HEALER;
  screen = RenderScreen(as_healer);
  EXPECT_NE(screen.text.find("party member; role healer"), std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.text.find("(loose, on"), std::string::npos) << screen.text;

  // Fixated on the mage (threat can't take it) and taunt immune: shown as
  // facts, never loose; a taunt-immune mob that isn't fixated stays loose.
  pb::Snapshot fixate = snap;
  fixate.base.nearby.units[1].fixated_on = "Mage1";
  fixate.base.nearby.units[1].taunt_immune = true;
  screen = RenderScreen(fixate);
  EXPECT_NE(screen.text.find("2. Attack Trogg: "), std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("target Mage1, fixated on Mage1, taunt immune "
                             "yes"),
            std::string::npos)
      << screen.text;
  EXPECT_EQ(screen.text.find("loose mobs:"), std::string::npos) << screen.text;
  fixate.base.nearby.units[1].fixated_on.clear();
  screen = RenderScreen(fixate);
  EXPECT_NE(screen.text.find("2. Attack Trogg (loose, on Mage1): "),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("taunt immune yes"), std::string::npos)
      << screen.text;

  // Solo: no role on `me:`.
  pb::Snapshot solo = snap;
  solo.ext.party.reset();
  solo.base.self.unit.group.reset();
  screen = RenderScreen(solo);
  EXPECT_EQ(screen.text.find("role "), std::string::npos) << screen.text;
  EXPECT_EQ(screen.text.find("(loose, on"), std::string::npos) << screen.text;
}

TEST(ScreenRendererTest, SpellMissShowsOnCasterScreenWithAndWithoutTarget) {
  pb::Snapshot snapshot = NewSnapshot();
  EXPECT_EQ(RenderScreen(snapshot).text.find("missed:"), std::string::npos);

  pb::Self::SpellMiss& miss = snapshot.base.self.spell_miss.emplace();
  miss.spell = "Sap";
  miss.target = "Defias Cutpurse";
  miss.reason = "dodge";
  miss.seq = 1;
  const Screen with_target = RenderScreen(snapshot);
  EXPECT_NE(with_target.text.find("\nmissed: Sap on Defias Cutpurse (dodge)\n"),
            std::string::npos)
      << with_target.text;
  // Not an option: the fixed options are unchanged.
  EXPECT_EQ(with_target.options,
            (std::vector<OptionKind>{OptionKind::kKeep, OptionKind::kExplore,
                                     OptionKind::kRest, OptionKind::kTown}));

  // Without target name, omits "on <target>".
  miss.target.clear();
  miss.reason = "immune";
  EXPECT_NE(RenderScreen(snapshot).text.find("\nmissed: Sap (immune)\n"),
            std::string::npos)
      << RenderScreen(snapshot).text;

  // Missing spell or reason is never rendered.
  miss.reason.clear();
  EXPECT_EQ(RenderScreen(snapshot).text.find("missed:"), std::string::npos);
}

TEST(ScreenRendererTest, TankDebuffsShowOnPartyTankLineAndNotNonTanks) {
  pb::Snapshot snap = NewSnapshot();
  snap.bot = "Offtank";
  snap.base.self.unit.name = "Offtank";
  snap.base.self.unit.group.emplace().role = pb::TANK;
  snap.base.self.in_combat = true;

  pb::Party& party = snap.ext.party.emplace();
  party.leader = "Hele";

  pb::Unit& mt = party.members.emplace_back();
  mt.guid = "0x10";
  mt.name = "MainTank";
  mt.kind = pb::Unit::PLAYER;
  mt.player.emplace().class_ = "Paladin";
  mt.health_pct = 82.0f;
  mt.target = "Professor Putricide";
  mt.group.emplace().role = pb::TANK;
  mt.group->online = true;
  mt.group->in_range = true;
  mt.group->aggro = true;
  auto make_aura = [](uint32_t id, std::string name, bool harmful,
                      std::optional<float> remaining_s = std::nullopt,
                      uint32_t stacks = 0) {
    pb::Aura a;
    a.id = id;
    a.name = std::move(name);
    a.harmful = harmful;
    a.remaining_s = remaining_s;
    a.stacks = stacks;
    return a;
  };
  // Harmful stacking debuff + single-stack debuff + positive buff + deserter.
  mt.auras.push_back(make_aura(72451, "Mutated Plague", true, 10.0f, 3));
  mt.auras.push_back(make_aura(72410, "Rune of Blood", true, 18.0f, 0));
  mt.auras.push_back(make_aura(25898, "Greater Blessing of Kings", false));
  mt.auras.push_back(make_aura(71041, "Dungeon Deserter", true));

  pb::Unit& dps = party.members.emplace_back();
  dps.guid = "0x11";
  dps.name = "Mage1";
  dps.kind = pb::Unit::PLAYER;
  dps.player.emplace().class_ = "Mage";
  dps.health_pct = 90.0f;
  dps.group.emplace().role = pb::DPS;
  dps.group->online = true;
  dps.group->in_range = true;
  dps.auras.push_back(make_aura(72451, "Mutated Plague", true, 10.0f, 2));

  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find(
                "MainTank: paladin, tank, has aggro, health 82%, "
                "target Professor Putricide, debuffs: Mutated Plague x3 (10s), "
                "Rune of Blood (18s)"),
            std::string::npos)
      << screen.text;
  // Positive buffs and Dungeon Deserter stay out of tank `debuffs:`.
  EXPECT_EQ(screen.text.find("Greater Blessing of Kings"), std::string::npos);
  // Non-tank debuffs are not rendered on their party line.
  const size_t mage_pos = screen.text.find("Mage1");
  ASSERT_NE(mage_pos, std::string::npos);
  const size_t mage_eol = screen.text.find('\n', mage_pos);
  const std::string mage_line =
      screen.text.substr(mage_pos, mage_eol - mage_pos);
  EXPECT_EQ(mage_line.find("debuffs:"), std::string::npos) << mage_line;

  // Dead tank: no debuffs shown.
  snap.ext.party->members[0].dead = true;
  EXPECT_EQ(RenderScreen(snap).text.find("debuffs: Mutated Plague"),
            std::string::npos);
}

TEST(ScreenRendererTest, RendersVehicleStatusSeatAbilitiesAndClearsOnExit) {
  pb::Snapshot snap = NewSnapshot();
  snap.bot = "Tess";
  pb::Unit& veh = snap.ext.vehicle.emplace();
  veh.guid = "0xF130006E00000001";
  veh.name = "Wyrmrest Defender";
  veh.kind = pb::Unit::VEHICLE;
  veh.health_pct = 84.0f;
  pb::VehiclePart& vp = veh.vehicle.emplace();
  vp.seats.push_back({0, "Tess", true});
  vp.seats.push_back({1, "", true});
  pb::Spell sp1;
  sp1.id = 50240;
  sp1.name = "Flame Spike";
  sp1.ability.emplace().usable = true;
  vp.abilities.push_back(std::move(sp1));
  pb::Spell sp2;
  sp2.id = 50248;
  sp2.name = "Engulf in Flames";
  sp2.ability.emplace().cooldown_remaining_s = 5.0f;
  vp.abilities.push_back(std::move(sp2));

  const Screen in_veh = RenderScreen(snap);
  EXPECT_NE(in_veh.text.find("in vehicle"), std::string::npos) << in_veh.text;
  EXPECT_NE(
      in_veh.text.find("vehicle: Wyrmrest Defender (84% hp; seat 0; abilities: "
                       "Flame Spike, Engulf in Flames (cd 5s))"),
      std::string::npos)
      << in_veh.text;

  // Adversarial counter-test: after exiting vehicle or disconnecting, clearing
  // `ext.vehicle` removes both `in vehicle` and the `vehicle:` line.
  snap.ext.vehicle.reset();
  const Screen exited = RenderScreen(snap);
  EXPECT_EQ(exited.text.find("in vehicle"), std::string::npos) << exited.text;
  EXPECT_EQ(exited.text.find("vehicle:"), std::string::npos) << exited.text;
}

TEST(ScreenRendererTest, RendersRaidDebuffSummaryInstanceMetaAndDbmTimers) {
  pb::Snapshot snap = NewSnapshot();
  snap.bot = "Mirabel";
  snap.base.where.zone = "Icecrown Citadel";
  snap.base.where.in_instance = true;
  pb::Instance& inst = snap.ext.instance.emplace();
  inst.name = "Icecrown Citadel";
  inst.difficulty = "25H";
  inst.size = 25;
  inst.locked_until = "locked (2d 4h)";

  pb::Encounter& enc = snap.ext.encounter.emplace();
  enc.dbm.push_back({"Defile", 4.2f});
  enc.dbm.push_back({"Next Infest", 12.5f});

  pb::Party& party = snap.ext.party.emplace();
  party.leader = "Human";
  party.missing_buffs_i_can_give.push_back("Prayer of Fortitude");
  for (int i = 1; i <= 6; ++i) {
    pb::Unit& dps = party.members.emplace_back();
    dps.guid = "0x2" + std::to_string(i);
    dps.name = "Dps" + std::to_string(i);
    dps.kind = pb::Unit::PLAYER;
    dps.health_pct = 90.0f;
    dps.group.emplace().role = pb::DPS;
    dps.group->online = true;
    dps.group->in_range = true;
    pb::Aura a;
    a.id = 70337;
    a.name = "Necrotic Plague";
    a.harmful = true;
    a.dispel_type = pb::DispelType::DISEASE;
    a.dispellable_by_me = true;
    dps.auras.push_back(std::move(a));
  }

  const Screen screen = RenderScreen(snap);
  EXPECT_NE(screen.text.find("in an instance (25H, locked (2d 4h))"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("dbm: Defile 4.2s, Next Infest 12.5s"),
            std::string::npos)
      << screen.text;
  EXPECT_NE(screen.text.find("missing my buffs Prayer of Fortitude"),
            std::string::npos)
      << screen.text;
  // Compact summary groups all 6 non-tank victims into 4 names + "+2" and
  // surfaces `(dispellable)` without bloating individual member lines.
  EXPECT_NE(screen.text.find("debuffs on party: Necrotic Plague on Dps1, Dps2, "
                             "Dps3, Dps4 +2 (dispellable)"),
            std::string::npos)
      << screen.text;

  // Adversarial counter-test: outdoors (`in_instance = false`, no
  // `ext.instance`) omits instance difficulty/lockout and DBM timers.
  snap.base.where.in_instance = false;
  snap.ext.instance.reset();
  snap.ext.encounter.reset();
  const Screen outdoors = RenderScreen(snap);
  EXPECT_EQ(outdoors.text.find("25H"), std::string::npos) << outdoors.text;
  EXPECT_EQ(outdoors.text.find("dbm:"), std::string::npos) << outdoors.text;
}

}  // namespace
}  // namespace harness
