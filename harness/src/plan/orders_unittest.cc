// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/orders.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/time/time.h"
#include "grammar/orders.wlb.h"
#include "grammar/wlb_runtime.h"
#include "gtest/gtest.h"
#include "runtime/file_util.h"
#include "snapshot/snapshot.h"
#include "test/gbnf_check.h"

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
}

// In `use <name> on <thing>`, a name that holds " on " can split two ways: the
// parser prefers the names the call offered, else takes the shorter name.
TEST(OrdersTest, AUseNameHoldingOnIsReadAsTheNameTheCallOffered) {
  // Without the call's names, the first " on " splits it.
  std::optional<std::vector<Act>> acts = ParseActs("use Fire on the ground");
  ASSERT_TRUE(acts);
  const auto* use = std::get_if<Act::Use>(&(*acts)[0].value);
  ASSERT_TRUE(use);
  EXPECT_EQ(use->name, "Fire");
  ASSERT_TRUE(use->t);
  EXPECT_EQ(std::get<Thing::Named>(use->t->value).name, "the ground");
  // With them, it is the name offered.
  acts = ParseActs("use Fire on the ground", {"Fire on the ground"});
  ASSERT_TRUE(acts);
  use = std::get_if<Act::Use>(&(*acts)[0].value);
  ASSERT_TRUE(use);
  EXPECT_EQ(use->name, "Fire on the ground");
  EXPECT_FALSE(use->t);
  acts = ParseActs("use Fire on the ground on Defile",
                   {"Fire on the ground", "Defile"});
  ASSERT_TRUE(acts);
  use = std::get_if<Act::Use>(&(*acts)[0].value);
  ASSERT_TRUE(use);
  EXPECT_EQ(use->name, "Fire on the ground");
  ASSERT_TRUE(use->t);
  EXPECT_EQ(std::get<Thing::Named>(use->t->value).name, "Defile");
}

// A name that ends like a number: read as the offered name, not as a shorter
// name and a distance (review of f40b0b6, finding 18).
TEST(OrdersTest, AnOfferedNameEndingInANumberIsReadWhole) {
  const grammar::rt::Env offered{.names = {{"order_names", {"Area 52"}}},
                                 .counts = {}};
  const std::optional<std::vector<Act>> acts =
      ParseActs("away Area 52", {"Area 52"});
  ASSERT_TRUE(acts);
  const auto* away = std::get_if<Act::Away>(&(*acts)[0].value);
  ASSERT_TRUE(away);
  ASSERT_TRUE(away->t);
  EXPECT_EQ(std::get<Thing::Named>(away->t->value).name, "Area 52");
  EXPECT_FALSE(away->yd);
  const std::optional<Order> order =
      ParseOrder("always -> away Area 52", offered);
  ASSERT_TRUE(order);
  EXPECT_EQ(FormatOrder(*order), "always -> away Area 52");
}

// What the GBNF lets the model write, sampled with llama.cpp's own grammar
// code (not only what our printer writes), parses with the call's names, and
// reads back the same once printed (review of f40b0b6, finding 18).
TEST(OrdersTest, EverythingTheGrammarAllowsParses) {
  const std::vector<std::string> pool = {
      "Defile",         "Area 52",    "Fire on the ground",
      "Lord Marrowgar", "SI:7 Agent", "Mark of the Fallen Champion",
      "Kel'Thuzad"};
  const grammar::rt::Env offered{.names = {{"order_names", pool}},
                                 .counts = {}};
  const std::string rules = OrderGrammar(pool);
  const std::string root = grammar::OrdersRuleName("Order");
  int sampled = 0;
  for (uint64_t seed = 1; seed <= 400; ++seed) {
    const std::optional<std::string> text =
        GbnfSample(rules, root, seed, /*max_len=*/300, /*stop_chance=*/0.2);
    if (!text) {
      continue;
    }
    ++sampled;
    const std::optional<Order> order = ParseOrder(*text, offered);
    ASSERT_TRUE(order) << *text;
    const std::string printed = FormatOrder(*order);
    EXPECT_EQ(ParseOrder(printed, offered), order) << *text << "\n" << printed;
  }
  EXPECT_GT(sampled, 300);
}

