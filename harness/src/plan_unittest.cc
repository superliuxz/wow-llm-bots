// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gbnf_check.h"
#include "grammar/plan.wlb.h"
#include "grammar/wlb_runtime.h"
#include "gtest/gtest.h"
#include "orders.h"

namespace harness {
namespace {

// Whether `grammar` (what llama-server is held to) accepts a plan: the
// required lines, then `tail` (optional lines, each after a newline). With
// `whole` set, that is the whole text instead.
bool Accepts(const std::string& grammar,
             std::string_view tail,
             std::string_view whole = {}) {
  std::string text =
      whole.empty() ? std::string("do: 1\nexpect: none\nwatch: none\nlook: 60")
                    : std::string(whole);
  if (whole.empty() && !tail.empty()) {
    text += "\n" + std::string(tail);
  }
  const GbnfResult result = GbnfAccepts(grammar, "root", text);
  EXPECT_TRUE(result.grammar_ok) << grammar;
  return result.accepted;
}

constexpr char kFullPlan[] =
    "do: 3x4, 5\n"
    "expect: 3\n"
    "watch: hp<40, enemy_player<40\n"
    "look: 60\n"
    "goal: kill boars for the quest\n"
    "say: anyone want to group?";

TEST(PlanTest, ParsesEveryField) {
  const std::optional<Plan> plan = ParsePlan(kFullPlan, 6);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->goal, "kill boars for the quest");
  EXPECT_EQ(plan->steps, (std::vector<PlanStep>{{3, 4}, {5, 1}}));
  EXPECT_EQ(plan->expect, std::vector<int>{3});
  EXPECT_EQ(plan->watches,
            (std::vector<Watch>{{Watch::Kind::kHealthBelow, 40},
                                {Watch::Kind::kEnemyPlayerWithin, 40}}));
  EXPECT_EQ(plan->look_s, 60);
  EXPECT_EQ(plan->say, "anyone want to group?");
}

TEST(PlanTest, FormatAndParseRoundTrip) {
  const std::optional<Plan> plan = ParsePlan(kFullPlan, 6);
  ASSERT_TRUE(plan);
  EXPECT_EQ(FormatPlan(*plan), kFullPlan);
  EXPECT_EQ(ParsePlan(FormatPlan(*plan), 6), plan);
}

TEST(PlanTest, ExpectWatchAndTheGoalAreOptional) {
  const std::optional<Plan> plan = ParsePlan("do: 1\nlook: 30", 4);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->goal, "");
  EXPECT_TRUE(plan->expect.empty());
  EXPECT_TRUE(plan->watches.empty());
  EXPECT_EQ(plan->say, "");
  EXPECT_EQ(FormatPlan(*plan), "do: 1\nexpect: none\nwatch: none\nlook: 30");
  const std::optional<Plan> none =
      ParsePlan("do: 2\nexpect: none\nwatch: none\nlook: 15", 4);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->expect.empty());
  EXPECT_TRUE(none->watches.empty());
}

TEST(PlanTest, DoAndLookAreRequired) {
  EXPECT_FALSE(ParsePlan("look: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1", 4));
  EXPECT_FALSE(ParsePlan("", 4));
  EXPECT_FALSE(ParsePlan("I would attack the boar.", 4));
}

TEST(PlanTest, OptionsMustExist) {
  EXPECT_TRUE(ParsePlan("do: 4\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 5\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 0\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1, 9\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nexpect: 7\nlook: 30", 4));
}

TEST(PlanTest, CountsAreTwoToTwentyNine) {
  EXPECT_TRUE(ParsePlan("do: 1x2\nlook: 30", 4));
  EXPECT_TRUE(ParsePlan("do: 1x29\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1x1\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1x30\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1x\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: x3\nlook: 30", 4));
  // What the bot expects is a list of options, not steps.
  EXPECT_FALSE(ParsePlan("do: 1\nexpect: 2x3\nlook: 30", 4));
}

TEST(PlanTest, AtMostFiveStepsAndFourWatches) {
  EXPECT_TRUE(ParsePlan("do: 1, 2, 3, 4, 1\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1, 2, 3, 4, 1, 2\nlook: 30", 4));
  EXPECT_TRUE(ParsePlan(
      "do: 1\nwatch: hp<10, mana<10, attacked, whisper\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan(
      "do: 1\nwatch: hp<10, mana<10, attacked, whisper, hp<20\nlook: 30", 4));
}

