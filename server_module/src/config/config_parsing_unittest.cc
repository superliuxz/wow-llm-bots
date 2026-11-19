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
  EXPECT_EQ(StrategyChecks(""), Checks{});
  EXPECT_EQ(StrategyChecks("grind, +"), Checks{});
}

}  // namespace
}  // namespace llm_brain
