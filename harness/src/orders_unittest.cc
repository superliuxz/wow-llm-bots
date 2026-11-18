// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "orders.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/string_split.h"
#include "file_util.h"
#include "gtest/gtest.h"

namespace harness {
namespace {

TEST(OrdersTest, ParsesTheSpecsExample) {
  const std::optional<Order> order =
      ParseOrder("Defile on me -> away, then to behind boss");
  ASSERT_TRUE(order);
  ASSERT_EQ(order->when.conditions.size(), 1u);
  const Check& test = order->when.conditions[0].test;
  EXPECT_EQ(test.kind, Check::Kind::kOn);
  EXPECT_EQ(test.mech, "Defile");
  EXPECT_EQ(test.who, Who::kMe);
  ASSERT_EQ(order->steps.size(), 2u);
  EXPECT_EQ(order->steps[0].verb, Act::Verb::kAway);
  EXPECT_FALSE(order->steps[0].thing);
  EXPECT_EQ(order->steps[1].verb, Act::Verb::kTo);
  EXPECT_EQ(order->steps[1].place.kind, Place::Kind::kBehind);
  EXPECT_EQ(order->steps[1].place.thing.who, Who::kBoss);
}

// Each line parses, and prints back exactly as written.
TEST(OrdersTest, EveryFormRoundTrips) {
  for (const char* text : {
           "always -> target skull",
           "Mortal Strike stacks>3 on tank -> use Hand of Protection on tank",
           "Mark of the Fallen ends<5s on me -> wake",
           "Shadow Bolt Volley cast -> stop casting",
           "Impale cast on anyone -> to 8 yd from boss",
           "Whirlwind cast in last 3s -> away boss 10",
           "Shadow Trap appears -> away nearest Shadow Trap",
           "Valkyr appears in last 2.5s -> target nearest Valkyr",
           "Mirror Image count>2 -> target focus",
           "boss within 10 -> away",
           "nearest ally within 8 of me -> away nearest ally 10",
           "boss beyond 30 -> to max range of boss",
           "boss targets me -> face away boss",
           "my target hp<20 -> use Execute",
           "mana<30 & not me mounted -> use Mana Potion",
           "mana_points<500 -> use Evocation",
           "boss is submerged -> stop attacking",
           "my target is marked skull -> target marker skull",
           "focus in sight & phase 2 -> interact focus",
           "boss out of sight -> to in front of boss",
           "not me behind boss -> to behind boss",
           "my target is Hodir -> stop moving",
           "me in vehicle & every 10s -> use Chain Lightning on my target",
           "me has Explosive Bomb -> use Explosive Bomb on boss, then away, "
           "then wake",
           "after 30s -> pet attack my target",
           "ally with Unbound Plague within 5 -> away ally with Unbound Plague",
           "Gaseous Bloat appears -> kite Gaseous Bloat",
           "Frost Bomb appears -> to group, then jump",
           "boss is flying -> board Frostwing Whelp, then leave vehicle",
           "Lance Rack in sight -> equip Argent Lance, then pet defensive",
           "anyone hp<30 -> mark nearest enemy cross, then cancel Divine "
           "Shield",
           "Spirit Bomb appears & me nearest to it -> avoid boss, then face "
           "boss",
       }) {
    const std::optional<Order> order = ParseOrder(text);
    ASSERT_TRUE(order) << text;
    EXPECT_EQ(FormatOrder(*order), text);
  }
}

TEST(OrdersTest, RejectsWhatIsNotTheGrammar) {
  for (const char* text : {
           "",
           "Defile on me",                                     // No act.
           "Defile on me -> dance",                            // Not a verb.
           "Defile on somebody -> away",                       // Not a who.
           "boss hp<thirty -> away",                           // Not a number.
           "a & b & c & d & e -> wake",                        // 5 conditions.
           "always -> jump, then jump, then jump, then jump",  // 4 steps.
           "boss is dancing -> wake",                          // Not a state.
           "mark boss purple -> wake",    // A when that isn't one.
           "always -> mark boss purple",  // Not an icon.
           "always -> to ",               // No place.
           "always -> stop running",
       }) {
    EXPECT_FALSE(ParseOrder(text)) << text;
  }
}

// docs/specs/trigger-{conditions,actions}.tsv: every `fits` row must parse
// (the grammar's regression set; replaces tools/check-trigger-grammar.py).
std::vector<std::string> FitsCells(const std::string& file,
                                   std::string_view cell,
                                   const std::vector<std::string>& verdicts) {
  const std::optional<std::string> text =
      ReadFile(std::string(WLB_SPECS_DIR) + "/" + file);
  EXPECT_TRUE(text) << file;
  std::vector<std::string> out;
  if (!text) {
    return out;
  }
  const std::vector<std::string> lines = base::SplitString(
      *text, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  const std::vector<std::string> header = base::SplitString(
      lines[0], "\t", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  size_t verdict_col = header.size();
  size_t cell_col = header.size();
  for (size_t i = 0; i < header.size(); ++i) {
    if (header[i] == "verdict") {
      verdict_col = i;
    }
    if (header[i] == cell) {
      cell_col = i;
    }
  }
  EXPECT_LT(verdict_col, header.size());
  EXPECT_LT(cell_col, header.size());
  for (size_t n = 1; n < lines.size(); ++n) {
    const std::vector<std::string> row = base::SplitString(
        lines[n], "\t", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    EXPECT_EQ(row.size(), header.size()) << file << ":" << n + 1;
    if (row.size() != header.size()) {
      continue;
    }
    const std::string& verdict = row[verdict_col];
    EXPECT_NE(std::find(verdicts.begin(), verdicts.end(), verdict),
              verdicts.end())
        << file << ":" << n + 1 << ": unknown verdict " << verdict;
    if (verdict == "always") {
      EXPECT_EQ(row[cell_col], "always") << file << ":" << n + 1;
    }
    if (verdict == "fits") {
      out.push_back(row[cell_col]);
    }
  }
  return out;
}

TEST(OrdersTest, EveryFittingConditionOfTheAuditParses) {
  const std::vector<std::string> whens =
      FitsCells("trigger-conditions.tsv", "when",
                {"fits", "always", "unsayable", "bugnet", "plumbing", "cheat"});
  EXPECT_GT(whens.size(), 300u);
  for (const std::string& when : whens) {
    EXPECT_TRUE(ParseWhen(when)) << when;
  }
}

TEST(OrdersTest, EveryFittingActionOfTheAuditParses) {
  const std::vector<std::string> acts =
      FitsCells("trigger-actions.tsv", "act",
                {"fits", "reflex", "unsayable", "bugnet", "plumbing", "cheat"});
  EXPECT_GT(acts.size(), 200u);
  for (const std::string& act : acts) {
    EXPECT_TRUE(ParseActs(act)) << act;
  }
}

TEST(OrdersTest, TheGrammarNamesOnlyWhatTheBotKnows) {
  EXPECT_EQ(OrderGrammar({}), "");
  const std::string grammar =
      OrderGrammar({"Defile", "Lord Marrowgar", "bad|name", ""});
  EXPECT_NE(grammar.find("oname ::= \"Defile\" | \"Lord Marrowgar\"\n"),
            std::string::npos)
      << grammar;
  EXPECT_NE(grammar.find("oline ::= \"\\norder: \" owhen"), std::string::npos);
}

}  // namespace
}  // namespace harness
