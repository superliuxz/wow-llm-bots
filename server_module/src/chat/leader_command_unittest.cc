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
  EXPECT_EQ(Cmd("stop"), LeaderCommand::kStop);
  EXPECT_EQ(Cmd("Stop!"), LeaderCommand::kStop);
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("stop", kBots)), "stop");
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
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("heroism", kBots)),
            "lust");
}

TEST(LeaderCommandTest, LustCommandsAndCounterTests) {
  EXPECT_EQ(Cmd("lust"), LeaderCommand::kLust);
  EXPECT_EQ(Cmd("Lust!"), LeaderCommand::kLust);
  EXPECT_EQ(Cmd("lust now"), LeaderCommand::kLust);
  EXPECT_EQ(Cmd("bloodlust pls"), LeaderCommand::kLust);
  EXPECT_EQ(Cmd("BL"), LeaderCommand::kLust);
  EXPECT_EQ(Cmd("hero"), LeaderCommand::kLust);
  EXPECT_EQ(Cmd("heroism guys"), LeaderCommand::kLust);
  const auto addressed = ParseLeaderCommand("Zenji, lust", kBots);
  ASSERT_TRUE(addressed);
  EXPECT_EQ(addressed->command, LeaderCommand::kLust);
  EXPECT_EQ(addressed->addressed, std::vector<std::string>{"Zenji"});
  // Adversarial counter-tests: sentences discussing lust or similar words must
  // not trigger an immediate cast.
  EXPECT_FALSE(Cmd("save lust for phase 2"));
  EXPECT_FALSE(Cmd("no lust yet"));
  EXPECT_FALSE(Cmd("don't hero"));
  EXPECT_FALSE(Cmd("who has bloodlust?"));
  EXPECT_FALSE(Cmd("heroic"));
  EXPECT_FALSE(Cmd("blood"));
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

TEST(LeaderCommandTest, RaidTacticsSpreadStackStopAndResumeDps) {
  EXPECT_EQ(Cmd("spread"), LeaderCommand::kSpread);
  EXPECT_EQ(Cmd("Spread out guys!"), LeaderCommand::kSpread);
  EXPECT_EQ(Cmd("everyone spread now"), LeaderCommand::kSpread);
  EXPECT_EQ(Cmd("stack"), LeaderCommand::kStack);
  EXPECT_EQ(Cmd("stack up pls"), LeaderCommand::kStack);
  EXPECT_EQ(Cmd("stack on me"), LeaderCommand::kStack);
  EXPECT_EQ(Cmd("stack here"), LeaderCommand::kStack);
  EXPECT_EQ(Cmd("group up"), LeaderCommand::kStack);
  EXPECT_EQ(Cmd("clump up"), LeaderCommand::kStack);
  EXPECT_EQ(Cmd("stop dps"), LeaderCommand::kStopDps);
  EXPECT_EQ(Cmd("dps stop!"), LeaderCommand::kStopDps);
  EXPECT_EQ(Cmd("hold dps everyone"), LeaderCommand::kStopDps);
  EXPECT_EQ(Cmd("cease fire"), LeaderCommand::kStopDps);
  EXPECT_EQ(Cmd("stop attacking"), LeaderCommand::kStopDps);
  EXPECT_EQ(Cmd("hands off"), LeaderCommand::kStopDps);
  EXPECT_EQ(Cmd("resume"), LeaderCommand::kResumeDps);
  EXPECT_EQ(Cmd("resume dps"), LeaderCommand::kResumeDps);
  EXPECT_EQ(Cmd("dps now"), LeaderCommand::kResumeDps);
  EXPECT_EQ(Cmd("burn!"), LeaderCommand::kResumeDps);
  EXPECT_EQ(Cmd("nuke"), LeaderCommand::kResumeDps);

  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("spread", kBots)),
            "spread");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("stack up", kBots)),
            "stack");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("stop dps", kBots)),
            "stop dps");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("resume dps", kBots)),
            "resume dps");

  // Adversarial counter-tests: casual sentences discussing spread/stack/dps
  // must never parse as fast C++ leader commands.
  EXPECT_FALSE(Cmd("don't spread yet"));
  EXPECT_FALSE(Cmd("should we stack on the boss?"));
  EXPECT_FALSE(Cmd("nice dps guys"));
  EXPECT_FALSE(Cmd("who stopped dps?"));
  EXPECT_FALSE(Cmd("stop the patrol"));
  EXPECT_FALSE(Cmd("stack on tank"));

  // M11: wipe / release commands trigger immediate spirit release.
  EXPECT_EQ(Cmd("wipe"), LeaderCommand::kWipe);
  EXPECT_EQ(Cmd("wipe it guys"), LeaderCommand::kWipe);
  EXPECT_EQ(Cmd("call wipe"), LeaderCommand::kWipe);
  EXPECT_EQ(Cmd("release"), LeaderCommand::kWipe);
  EXPECT_EQ(Cmd("release spirit everyone"), LeaderCommand::kWipe);
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("wipe", kBots)), "wipe");
  // Counter-test: casual sentence mentioning wipe/release is not a command.
  EXPECT_FALSE(Cmd("we might wipe if we pull"));
  EXPECT_FALSE(Cmd("don't release yet"));

  // M15: go repair / go train errands wake and direct bots to repair/train.
  EXPECT_EQ(Cmd("go repair"), LeaderCommand::kGoRepair);
  EXPECT_EQ(Cmd("repair"), LeaderCommand::kGoRepair);
  EXPECT_EQ(Cmd("repair gear guys"), LeaderCommand::kGoRepair);
  EXPECT_EQ(Cmd("go vendor"), LeaderCommand::kGoRepair);
  EXPECT_EQ(Cmd("sell and repair"), LeaderCommand::kGoRepair);
  EXPECT_EQ(Cmd("go train"), LeaderCommand::kGoTrain);
  EXPECT_EQ(Cmd("train"), LeaderCommand::kGoTrain);
  EXPECT_EQ(Cmd("train spells everyone"), LeaderCommand::kGoTrain);
  EXPECT_EQ(Cmd("go train spells"), LeaderCommand::kGoTrain);
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("go repair", kBots)),
            "go repair");
  EXPECT_EQ(DescribeLeaderCommand(*ParseLeaderCommand("go train", kBots)),
            "go train");
  const auto named_repair = ParseLeaderCommand("Loada go repair", kBots);
  ASSERT_TRUE(named_repair);
  EXPECT_EQ(named_repair->command, LeaderCommand::kGoRepair);
  EXPECT_EQ(named_repair->addressed, (std::vector<std::string>{"Loada"}));
  // Adversarial counter-test: casual sentences mentioning repair/train are not
  // leader commands.
  EXPECT_FALSE(Cmd("I need to repair my gear later"));
  EXPECT_FALSE(Cmd("did you train your new spells?"));
  EXPECT_FALSE(Cmd("let's hit the vendor after this quest"));

  // M11: tanks are excluded from /rw spread and /rw stack by default so they
  // don't spin or drag the boss, unless explicitly addressed by name.
  EXPECT_FALSE(ShouldApplySpreadOrStack(/*is_tank=*/true,
                                        /*explicitly_addressed=*/false));
  EXPECT_TRUE(ShouldApplySpreadOrStack(/*is_tank=*/false,
                                       /*explicitly_addressed=*/false));
  EXPECT_TRUE(ShouldApplySpreadOrStack(/*is_tank=*/true,
                                       /*explicitly_addressed=*/true));
}

