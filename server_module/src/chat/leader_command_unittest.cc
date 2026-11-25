// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/leader_command.h"

#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

const std::vector<std::string> kBots = {"Loada", "Zenji"};

std::optional<LeaderCommand> Cmd(const char* text) {
  const auto parsed = ParseLeaderCommand(text, kBots);
  return parsed ? std::optional<LeaderCommand>(parsed->command) : std::nullopt;
}

TEST(LeaderCommandTest, ShortCommandsParse) {
  EXPECT_EQ(Cmd("wait"), LeaderCommand::kWait);
  EXPECT_EQ(Cmd("Wait here!"), LeaderCommand::kWait);
  EXPECT_EQ(Cmd("follow me pls"), LeaderCommand::kFollow);
  EXPECT_EQ(Cmd("let's go"), LeaderCommand::kFollow);
  EXPECT_EQ(Cmd("attack my target"), LeaderCommand::kAttackMyTarget);
  EXPECT_EQ(Cmd("kill skull"), LeaderCommand::kAttackSkull);
  EXPECT_EQ(Cmd("drink up guys"), LeaderCommand::kDrink);
  EXPECT_EQ(Cmd("mount up"), LeaderCommand::kMount);
  EXPECT_EQ(Cmd("@heal"), LeaderCommand::kHealMe);
}

TEST(LeaderCommandTest, SentencesAreNotCommands) {
  EXPECT_FALSE(Cmd("I'll wait for the boat"));
  EXPECT_FALSE(Cmd("can you follow the road to the cave"));
  EXPECT_FALSE(Cmd("anyone up for RFC?"));
  EXPECT_FALSE(Cmd("lol"));
  EXPECT_FALSE(Cmd(""));
}

TEST(LeaderCommandTest, AddressedNamesAreCollected) {
  const auto parsed = ParseLeaderCommand("zenji, drink", kBots);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->command, LeaderCommand::kDrink);
  EXPECT_EQ(parsed->addressed, std::vector<std::string>{"Zenji"});
  const auto all = ParseLeaderCommand("everyone follow", kBots);
  ASSERT_TRUE(all);
  EXPECT_TRUE(all->addressed.empty());
}

}  // namespace
}  // namespace llm_brain