TEST(PlanTest, LookIsTenToNineHundredNinetyNineSeconds) {
  EXPECT_TRUE(ParsePlan("do: 1\nlook: 10", 4));
  EXPECT_TRUE(ParsePlan("do: 1\nlook: 999", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nlook: 9", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nlook: 0", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nlook: 1000", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nlook: soon", 4));
}

TEST(PlanTest, WatchesUseTheFixedVocabulary) {
  EXPECT_FALSE(ParsePlan("do: 1\nwatch: boredom\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nwatch: hp<0\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nwatch: hp<100\nlook: 30", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nwatch: enemy_player<1000\nlook: 30", 4));
  const std::optional<Plan> plan =
      ParsePlan("do: 1\nwatch: attacked, whisper, mana<25\nlook: 30", 4);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->watches, (std::vector<Watch>{{Watch::Kind::kAttacked, 0},
                                               {Watch::Kind::kWhisper, 0},
                                               {Watch::Kind::kManaBelow, 25}}));
}

TEST(PlanTest, ForgivesFormattingSlips) {
  // A model that ignores the grammar: brackets, spaces, a multiplication
  // sign, "hp20" for "hp<20", a unit after the number.
  const std::optional<Plan> plan = ParsePlan(
      "Goal: loot\n"
      "DO: [5], 3 x 10, 2×4\n"
      "expect: [3]\n"
      "watch: hp20, mana < 30%, enemy_player 40yd\n"
      "look: 60 seconds",
      6);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->steps, (std::vector<PlanStep>{{5, 1}, {3, 10}, {2, 4}}));
  EXPECT_EQ(plan->expect, std::vector<int>{3});
  EXPECT_EQ(plan->watches,
            (std::vector<Watch>{{Watch::Kind::kHealthBelow, 20},
                                {Watch::Kind::kManaBelow, 30},
                                {Watch::Kind::kEnemyPlayerWithin, 40}}));
  EXPECT_EQ(plan->look_s, 60);
}

TEST(PlanTest, LongGoalsAndChatAreCut) {
  const std::optional<Plan> plan =
      ParsePlan("goal: " + std::string(100, 'g') +
                    "\ndo: 1\nlook: 15\nsay: " + std::string(200, 's'),
                4);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->goal.size(), static_cast<size_t>(kMaxGoalChars));
  EXPECT_EQ(plan->say.size(), static_cast<size_t>(kMaxSayChars));
}

TEST(PlanTest, APlanWithoutAGoalOmitsGoalInFormatPlan) {
  // A model may leave the goal out, write "goal: none", or write one that
  // cutting empties (a lone continuation byte).
  const std::string cut_to_nothing =
      "do: 1\nlook: 15\ngoal: " + std::string(kMaxGoalChars + 1, '\x80');
  for (const std::string& reply :
       {std::string("do: 1\nlook: 15"),
        std::string("do: 1\nlook: 15\ngoal: none"), cut_to_nothing}) {
    const std::optional<Plan> plan = ParsePlan(reply, 4);
    ASSERT_TRUE(plan) << reply;
    EXPECT_EQ(plan->goal, "");
    const std::string text = FormatPlan(*plan);
    EXPECT_EQ(text.find("goal:"), std::string::npos) << text;
    EXPECT_EQ(ParsePlan(text, 4)->goal, "");
  }
}

TEST(PlanTest, ParsePlanStepsLineReadsFirstStreamedLine) {
  EXPECT_EQ(ParsePlanStepsLine("do: 3x4, 5", 6),
            (std::optional<std::vector<PlanStep>>{{{3, 4}, {5, 1}}}));
  EXPECT_EQ(ParsePlanStepsLine("  DO: [1] \r", 4),
            (std::optional<std::vector<PlanStep>>{{{1, 1}}}));
  EXPECT_FALSE(ParsePlanStepsLine("do: 7", 4));
  EXPECT_FALSE(ParsePlanStepsLine("goal: boars", 4));
  EXPECT_FALSE(ParsePlanStepsLine("do: 1\nlook: 30", 4));
  EXPECT_FALSE(ParsePlanStepsLine("", 4));
}

TEST(PlanTest, LookTakesMinutesToo) {
  EXPECT_EQ(ParsePlan("do: 1\nlook: 2m", 4)->look_s, 120);
  EXPECT_EQ(ParsePlan("do: 1\nlook: 5 minutes", 4)->look_s, 300);
  EXPECT_EQ(ParsePlan("do: 1\nlook: 90s", 4)->look_s, 90);
  EXPECT_FALSE(ParsePlan("do: 1\nlook: 2 hours", 4));
  EXPECT_FALSE(ParsePlan("do: 1\nlook: 20 minutes", 4));  // Over 999 s.
}

