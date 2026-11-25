// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/leader_command.h"

#include <cstddef>
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
  EXPECT_EQ(Cmd("kill skull"), LeaderCommand::kAttackMark);
  EXPECT_EQ(Cmd("drink up guys"), LeaderCommand::kDrink);
  EXPECT_EQ(Cmd("mount up"), LeaderCommand::kMount);
  EXPECT_EQ(Cmd("@heal"), LeaderCommand::kHealMe);
}

std::optional<size_t> Mark(const char* text) {
  const auto parsed = ParseLeaderCommand(text, kBots);
  return parsed ? parsed->mark : std::nullopt;
}

TEST(LeaderCommandTest, MarksNameTheirIcon) {
  EXPECT_EQ(Cmd("cross"), LeaderCommand::kAttackMark);
  EXPECT_EQ(Mark("cross"), 6u);
  EXPECT_EQ(Cmd("Skull!"), LeaderCommand::kAttackMark);
  EXPECT_EQ(Mark("skull"), 7u);
  EXPECT_EQ(Cmd("kill cross pls"), LeaderCommand::kAttackMark);
  EXPECT_EQ(Cmd("focus star"), LeaderCommand::kAttackMark);
  EXPECT_EQ(Mark("focus star"), 0u);
  EXPECT_EQ(Cmd("rti rt8"), LeaderCommand::kAttackMark);
  EXPECT_EQ(Mark("rti rt8"), 7u);
  // A CC mark alone, or with any other verb, is the brain's to read.
  EXPECT_FALSE(Cmd("moon"));
  EXPECT_FALSE(Cmd("square"));
  EXPECT_FALSE(Cmd("sheep moon"));
  EXPECT_FALSE(Cmd("kill x"));
  EXPECT_FALSE(Cmd("x"));
  EXPECT_FALSE(Cmd("skull then cross"));
}

TEST(LeaderCommandTest, MovesAndPulls) {
  EXPECT_EQ(Cmd("go there"), LeaderCommand::kMoveToPing);
  EXPECT_EQ(Cmd("flag"), LeaderCommand::kMoveToPing);
  EXPECT_EQ(Cmd("go to the ping pls"), LeaderCommand::kMoveToPing);
  EXPECT_EQ(Cmd("LOS!"), LeaderCommand::kPullBack);
  EXPECT_EQ(Cmd("pull back"), LeaderCommand::kPullBack);
  EXPECT_EQ(Cmd("fall back guys"), LeaderCommand::kPullBack);
  EXPECT_EQ(Cmd("pull"), LeaderCommand::kPull);
  EXPECT_FALSE(Mark("pull"));
  EXPECT_EQ(Cmd("pull now"), LeaderCommand::kPull);
  EXPECT_EQ(Cmd("pull skull"), LeaderCommand::kPull);
  EXPECT_EQ(Mark("pull skull"), 7u);
  EXPECT_EQ(Cmd("pull {cross}"), LeaderCommand::kPull);
  EXPECT_EQ(Mark("pull cross"), 6u);
  // Talk about a pull is not one.
  EXPECT_FALSE(Cmd("pulling"));
  EXPECT_FALSE(Cmd("don't pull"));
  EXPECT_FALSE(Cmd("wait for the pull"));
  EXPECT_FALSE(Cmd("who pulls?"));
  EXPECT_FALSE(Cmd("there"));
  EXPECT_FALSE(Cmd("go there and pull the boss"));
  EXPECT_FALSE(Cmd("i was los"));
}

TEST(LeaderCommandTest, DescribesWhatWasSaid) {
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("kill skull", kBots)),
            "kill skull");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("cross", kBots)),
            "kill cross");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("pull star", kBots)),
            "pull star");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("pull", kBots)), "pull");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("los", kBots)),
            "pull back");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("wait", kBots)), "wait");
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
  const auto puller = ParseLeaderCommand("Loada pull skull", kBots);
  ASSERT_TRUE(puller);
  EXPECT_EQ(puller->command, LeaderCommand::kPull);
  EXPECT_EQ(puller->addressed, std::vector<std::string>{"Loada"});
  const auto all = ParseLeaderCommand("everyone follow", kBots);
  ASSERT_TRUE(all);
  EXPECT_TRUE(all->addressed.empty());
}

}  // namespace
}  // namespace llm_brain
