// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/orders.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
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
// names the order uses, and a minimap ping on the screen).
void ExpectGrammarAccepts(const Order& order) {
  std::vector<std::string> names;
  grammar::CollectNames(order, &names);
  const std::string rules = OrderGrammar(
      names.empty() ? std::vector<std::string>{"X"} : names, {"Hele"});
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
           "always -> to ping, then pet dismiss",
           "ping within 5 -> use Flare on ping",
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
// (the grammar's regression set; replaces a retired Python checker).
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

// `ping` (where the party's minimap ping is) is offered only while there is
// one; without it the rest of the grammar is unchanged. Parsing still reads it
// (a standing order outlives the ping that it named).
TEST(OrdersTest, PingIsOfferedOnlyWhileAPingIsOnTheScreen) {
  const std::string root = grammar::OrdersRuleName("Order");
  const std::string without = OrderGrammar({"Flare"});
  const std::string with = OrderGrammar({"Flare"}, {"Hele"});
  EXPECT_FALSE(GbnfAccepts(without, root, "always -> to ping").accepted)
      << without;
  EXPECT_FALSE(GbnfAccepts(without, root, "ping within 5 -> jump").accepted);
  EXPECT_TRUE(GbnfAccepts(without, root, "always -> pet dismiss").accepted);
  EXPECT_TRUE(GbnfAccepts(without, root, "always -> use Flare").accepted);
  EXPECT_TRUE(GbnfAccepts(with, root, "always -> to ping").accepted) << with;
  EXPECT_TRUE(GbnfAccepts(with, root, "always -> use Flare on ping").accepted);
  // A ping alone doesn't turn orders on: they still need a name to use.
  EXPECT_EQ(OrderGrammar({}, {"Hele"}), "");
  // A non-name pinger (none of the screen's names) offers nothing.
  EXPECT_FALSE(
      GbnfAccepts(OrderGrammar({"Flare"}, {""}), root, "always -> to ping")
          .accepted);

  const std::optional<std::vector<Act>> acts = ParseActs("to ping");
  ASSERT_TRUE(acts);
  const auto& to = std::get<Act::To>((*acts)[0].value);
  const auto& at = std::get<Place::At>(to.place.value);
  EXPECT_TRUE(std::holds_alternative<Thing::Ping>(at.t.value));
  // `ping` is the ping, never a unit named "ping".
  const std::optional<std::vector<Act>> named =
      ParseActs("target ping", {"ping"});
  ASSERT_TRUE(named);
  EXPECT_TRUE(std::holds_alternative<Thing::Ping>(
      std::get<Act::Target>((*named)[0].value).t.value));
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
// up (docs/specs/grammar-idl.md, "Tests").
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
      "- Phase 1: The Lich King, Defile: Black pool under target; run away "
      "immediately.\n"
      "- Sindragosa, Mystic Buffet: when above 4 stacks, use Ice Block or "
      "Hand of Protection.\n"
      "- VX-001: priority add.\n"
      "- Kinetic Bomb1-4: synthetic range label.";

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
  EXPECT_TRUE(has("VX-001"));
  // Dead corpses, the bot's own unit name, structural headings, attached
  // digit ranges, and sentence stop-words stay out.
  EXPECT_FALSE(has("Rimefang"));
  EXPECT_FALSE(has("Aelindra"));
  EXPECT_FALSE(has("The"));
  EXPECT_FALSE(has("Use"));
  EXPECT_FALSE(has("Phase 1"));
  EXPECT_FALSE(has("Defile: Black"));
  EXPECT_FALSE(has("Kinetic Bomb1-4"));
}

// A dead party or raid member is a body to resurrect (`use Resurrection on
// Loadd`), so its name stays an order target; a dead mob's does not, even with
// the same name as nothing living.
TEST(OrdersTest, CollectOrderNamesKeepsDeadGroupMembers) {
  wlb::snapshot::Snapshot snap;
  snap.bot = "Loadb";
  wlb::snapshot::Unit dead_mate;
  dead_mate.name = "Loadd";
  dead_mate.dead = true;
  snap.base.nearby.units.push_back(dead_mate);
  snap.ext.party.emplace().members.push_back(dead_mate);
  wlb::snapshot::Unit dead_raider;
  dead_raider.name = "Kelra";
  dead_raider.dead = true;
  snap.ext.raid.emplace().members.push_back(dead_raider);
  wlb::snapshot::Unit dead_mob;
  dead_mob.name = "Defias Thug";
  dead_mob.dead = true;
  snap.base.nearby.units.push_back(dead_mob);

  const std::vector<std::string> names = CollectOrderNames(snap);
  auto has = [&](std::string_view want) {
    return std::find(names.begin(), names.end(), want) != names.end();
  };
  EXPECT_TRUE(has("Loadd"));
  EXPECT_TRUE(has("Kelra"));
  EXPECT_FALSE(has("Defias Thug"));
}

TEST(OrdersTest, CollectOrderNamesIsEmptyOnBlankSnapshotAndNotes) {
  wlb::snapshot::Snapshot empty;
  EXPECT_TRUE(CollectOrderNames(empty).empty());
}

TEST(OrdersTest, AllContentNotesParseWithRealOrdersParser) {
  // Resolve repo root from this test file's path so the real C++ orders.wlb
  // parser validates every standing order across all 41 content/notes files.
  const std::filesystem::path notes_root = std::filesystem::path(__FILE__)
                                               .parent_path()
                                               .parent_path()
                                               .parent_path()
                                               .parent_path() /
                                           "content" / "notes";
  ASSERT_TRUE(std::filesystem::is_directory(notes_root)) << notes_root;
  int files_checked = 0;
  int orders_checked = 0;
  for (const auto& entry : std::filesystem::directory_iterator(notes_root)) {
    const std::filesystem::path notes_file = entry.path() / "notes.md";
    if (!std::filesystem::is_regular_file(notes_file)) {
      continue;
    }
    ++files_checked;
    std::ifstream in(notes_file);
    ASSERT_TRUE(in.is_open()) << notes_file;
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    // Validate per `## ` section (matching SelectRelevantNotes on large files)
    // so a name in one boss section cannot mask a missing name in another.
    std::vector<std::string> sections;
    std::string cur;
    for (std::string_view raw_line : base::SplitStringPiece(
             content, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
      if (raw_line.starts_with("## ") && !cur.empty()) {
        sections.push_back(std::move(cur));
        cur.clear();
      }
      cur.append(raw_line);
      cur.push_back('\n');
    }
    if (!cur.empty()) {
      sections.push_back(std::move(cur));
    }
    for (const std::string& sec : sections) {
      const std::vector<std::string> names =
          CollectOrderNames(wlb::snapshot::Snapshot{}, sec);
      const grammar::rt::Env env{.names = {{"order_names", names}},
                                 .counts = {}};
      bool in_fence = false;
      for (std::string_view raw_line : base::SplitStringPiece(
               sec, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
        const std::string_view trimmed =
            base::TrimWhitespaceASCII(raw_line, base::TRIM_ALL);
        if (trimmed.starts_with("```")) {
          in_fence = !in_fence;
          continue;
        }
        if (in_fence && trimmed.find("->") != std::string_view::npos) {
          EXPECT_FALSE(names.empty())
              << notes_file.string()
              << ": 0 names extracted for section with: " << trimmed;
          const std::optional<Order> parsed = grammar::ParseOrder(trimmed, env);
          EXPECT_TRUE(parsed.has_value())
              << notes_file.string() << ": failed to parse order against "
              << "ExtractNamesFromNotes: " << trimmed;
          ++orders_checked;
        }
      }
    }
  }
  EXPECT_GE(files_checked, 40);
  EXPECT_GT(orders_checked, 100);
}

TEST(OrdersTest, EveryZeroSecondsIsRejectedByParserAndGrammar) {
  EXPECT_FALSE(ParseOrder("every 0s -> wake").has_value());
  EXPECT_FALSE(ParseOrder("every 0.5s -> wake").has_value());
  EXPECT_TRUE(ParseOrder("every 1s -> wake").has_value());
  EXPECT_TRUE(ParseOrder("every 1.5s -> wake").has_value());
  const std::string gbnf = OrderGrammar({"Defile"});
  ASSERT_FALSE(gbnf.empty());
  EXPECT_FALSE(GbnfAccepts(gbnf, "orders-Order", "every 0s -> wake").accepted);
  EXPECT_FALSE(
      GbnfAccepts(gbnf, "orders-Order", "every 0.5s -> wake").accepted);
  EXPECT_TRUE(GbnfAccepts(gbnf, "orders-Order", "every 1s -> wake").accepted);
}

}  // namespace
}  // namespace harness
