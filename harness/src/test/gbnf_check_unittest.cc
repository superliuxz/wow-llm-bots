// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/gbnf_check.h"

#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "plan/plan.h"

namespace harness {
namespace {

TEST(GbnfCheckTest, AcceptsOnlyWhatTheGrammarAllows) {
  const char* grammar = "root ::= \"do: \" [1-3] (\", \" [1-3])?";
  EXPECT_TRUE(GbnfAccepts(grammar, "root", "do: 2").accepted);
  EXPECT_TRUE(GbnfAccepts(grammar, "root", "do: 2, 3").accepted);
  EXPECT_FALSE(GbnfAccepts(grammar, "root", "do: 4").accepted);
  EXPECT_FALSE(
      GbnfAccepts(grammar, "root", "do: 2, ").accepted);  // Unfinished.
  EXPECT_FALSE(GbnfAccepts(grammar, "root", "do: 2x").accepted);
  EXPECT_TRUE(GbnfAccepts(grammar, "root", "do: 2").grammar_ok);
}

TEST(GbnfCheckTest, ABrokenGrammarOrMissingRootIsReported) {
  EXPECT_FALSE(GbnfAccepts("root ::= (", "root", "x").grammar_ok);
  EXPECT_FALSE(GbnfAccepts("root ::= \"x\"", "nope", "x").grammar_ok);
}

// Today's hand-written grammar and printer agree: what FormatPlan writes, the
// grammar the brain is held to accepts (the property the generated code must
// keep; docs/specs/grammar-idl.md).
TEST(GbnfCheckTest, TodaysPlanGrammarAcceptsWhatFormatPlanWrites) {
  const std::optional<Plan> plan = ParsePlan(
      "do: 2x3, 1\nexpect: 2\nwatch: hp<35, whisper\nlook: 90\n"
      "goal: hunt boars\nsay: hello all\ntell Brakka: sure\n"
      "open: bags, quest log\nhover: Tough Hunk of Bread\n"
      "order: Defile on me -> away, then to behind boss",
      3);
  ASSERT_TRUE(plan);
  const std::string grammar =
      PlanGrammar(3, {"Brakka"}, {"Tough Hunk of Bread"}, {"Defile"});
  const GbnfResult result = GbnfAccepts(grammar, "root", FormatPlan(*plan));
  EXPECT_TRUE(result.grammar_ok) << grammar;
  EXPECT_TRUE(result.accepted) << FormatPlan(*plan);
}

}  // namespace
}  // namespace harness
