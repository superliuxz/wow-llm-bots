// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "grammar/plan_hooks.h"

#include <string>

#include "gtest/gtest.h"

namespace harness::grammar::hooks {
namespace {

TEST(PlanHooksTest, NormalizeStepsDropsBracketsAndTightensRepeats) {
  EXPECT_EQ(NormalizeSteps("[3], (1)"), "3, 1");
  EXPECT_EQ(NormalizeSteps("3 x 4, 1"), "3x4, 1");
  EXPECT_EQ(NormalizeSteps("3X4"), "3x4");
  EXPECT_EQ(NormalizeSteps("3\xC3\x97"
                           "4"),
            "3x4");
  EXPECT_EQ(NormalizeSteps("none"), "none");
}

TEST(PlanHooksTest, NormalizeWatchesWritesTheCanonicalForm) {
  EXPECT_EQ(NormalizeWatches("HP < 40%"), "hp<40");
  EXPECT_EQ(NormalizeWatches("hp40, mana 20 %"), "hp<40, mana<20");
  EXPECT_EQ(NormalizeWatches("enemy_player 40yd, Whisper"),
            "enemy_player<40, whisper");
  EXPECT_EQ(NormalizeWatches("attacked"), "attacked");
}

TEST(PlanHooksTest, LookSecondsReadsSecondsAndMinutes) {
  int seconds = 0;
  EXPECT_TRUE(LookSeconds("60", &seconds));
  EXPECT_EQ(seconds, 60);
  EXPECT_TRUE(LookSeconds("90s", &seconds));
  EXPECT_EQ(seconds, 90);
  EXPECT_TRUE(LookSeconds("2 minutes", &seconds));
  EXPECT_EQ(seconds, 120);
  EXPECT_TRUE(LookSeconds("3m", &seconds));
  EXPECT_EQ(seconds, 180);
  EXPECT_FALSE(LookSeconds("soon", &seconds));
  EXPECT_FALSE(LookSeconds("60 hours", &seconds));
  EXPECT_FALSE(LookSeconds("99999", &seconds));  // Too many digits.
}

TEST(PlanHooksTest, CapitalizedNameIsWhatTheGameWrites) {
  std::string name;
  EXPECT_TRUE(CapitalizedName("gATDA", &name));
  EXPECT_EQ(name, "Gatda");
  EXPECT_TRUE(CapitalizedName(" Brakka ", &name));
  EXPECT_EQ(name, "Brakka");
  EXPECT_FALSE(CapitalizedName("X", &name));
  EXPECT_FALSE(CapitalizedName("Bad Name", &name));
  EXPECT_FALSE(CapitalizedName("Thirteenchars", &name));
  EXPECT_FALSE(CapitalizedName("Kel'thuzad", &name));
}

}  // namespace
}  // namespace harness::grammar::hooks