// For random orders: parse(format(x)) == x, and the GBNF accepts the text.
// The names are realistic ones holding " of ", " on ", hyphens and colons, read
// back with the names offered (as a call does); none is also a keyword of the
// grammar ("tank", "boss") or ends like a form ("... cast"), which the
// language can't tell apart.
TEST(OrdersTest, RandomOrdersRoundTripAndTheGrammarAcceptsThem) {
  const std::vector<std::string> pool = {"Defile",
                                         "Ring of Frost",
                                         "Blood-Queen Lana'thel",
                                         "SI:7 Agent",
                                         "Lord Marrowgar",
                                         "Kel'Thuzad",
                                         "Invocation of Blood",
                                         "Mark of the Fallen Champion",
                                         "Fire on the ground"};
  const grammar::rt::Env offered{.names = {{"order_names", pool}},
                                 .counts = {}};
  grammar::rt::RandomSource source(20261118, pool);
  for (int i = 0; i < 2000; ++i) {
    const Order order = grammar::RandomOrder(&source);
    const std::string text = FormatOrder(order);
    EXPECT_EQ(ParseOrder(text, offered), order) << text;
    if (i % 10 == 0) {  // llama.cpp's grammar check is the slow part.
      ExpectGrammarAccepts(order);
    }
  }
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

TEST(OrdersTest, CollectOrderNamesExtractsFromNotesAndSnapshot) {
  wlb::snapshot::Snapshot snap;
  snap.bot = "Aelindra";
  snap.base.self.unit.name = "Aelindra";
  snap.base.self.unit.auras.emplace_back().name = "Arcane Brilliance";

  wlb::snapshot::Unit boss;
  boss.name = "Sindragosa";
  snap.base.nearby.units.push_back(boss);

  wlb::snapshot::Unit corpse;
  corpse.name = "Rimefang";
  corpse.dead = true;
  snap.base.nearby.units.push_back(corpse);

  snap.ext.spellbook.emplace().spells.emplace_back().name =
      "Power Word: Shield";
  snap.ext.bags.emplace().items.emplace_back().name = "Runic Mana Potion";

  const std::string notes =
      "- The Lich King, Defile: Black pool under target; run away "
      "immediately.\n"
      "- Sindragosa, Mystic Buffet: when above 4 stacks, use Ice Block or "
      "Hand of Protection.";

  const std::vector<std::string> names = CollectOrderNames(snap, notes);
  auto has = [&](std::string_view want) {
    return std::find(names.begin(), names.end(), want) != names.end();
  };
  EXPECT_TRUE(has("The Lich King"));
  EXPECT_TRUE(has("Defile"));
  EXPECT_TRUE(has("Sindragosa"));
  EXPECT_TRUE(has("Mystic Buffet"));
  EXPECT_TRUE(has("Ice Block"));
  EXPECT_TRUE(has("Hand of Protection"));
  EXPECT_TRUE(has("Arcane Brilliance"));
  EXPECT_TRUE(has("Power Word: Shield"));
  EXPECT_TRUE(has("Runic Mana Potion"));
  // Dead corpses, the bot's own unit name, and sentence stop-words stay out.
  EXPECT_FALSE(has("Rimefang"));
  EXPECT_FALSE(has("Aelindra"));
  EXPECT_FALSE(has("The"));
  EXPECT_FALSE(has("Use"));
  EXPECT_FALSE(has("Defile: Black"));
}

TEST(OrdersTest, CollectOrderNamesIsEmptyOnBlankSnapshotAndNotes) {
  wlb::snapshot::Snapshot empty;
  EXPECT_TRUE(CollectOrderNames(empty).empty());
}

}  // namespace
}  // namespace harness
