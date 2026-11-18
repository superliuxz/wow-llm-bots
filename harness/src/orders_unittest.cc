// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "orders.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/time/time.h"
#include "file_util.h"
#include "gbnf_check.h"
#include "grammar/orders.wlb.h"
#include "gtest/gtest.h"

namespace harness {
namespace {

TEST(OrdersTest, ParsesTheSpecsExample) {
  const std::optional<Order> order =
      ParseOrder("Defile on me -> away, then to behind boss");
  ASSERT_TRUE(order);
  const auto* all = std::get_if<When::All>(&order->when.value);
  ASSERT_TRUE(all);
  ASSERT_EQ(all->conds.size(), 1u);
  EXPECT_FALSE(all->conds[0].negated);
  const auto* on = std::get_if<Check::On>(&all->conds[0].check.value);
  ASSERT_TRUE(on);
  EXPECT_EQ(on->mech, "Defile");
  EXPECT_EQ(on->who, Who::kMe);
  ASSERT_EQ(order->steps.size(), 2u);
  const auto* away = std::get_if<Act::Away>(&order->steps[0].value);
  ASSERT_TRUE(away);
  EXPECT_FALSE(away->t);
  EXPECT_FALSE(away->yd);
  const auto* to = std::get_if<Act::To>(&order->steps[1].value);
  ASSERT_TRUE(to);
  const auto* behind = std::get_if<Place::Behind>(&to->place.value);
  ASSERT_TRUE(behind);
  const auto* boss = std::get_if<Thing::Who>(&behind->t.value);
  ASSERT_TRUE(boss);
  EXPECT_EQ(boss->who, Who::kBoss);
}

// A name that holds a separator splits where both sides parse ("Fire on the
// ground" is the aura, not "Fire").
TEST(OrdersTest, ANameMayHoldTheSeparator) {
  const std::optional<Order> order =
      ParseOrder("Fire on the ground on me -> away");
  ASSERT_TRUE(order);
  const auto& all = std::get<When::All>(order->when.value);
  EXPECT_EQ(std::get<Check::On>(all.conds[0].check.value).mech,
            "Fire on the ground");
  // "away boss 10": the boss and 10 yards, not a unit named "boss 10" (a name
  // needs a letter, and the shorter thing is tried first).
  const std::optional<std::vector<Act>> acts = ParseActs("away boss 10");
  ASSERT_TRUE(acts);
  const auto& away = std::get<Act::Away>((*acts)[0].value);
  ASSERT_TRUE(away.t && away.yd);
  EXPECT_EQ(std::get<Thing::Who>(away.t->value).who, Who::kBoss);
  EXPECT_EQ(*away.yd, 10);
}

// What the printer writes, the GBNF the brain is held to accepts (with the
// names the order uses).
void ExpectGrammarAccepts(const Order& order) {
  std::vector<std::string> names;
  grammar::CollectNames(order, &names);
  const std::string rules =
      OrderGrammar(names.empty() ? std::vector<std::string>{"X"} : names);
  const std::string text = FormatOrder(order);
  const GbnfResult result =
      GbnfAccepts(rules, grammar::OrdersRuleName("Order"), text);
  EXPECT_TRUE(result.grammar_ok) << rules;
  EXPECT_TRUE(result.accepted) << text << "\n" << rules;
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
    ExpectGrammarAccepts(*order);
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
    const std::optional<Order> order =
        ParseOrder(base::StrCat({when, " -> wake"}));
    ASSERT_TRUE(order) << when;
    EXPECT_EQ(ParseOrder(FormatOrder(*order)), order) << when;
    ExpectGrammarAccepts(*order);
  }
}

TEST(OrdersTest, EveryFittingActionOfTheAuditParses) {
  const std::vector<std::string> acts =
      FitsCells("trigger-actions.tsv", "act",
                {"fits", "reflex", "unsayable", "bugnet", "plumbing", "cheat"});
  EXPECT_GT(acts.size(), 200u);
  for (const std::string& act : acts) {
    const std::optional<Order> order =
        ParseOrder(base::StrCat({"always -> ", act}));
    ASSERT_TRUE(order) << act;
    EXPECT_EQ(ParseOrder(FormatOrder(*order)), order) << act;
    ExpectGrammarAccepts(*order);
  }
}

TEST(OrdersTest, TheGrammarNamesOnlyWhatTheBotKnows) {
  EXPECT_EQ(OrderGrammar({}), "");
  const std::string grammar =
      OrderGrammar({"Defile", "Lord Marrowgar", "bad|name", ""});
  EXPECT_NE(grammar.find("orders-Name ::= \"Defile\" | \"Lord Marrowgar\"\n"),
            std::string::npos)
      << grammar;
  EXPECT_NE(grammar.find("oline ::= \"\\norder: \" orders-Order\n"),
            std::string::npos);
}

// An unconstrained model can send junk; trying every split point must not blow
// up (docs/plans/grammar-idl.md, "Tests").
TEST(OrdersTest, JunkFailsFast) {
  std::string junk;
  while (junk.size() < 240) {
    junk += "a on a & ";
  }
  for (const std::string& text :
       {junk, junk + "-> wake", "always -> " + junk,
        std::string(240, 'a') + " within 5 of " + std::string(10, 'b')}) {
    const base::TimeTicks start = base::TimeTicks::Now();
    ParseOrder(text);
    EXPECT_LT(base::TimeTicks::Now() - start, base::Milliseconds(200)) << text;
  }
}

}  // namespace
}  // namespace harness