TEST(PlanTest, FreeTextKeepsItsBrackets) {
  const std::optional<Plan> plan = ParsePlan(
      "goal: get [Linen Cloth] (x10)\ndo: [1]\nlook: 15\nsay: lfg :)", 4);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->goal, "get [Linen Cloth] (x10)");
  EXPECT_EQ(plan->say, "lfg :)");
}

TEST(PlanTest, CuttingNeverSplitsACharacter) {
  // "é" is two bytes: cut at an odd byte count it must be dropped whole.
  std::string goal;
  for (int i = 0; i < kMaxGoalChars; ++i) {
    goal += "\xC3\xA9";
  }
  const std::optional<Plan> plan =
      ParsePlan("goal: x" + goal + "\ndo: 1\nlook: 15", 4);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->goal.size(), static_cast<size_t>(kMaxGoalChars - 1));
  EXPECT_EQ(plan->goal.back(), '\xA9');
}

// `tell` may answer only who whispered: no invented names, not the bot itself.
TEST(PlanTest, TellNamesAreTheWhisperers) {
  const std::string grammar =
      PlanGrammar(2, {"Brakka", "Gatda", "Bad Name", "X"});
  EXPECT_TRUE(Accepts(grammar, "tell Brakka: sure"));
  EXPECT_TRUE(Accepts(grammar, "tell Gatda: on my way"));
  EXPECT_FALSE(Accepts(grammar, "tell Kelra: hi"));     // Didn't whisper.
  EXPECT_FALSE(Accepts(grammar, "tell Bad Name: hi"));  // Not a name.
  EXPECT_FALSE(Accepts(grammar, "tell X: hi"));
  EXPECT_FALSE(Accepts(grammar, "tell Brakka: a|link"));  // No '|' in chat.
  EXPECT_FALSE(Accepts(PlanGrammar(2), "tell Brakka: sure"));
}

TEST(PlanTest, TheGrammarOnlyAllowsTheBotsOwnOptions) {
  const std::string grammar = PlanGrammar(3);
  EXPECT_TRUE(Accepts(grammar, "",
                      "do: 3, 1x29\nexpect: 3\nwatch: hp<40, "
                      "enemy_player<120, whisper\nlook: 999"));
  EXPECT_FALSE(
      Accepts(grammar, "", "do: 4\nexpect: none\nwatch: none\nlook: 60"));
  EXPECT_FALSE(
      Accepts(grammar, "", "do: 1x30\nexpect: none\nwatch: none\nlook: 60"));
  EXPECT_FALSE(
      Accepts(grammar, "", "do: 1x1\nexpect: none\nwatch: none\nlook: 60"));
  EXPECT_FALSE(
      Accepts(grammar, "", "do: 1\nexpect: none\nwatch: hp<100\nlook: 60"));
  EXPECT_FALSE(
      Accepts(grammar, "", "do: 1\nexpect: none\nwatch: none\nlook: 9"));
  EXPECT_FALSE(Accepts(grammar, "",
                       "do: 1, 1, 1, 1, 1, 1\nexpect: none\n"
                       "watch: none\nlook: 60"));
  // The lines come in RULES.md's order.
  EXPECT_FALSE(
      Accepts(grammar, "", "do: 1\nwatch: none\nexpect: none\nlook: 60"));
  EXPECT_TRUE(Accepts(grammar, "goal: hunt\nsay: hi"));
  EXPECT_FALSE(Accepts(grammar, "say: hi\ngoal: hunt"));
  EXPECT_TRUE(Accepts(PlanGrammar(12), "",
                      "do: 12\nexpect: none\nwatch: none\n"
                      "look: 60"));
  // No options at all would be an invalid grammar: there is always one.
  EXPECT_TRUE(Accepts(PlanGrammar(0), ""));
}

TEST(PlanTest, ATellLineIsAWhisperToOnePlayer) {
  const std::optional<Plan> plan = ParsePlan(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "say: hello all\ntell Gatda: sure, inviting you now",
      2);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->say, "hello all");
  EXPECT_EQ(plan->tell_to, "Gatda");
  EXPECT_EQ(plan->tell, "sure, inviting you now");
  EXPECT_EQ(ParsePlan(FormatPlan(*plan), 2), plan);
}

