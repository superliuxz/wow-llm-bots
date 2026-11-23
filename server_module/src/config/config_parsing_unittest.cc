// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "config/config_parsing.h"

#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

using Strings = std::vector<std::string>;
using Fixtures = std::vector<std::pair<std::string, std::string>>;

TEST(ConfigParsingTest, SplitListTrimsAndDropsEmpty) {
  EXPECT_EQ(SplitList("Dezlu, Wozkaruy,,\tGatda "),
            (Strings{"Dezlu", "Wozkaruy", "Gatda"}));
  EXPECT_EQ(SplitList(""), Strings{});
  EXPECT_EQ(SplitList(" , "), Strings{});
}

TEST(ConfigParsingTest, ParseFixturesJoinsRelativeFilesToDir) {
  EXPECT_EQ(ParseFixtures("Testpriest=priest.pdump, Testwar=/abs/war.pdump",
                          "/repo/server_module/testdata/characters"),
            (Fixtures{{"Testpriest",
                       "/repo/server_module/testdata/characters/priest.pdump"},
                      {"Testwar", "/abs/war.pdump"}}));
  EXPECT_EQ(ParseFixtures("Testpriest=priest.pdump", ""),
            (Fixtures{{"Testpriest", "priest.pdump"}}));
  EXPECT_EQ(ParseFixtures("Loada=new:orc:warrior",
                          "/repo/server_module/testdata/characters"),
            (Fixtures{{"Loada", "new:orc:warrior"}}));
}

TEST(ConfigParsingTest, ParseFixturesSkipsMalformedEntries) {
  EXPECT_EQ(ParseFixtures("NoFile, =x.pdump, Name=, Ok=ok.pdump", ""),
            (Fixtures{{"Ok", "ok.pdump"}}));
}

TEST(ConfigParsingTest, StrategyChecksCoverEveryChange) {
  using Checks = std::vector<StrategyCheck>;
  EXPECT_EQ(StrategyChecks("-follow,+grind,+new rpg"),
            (Checks{{"follow", false}, {"grind", true}, {"new rpg", true}}));
  // Turning grinding off: "grind absent" is checked too, not only "follow".
  EXPECT_EQ(StrategyChecks("-grind, -new rpg, -follow"),
            (Checks{{"grind", false}, {"new rpg", false}, {"follow", false}}));
  EXPECT_EQ(StrategyChecks("+stay,-stay"), (Checks{{"stay", false}}));
  EXPECT_EQ(StrategyChecks("-follow,-stay"),
            (Checks{{"follow", false}, {"stay", false}}));
  EXPECT_EQ(StrategyChecks(""), Checks{});
  EXPECT_EQ(StrategyChecks("grind, +"), Checks{});
}

TEST(ConfigParsingTest, AdjustNonCombatStrategiesRespectsGroupAndDungeon) {
  const std::string live = "-follow,+grind,+new rpg";
  // Solo open-world: unchanged.
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false),
            live);
  // Grouped leader in open world: unchanged.
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/false),
            live);
  // Grouped leader in dungeon: keeps +grind, strips +new rpg.
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/true),
            "-follow,+grind,-new rpg");
  // Grouped follower (open-world or dungeon): runs with leader (+follow).
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/true),
            "+follow,-grind,-new rpg");
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false),
            "+follow,-grind,-new rpg");
  // Stripped test configs (no +grind) are never altered.
  const std::string boss = "-follow,-grind,-new rpg,-wotlk-hol";
  EXPECT_EQ(AdjustNonCombatStrategies(boss, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/true),
            boss);
}

}  // namespace
}  // namespace llm_brain
