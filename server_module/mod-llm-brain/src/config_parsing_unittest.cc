// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "config_parsing.h"

#include <optional>
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
  EXPECT_EQ(
      ParseFixtures("Testpriest=priest.pdump, Testwar=/abs/war.pdump",
                    "/repo/tests/fixtures/characters"),
      (Fixtures{{"Testpriest", "/repo/tests/fixtures/characters/priest.pdump"},
                {"Testwar", "/abs/war.pdump"}}));
  EXPECT_EQ(ParseFixtures("Testpriest=priest.pdump", ""),
            (Fixtures{{"Testpriest", "priest.pdump"}}));
}

TEST(ConfigParsingTest, ParseFixturesSkipsMalformedEntries) {
  EXPECT_EQ(ParseFixtures("NoFile, =x.pdump, Name=, Ok=ok.pdump", ""),
            (Fixtures{{"Ok", "ok.pdump"}}));
}

TEST(ConfigParsingTest, ResetCheckPrefersAnAddedStrategy) {
  EXPECT_EQ(ResetCheckFor("-follow,+grind,+new rpg"),
            (StrategyCheck{"grind", /*present=*/true}));
  EXPECT_EQ(ResetCheckFor("-grind, -new rpg, -follow"),
            (StrategyCheck{"grind", /*present=*/false}));
  EXPECT_EQ(ResetCheckFor("+stay"), (StrategyCheck{"stay", true}));
  EXPECT_EQ(ResetCheckFor(""), std::nullopt);
  EXPECT_EQ(ResetCheckFor("grind, +"), std::nullopt);
}

}  // namespace
}  // namespace llm_brain