TEST(LeaderCommandTest, AuthorizationIgnoresNonLeadersAndBots) {
  const auto unaddressed = ParseLeaderCommand("spread", kBots);
  ASSERT_TRUE(unaddressed);
  // Human leader reaches every bot in the group/raid.
  EXPECT_TRUE(IsLeaderCommandForBot(/*sender_is_human=*/true,
                                    /*sender_is_leader=*/true,
                                    /*leader_is_human=*/true,
                                    /*is_whisper=*/false, *unaddressed,
                                    "Loada"));
  EXPECT_TRUE(IsLeaderCommandForBot(/*sender_is_human=*/true,
                                    /*sender_is_leader=*/true,
                                    /*leader_is_human=*/true,
                                    /*is_whisper=*/false, *unaddressed,
                                    "Zenji"));
  // Counter-test: non-leader human unaddressed call is ignored when another
  // human is group leader.
  EXPECT_FALSE(IsLeaderCommandForBot(/*sender_is_human=*/true,
                                     /*sender_is_leader=*/false,
                                     /*leader_is_human=*/true,
                                     /*is_whisper=*/false, *unaddressed,
                                     "Loada"));
  // Bot-led group: human group member's unaddressed party/raid command applies.
  EXPECT_TRUE(IsLeaderCommandForBot(/*sender_is_human=*/true,
                                    /*sender_is_leader=*/false,
                                    /*leader_is_human=*/false,
                                    /*is_whisper=*/false, *unaddressed,
                                    "Loada"));
  // Whisper from a human group member applies to the whispered bot even when
  // another human is group leader.
  EXPECT_TRUE(IsLeaderCommandForBot(/*sender_is_human=*/true,
                                    /*sender_is_leader=*/false,
                                    /*leader_is_human=*/true,
                                    /*is_whisper=*/true, *unaddressed,
                                    "Loada"));
  // Counter-test: bot sender is always ignored, even if group leader or
  // whispering.
  EXPECT_FALSE(IsLeaderCommandForBot(/*sender_is_human=*/false,
                                     /*sender_is_leader=*/true,
                                     /*leader_is_human=*/false,
                                     /*is_whisper=*/false, *unaddressed,
                                     "Loada"));
  EXPECT_FALSE(IsLeaderCommandForBot(/*sender_is_human=*/false,
                                     /*sender_is_leader=*/true,
                                     /*leader_is_human=*/false,
                                     /*is_whisper=*/true, *unaddressed,
                                     "Loada"));
  // Addressed command reaches only the named bot (in party or whisper).
  const auto addressed = ParseLeaderCommand("Zenji stop dps", kBots);
  ASSERT_TRUE(addressed);
  EXPECT_TRUE(IsLeaderCommandForBot(/*sender_is_human=*/true,
                                    /*sender_is_leader=*/false,
                                    /*leader_is_human=*/true,
                                    /*is_whisper=*/false, *addressed, "Zenji"));
  EXPECT_FALSE(IsLeaderCommandForBot(/*sender_is_human=*/true,
                                     /*sender_is_leader=*/true,
                                     /*leader_is_human=*/true,
                                     /*is_whisper=*/false, *addressed,
                                     "Loada"));
  EXPECT_FALSE(IsLeaderCommandForBot(/*sender_is_human=*/true,
                                     /*sender_is_leader=*/true,
                                     /*leader_is_human=*/true,
                                     /*is_whisper=*/true, *addressed, "Loada"));
}

}  // namespace
}  // namespace llm_brain
