// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/plan.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "grammar/orders.wlb.h"
#include "grammar/plan.wlb.h"
#include "grammar/wlb_runtime.h"
#include "gtest/gtest.h"
#include "plan/orders.h"
#include "test/gbnf_check.h"

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

TEST(PlanTest, MistypedOrdersLineFailsParseInsteadOfSilentDrop) {
  const std::vector<std::string> names = {"Fire"};
  EXPECT_TRUE(
      ParsePlan("do: 1\nexpect: none\nwatch: none\nlook: 60\norders: none", 2,
                names)
          .has_value());
  EXPECT_FALSE(ParsePlan("do: 1\nexpect: none\nwatch: none\nlook: 60\n"
                         "orders: in Fire -> out of it",
                         2, names)
                   .has_value());
  EXPECT_FALSE(ParsePlan("do: 1\nexpect: none\nwatch: none\nlook: 60\n"
                         "orders: clear",
                         2, names)
                   .has_value());
}

TEST(PlanTest, ATellLineIsAWhisperToOnePlayer) {
  const std::optional<Plan> plan = ParsePlan(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "say: hello all\nparty: oom, drinking\n"
      "tell Gatda: sure, inviting you now",
      2);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->say, "hello all");
  EXPECT_EQ(plan->party, "oom, drinking");
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

// `act:` is up to 3 order steps done at once; like `order:` it needs names
// to use, and `ping` is offered in it only while a ping is on the screen.
TEST(PlanTest, ActLinesCarryAtMostThreeImmediateActs) {
  const std::optional<Plan> plan = ParsePlan(
      "do: 1\nexpect: none\nwatch: none\nlook: 60\n"
      "act: use Polymorph on marker star, then to ping, then pet dismiss",
      1, {"Polymorph"});
  ASSERT_TRUE(plan);
  ASSERT_EQ(plan->acts.size(), 3u);
  EXPECT_TRUE(std::holds_alternative<Act::Use>(plan->acts[0].value));
  EXPECT_TRUE(std::holds_alternative<Act::To>(plan->acts[1].value));
  EXPECT_EQ(std::get<Act::Pet>(plan->acts[2].value).cmd,
            grammar::PetCmd::kDismiss);
  EXPECT_TRUE(plan->orders.empty());
  EXPECT_EQ(ParsePlan(FormatPlan(*plan), 1, {"Polymorph"}), plan);
  EXPECT_NE(FormatPlan(*plan).find(
                "\nact: use Polymorph on marker star, then to ping, then pet "
                "dismiss"),
            std::string::npos);

  // Four acts, or a junk one: the line is dropped, the plan stands.
  for (const char* bad :
       {"act: jump, then jump, then jump, then jump", "act: dance", "act: "}) {
    const std::optional<Plan> dropped = ParsePlan(
        std::string("do: 1\nexpect: none\nwatch: none\nlook: 60\n") + bad, 1);
    ASSERT_TRUE(dropped) << bad;
    EXPECT_TRUE(dropped->acts.empty()) << bad;
  }
  // No `act:` line, no acts (and FormatPlan writes none).
  const std::optional<Plan> none = ParsePlan(kFullPlan, 5);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->acts.empty());
  EXPECT_EQ(FormatPlan(*none).find("act:"), std::string::npos);

  // The grammar: no names, no `act:`; `ping` only with a pinger.
  EXPECT_FALSE(Accepts(PlanGrammar(1), "act: jump"));
  const std::string no_ping = PlanGrammar(1, {}, {}, {"Sap"});
  EXPECT_TRUE(Accepts(no_ping, "act: use Sap on marker diamond, then back"));
  EXPECT_TRUE(Accepts(no_ping, "order: always -> jump\nact: pet dismiss"));
  EXPECT_FALSE(Accepts(no_ping, "act: to ping"));
  EXPECT_FALSE(Accepts(no_ping, "act: jump, then jump, then jump, then jump"));
  const std::string ping = PlanGrammar(1, {}, {}, {"Sap"}, {"Hele"});
  EXPECT_TRUE(Accepts(ping, "act: to ping"));
  EXPECT_TRUE(
      Accepts(ping, "order: ping within 3 -> back\nact: use Sap on ping"));
  // A pinger without order names doesn't turn `act:` on.
  EXPECT_FALSE(Accepts(PlanGrammar(1, {}, {}, {}, {"Hele"}), "act: to ping"));
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
  const std::string grammar = PlanGrammar(9, names, names, names, {"Hele"});
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

TEST(PlanTest, StripTruncatedLastLineDropsCutLineOnLengthFinishReason) {
  const std::string_view cut =
      "do: 1\nlook: 30\norder: boss casts Nova -> away boss 2";
  // Normal stop keeps everything.
  EXPECT_EQ(StripTruncatedLastLine(cut, "stop"), cut);
  // Length truncation drops the cut last line so `away boss 2` (cut from 25)
  // is not installed.
  EXPECT_EQ(StripTruncatedLastLine(cut, "length"), "do: 1\nlook: 30");
  // If the reply ended right on a newline, no line was cut mid-line.
  EXPECT_EQ(StripTruncatedLastLine("do: 1\nlook: 30\n", "length"),
            "do: 1\nlook: 30\n");
}

}  // namespace
}  // namespace harness
