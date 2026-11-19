// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The generator (tools/grammar/wlb_gen.py) on a grammar with one of each
// construct (testdata/constructs.wlb): what the generated code parses, prints
// and offers in its GBNF.

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "grammar/constructs.wlb.h"
#include "grammar/wlb_runtime.h"
#include "gtest/gtest.h"
#include "test/gbnf_check.h"

namespace harness::grammar {
namespace {

rt::Env TestEnv(std::vector<std::string> labels,
                std::vector<std::string> others) {
  return {
      .names = {{"labels", std::move(labels)}, {"others", std::move(others)}},
      .counts = {}};
}

TEST(ConstructsTest, WordsMatchAnyCaseAndTheirAliasesButPrintTheirSpelling) {
  EXPECT_EQ(ParseColor("RED"), Color::kRed);
  EXPECT_EQ(ParseColor("navy"), Color::kDarkBlue);
  EXPECT_EQ(ParseColor("darkblue"), Color::kDarkBlue);
  EXPECT_FALSE(ParseColor("blue"));
  EXPECT_EQ(FormatColor(Color::kDarkBlue), "dark blue");
  // The GBNF offers only the spelling.
  const std::string grammar = ConstructsGrammar("Color", TestEnv({"x"}, {"y"}));
  EXPECT_NE(grammar.find("\"dark blue\""), std::string::npos) << grammar;
  EXPECT_EQ(grammar.find("navy"), std::string::npos) << grammar;
}

TEST(ConstructsTest, ParseOnlyLiteralsAreAcceptedButNeverPrintedOrOffered) {
  const std::optional<Line> line = ParseLine("hp < 3% dot");
  ASSERT_TRUE(line);
  EXPECT_EQ(line->n, 3);
  EXPECT_FALSE(line->bold);
  EXPECT_TRUE(std::holds_alternative<Shape::Dot>(line->shape.value));
  EXPECT_EQ(FormatLine(*line), "hp3 dot");
  EXPECT_EQ(ParseLine("hp3 dot"), line);
  const std::string grammar = ConstructsGrammar("Line", TestEnv({"x"}, {}));
  EXPECT_EQ(grammar.find('%'), std::string::npos) << grammar;
  EXPECT_EQ(grammar.find("\"<\""), std::string::npos) << grammar;
}

TEST(ConstructsTest, FlagsOptionalsAndRanges) {
  const std::optional<Line> line = ParseLine("bold hp5 circle 2.5 in green");
  ASSERT_TRUE(line);
  EXPECT_TRUE(line->bold);
  const auto& circle = std::get<Shape::Circle>(line->shape.value);
  EXPECT_EQ(circle.r, 2.5);
  EXPECT_EQ(circle.color, Color::kGreen);
  EXPECT_EQ(FormatLine(*line), "bold hp5 circle 2.5 in green");
  EXPECT_FALSE(ParseLine("hp6 dot"));          // Small is 1..5.
  EXPECT_FALSE(ParseLine("hp1 circle 2.55"));  // One decimal at most.
  EXPECT_FALSE(ParseLine("hp1 circle 10"));    // Ratio is 0..9.
  EXPECT_FALSE(ParseLine("hp1 dot "));         // The whole text, no more.
}

TEST(ConstructsTest, ListsWithinTheirBounds) {
  for (const char* text :
       {"hp1 pairs:.", "hp1 pairs:1/2.", "hp1 pairs:1/2,3/4,5/5."}) {
    const std::optional<Line> line = ParseLine(text);
    ASSERT_TRUE(line) << text;
    EXPECT_EQ(FormatLine(*line), text);
  }
  EXPECT_FALSE(ParseLine("hp1 pairs:1/2,3/4,5/5,1/1."));  // At most 3.
  EXPECT_FALSE(ParseLine("hp1 pairs:1/2,."));
}

TEST(ConstructsTest, AnEmptyNameListPrunesWhatNeedsIt) {
  // No labels: the Tagged arm can't be written, so it isn't offered.
  std::string grammar = ConstructsGrammar("Shape", TestEnv({}, {"Other"}));
  EXPECT_EQ(grammar.find("\"tag \""), std::string::npos) << grammar;
  EXPECT_NE(grammar.find("\"dot\""), std::string::npos) << grammar;
  // Labels but no others: Tagged without its optional " and " part.
  grammar = ConstructsGrammar("Shape", TestEnv({"Label One"}, {}));
  EXPECT_NE(grammar.find("\"tag \" constructs-Label"), std::string::npos)
      << grammar;
  EXPECT_EQ(grammar.find("\" and \""), std::string::npos) << grammar;
  // A root that needs an empty list: no grammar at all.
  EXPECT_EQ(ConstructsGrammar("Label", TestEnv({}, {})), "");
}

TEST(ConstructsTest, RandomValuesRoundTripAndTheGrammarAcceptsThem) {
  rt::RandomSource source(7, {"Label One", "Kel'Thuzad", "SI:7"});
  for (int i = 0; i < 500; ++i) {
    const Line line = RandomLine(&source);
    const std::string text = FormatLine(line);
    EXPECT_EQ(ParseLine(text), line) << text;
    std::vector<std::string> names;
    CollectNames(line, &names);
    const GbnfResult result =
        GbnfAccepts(ConstructsGrammar("Line", TestEnv(names, names)),
                    ConstructsRuleName("Line"), text);
    EXPECT_TRUE(result.grammar_ok);
    EXPECT_TRUE(result.accepted) << text;
  }
}

}  // namespace
}  // namespace harness::grammar