TEST(PlanTest, ATellNameIsCapitalizedAndABadOneIsDropped) {
  std::optional<Plan> plan = ParsePlan("do: 1\nlook: 60\ntell gATDA: hi", 1);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->tell_to, "Gatda");
  for (const char* bad : {"tell G: hi", "tell Gat da: hi", "tell Gatda1: hi",
                          "tell Abcdefghijklm: hi", "tell Gatda:   "}) {
    plan = ParsePlan(std::string("do: 1\nlook: 60\n") + bad, 1);
    ASSERT_TRUE(plan) << bad;  // The plan stands; only the whisper is lost.
    EXPECT_TRUE(plan->tell.empty()) << bad;
    EXPECT_TRUE(plan->tell_to.empty()) << bad;
  }
}

TEST(PlanTest, OpenCloseAndHoverLines) {
  const std::optional<Plan> plan = ParsePlan(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\nopen: Bags, quest log, "
      "talents\nclose: character\nhover: Tough Hunk of Bread",
      1);
  ASSERT_TRUE(plan);
  // A name that isn't a panel is dropped; the rest of the plan stands.
  EXPECT_EQ(plan->open, (std::vector<std::string>{"bags", "quest log"}));
  EXPECT_EQ(plan->close, (std::vector<std::string>{"character"}));
  EXPECT_EQ(plan->hover, "Tough Hunk of Bread");
  EXPECT_EQ(ParsePlan(FormatPlan(*plan), 1), plan);
}

TEST(PlanTest, OrderLinesAreStandingOrdersInPriorityOrder) {
  const std::optional<Plan> plan = ParsePlan(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "order: Defile on me -> away, then to behind boss\n"
      "order: this is not an order\n"
      "order: boss hp<20 -> use Execute",
      1);
  ASSERT_TRUE(plan);  // The bad order is dropped; the plan stands.
  ASSERT_EQ(plan->orders.size(), 2u);
  EXPECT_EQ(FormatOrder(plan->orders[0]),
            "Defile on me -> away, then to behind boss");
  EXPECT_EQ(FormatOrder(plan->orders[1]), "boss hp<20 -> use Execute");
  EXPECT_EQ(ParsePlan(FormatPlan(*plan), 1), plan);
}

TEST(PlanTest, OrderLinesOnlyWithNamesToUse) {
  EXPECT_FALSE(Accepts(PlanGrammar(1), "order: always -> jump"));
  const std::string grammar = PlanGrammar(1, {}, {}, {"Defile"});
  EXPECT_TRUE(Accepts(grammar, "order: Defile on me -> away"));
  EXPECT_TRUE(Accepts(grammar, "order: always -> jump\norder: always -> wake"));
  EXPECT_FALSE(Accepts(grammar, "order: Frost Bomb appears -> away"));
}

TEST(PlanTest, TheGrammarOffersHoverOnlyForNamesOnScreen) {
  EXPECT_FALSE(Accepts(PlanGrammar(1), "hover: Tough Hunk of Bread"));
  const std::string grammar =
      PlanGrammar(1, {}, {"Tough Hunk of Bread", "Say \"hi\"", "bad\nname"});
  EXPECT_TRUE(Accepts(grammar, "hover: Tough Hunk of Bread"));
  EXPECT_TRUE(Accepts(grammar, "hover: Say \"hi\""));
  EXPECT_FALSE(Accepts(grammar, "hover: bad"));
  EXPECT_TRUE(
      Accepts(grammar, "open: bags, quest log\nhover: Tough Hunk of Bread"));
}

TEST(PlanTest, RandomPlansRoundTripAndTheGrammarAcceptsThem) {
  const std::vector<std::string> names = {"Gatda", "Brakka", "Kelra"};
  const std::string grammar = PlanGrammar(9, names, names, names);
  grammar::rt::RandomSource source(11, names);
  for (int i = 0; i < 500; ++i) {
    const std::string text = grammar::FormatPlan(grammar::RandomPlan(&source));
    // Through the harness's Plan and back.
    const std::optional<Plan> plan = ParsePlan(text, 9);
    ASSERT_TRUE(plan) << text;
    EXPECT_EQ(FormatPlan(*plan), text);
    if (i % 10 == 0) {
      const GbnfResult result = GbnfAccepts(grammar, "root", text);
      EXPECT_TRUE(result.grammar_ok);
      EXPECT_TRUE(result.accepted) << text;
    }
  }
}

TEST(PlanTest, CheckPlanRecordsUsesTheRealParser) {
  const std::string records =
      std::string("2\tdo: 2\nexpect: none\nwatch: none\nlook: 60") + '\0' +
      "2\tdo: 3\nexpect: none\nwatch: none\nlook: 60" + '\0' + "junk";
  EXPECT_EQ(CheckPlanRecords(records), "1\n0\n0\n");
}

}  // namespace
}  // namespace harness
