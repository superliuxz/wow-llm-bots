// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "runtime/decide.h"

#include <stdlib.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "llm/llm_client.h"
#include "runtime/file_util.h"
#include "test/gbnf_check.h"

namespace harness {
namespace {

// Three module options in combat: 1 attack, 2 gather, 3 keep.
constexpr char kGoodPlan[] =
    "do: 1x3, 2\nexpect: 1\nwatch: hp<40\nlook: 90\ngoal: kill the boars";

DecideOptions Options() {
  DecideOptions options;
  options.snapshot_path =
      std::string(WLB_HARNESS_TESTDATA) + "/screens/fight.snapshot.json";
  options.rules_path = std::string(WLB_HARNESS_TESTDATA) + "/rules.txt";
  options.bot_name = "Tess";
  return options;
}

AskFn Replying(const std::string& text, LlmRequest* seen = nullptr) {
  return [text, seen](const LlmRequest& request) {
    if (seen != nullptr) {
      *seen = request;
    }
    LlmResult result;
    LlmReply reply;
    reply.text = text;
    reply.prompt_tokens = 700;
    reply.prompt_computed = 100;
    reply.completion_tokens = 12;
    result.reply = reply;
    result.round_trip_ms = 42;
    return result;
  };
}

TEST(DecideArgsTest, TheSnapshotIsTheOnlyRequiredThing) {
  std::string error;
  const auto options = ParseDecideArgs({"snap.pb"}, &error);
  ASSERT_TRUE(options) << error;
  EXPECT_EQ(options->snapshot_path, "snap.pb");
  EXPECT_EQ(options->wake_reason, "plan done");
  EXPECT_EQ(options->rules_path, "content/RULES.md");
  EXPECT_EQ(options->endpoint.port, 8090);
  EXPECT_EQ(options->slot, -1);
  EXPECT_FALSE(options->dry_run);
}

TEST(DecideArgsTest, EveryFlag) {
  std::string error;
  const auto options = ParseDecideArgs(
      {"--name=Tess", "--goal=boars", "--reason=attacked", "--rules=r.md",
       "--identity=i.md", "snap.pb", "--log-dir=var/bots", "--host=10.0.0.2",
       "--port=9000", "--slot=5", "--temperature=0", "--dry-run"},
      &error);
  ASSERT_TRUE(options) << error;
  EXPECT_EQ(options->bot_name, "Tess");
  EXPECT_EQ(options->current_goal, "boars");
  EXPECT_EQ(options->wake_reason, "attacked");
  EXPECT_EQ(options->rules_path, "r.md");
  EXPECT_EQ(options->identity_path, "i.md");
  EXPECT_EQ(options->log_dir, "var/bots");
  EXPECT_EQ(options->endpoint.host, "10.0.0.2");
  EXPECT_EQ(options->endpoint.port, 9000);
  EXPECT_EQ(options->slot, 5);
  EXPECT_FLOAT_EQ(options->temperature, 0.0f);
  EXPECT_TRUE(options->dry_run);
}

TEST(DecideArgsTest, SaysWhatIsWrong) {
  std::string error;
  EXPECT_FALSE(ParseDecideArgs({}, &error));
  EXPECT_NE(error.find("which snapshot"), std::string::npos);
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "b.pb"}, &error));
  EXPECT_NE(error.find("one snapshot"), std::string::npos);
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "--colour=red"}, &error));
  EXPECT_NE(error.find("--colour=red"), std::string::npos);
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "--port=0"}, &error));
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "--port=70000"}, &error));
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "--port=abc"}, &error));
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "--slot=-2"}, &error));
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "--temperature=-0.1"}, &error));
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "--temperature=2.5"}, &error));
  EXPECT_FALSE(ParseDecideArgs({"a.pb", "--temperature=nan"}, &error));
}

TEST(DecideTest, ShowsTheScreenTheReplyAndWhatThePlanMeans) {
  LlmRequest seen;
  std::string report;
  EXPECT_EQ(RunDecide(Options(), Replying(kGoodPlan, &seen), &report), 0);
  EXPECT_NE(report.find("== screen of Tess (3 options)"), std::string::npos)
      << report;
  EXPECT_NE(report.find("Mottled Boar"), std::string::npos);
  EXPECT_NE(
      report.find(
          "== reply (42 ms; prompt 700 tokens, 100 read again; wrote 12)"),
      std::string::npos)
      << report;
  EXPECT_NE(report.find("== plan: 1x3 attack, 2 gather; look again in 90 s"),
            std::string::npos)
      << report;
  // The model was asked with shared rules in system and bot context in user.
  EXPECT_NE(seen.system.find("Test rules"), std::string::npos);
  EXPECT_EQ(seen.system.find("Tess"), std::string::npos);
  EXPECT_NE(seen.user.find("You are Tess."), std::string::npos);
  EXPECT_NE(seen.user.find("Woken: plan done."), std::string::npos);
  // The grammar offers the screen's three options and no more.
  EXPECT_TRUE(GbnfAccepts(seen.grammar, "root",
                          "do: 3\nexpect: none\nwatch: none\nlook: 60")
                  .accepted);
  EXPECT_FALSE(GbnfAccepts(seen.grammar, "root",
                           "do: 4\nexpect: none\nwatch: none\nlook: 60")
                   .accepted);
}

TEST(DecideTest, TheBotNameDefaultsToTheSnapshots) {
  DecideOptions options = Options();
  options.bot_name.clear();
  std::string report;
  EXPECT_EQ(RunDecide(options, Replying(kGoodPlan), &report), 0);
  EXPECT_NE(report.find("== screen of Loadaa"), std::string::npos) << report;
}

TEST(DecideTest, AReplyThatIsNotAPlanFailsAndShowsWhatTheModelSaid) {
  std::string report;
  EXPECT_EQ(RunDecide(Options(), Replying("I would attack the boar."), &report),
            1);
  EXPECT_NE(report.find("I would attack the boar."), std::string::npos);
  EXPECT_NE(report.find("== not a valid plan"), std::string::npos);
  // An option that isn't on this screen is not a plan either.
  EXPECT_EQ(RunDecide(Options(), Replying("do: 12\nlook: 5"), &report), 1);
}

TEST(DecideTest, AFailedCallIsReported) {
  std::string report;
  const AskFn failing = [](const LlmRequest&) {
    LlmResult result;
    result.error = "could not reach llama-server";
    result.round_trip_ms = 3;
    return result;
  };
  EXPECT_EQ(RunDecide(Options(), failing, &report), 1);
  EXPECT_NE(report.find("== no reply (3 ms): could not reach llama-server"),
            std::string::npos)
      << report;
}

TEST(DecideTest, ADryRunShowsTheScreenAndDoesNotAsk) {
  DecideOptions options = Options();
  options.dry_run = true;
  int asked = 0;
  const AskFn counting = [&asked](const LlmRequest&) {
    ++asked;
    return LlmResult();
  };
  std::string report;
  EXPECT_EQ(RunDecide(options, counting, &report), 0);
  EXPECT_EQ(asked, 0);
  EXPECT_NE(report.find("== dry run"), std::string::npos);
  EXPECT_NE(report.find("== screen of Tess"), std::string::npos);
}

TEST(DecideTest, MissingFilesAreNamed) {
  std::string report;
  DecideOptions options = Options();
  options.snapshot_path = "/nonexistent/snap.pb";
  EXPECT_EQ(RunDecide(options, Replying(kGoodPlan), &report), 1);
  EXPECT_NE(report.find("can't read the snapshot /nonexistent/snap.pb"),
            std::string::npos);

  report.clear();
  options = Options();
  options.rules_path = "/nonexistent/rules.md";
  EXPECT_EQ(RunDecide(options, Replying(kGoodPlan), &report), 1);
  EXPECT_NE(report.find("can't read the rules"), std::string::npos);

  report.clear();
  options = Options();
  options.identity_path = "/nonexistent/identity.md";
  EXPECT_EQ(RunDecide(options, Replying(kGoodPlan), &report), 1);
  EXPECT_NE(report.find("can't read the identity"), std::string::npos);

  report.clear();
  options = Options();
  options.snapshot_path = options.rules_path;  // A text file, not a snapshot.
  EXPECT_EQ(RunDecide(options, Replying(kGoodPlan), &report), 1);
  EXPECT_NE(report.find("is not a snapshot"), std::string::npos);
}

TEST(DecideTest, TheIdentityAndGoalJoinTheUserPromptLeavingSystemShared) {
  std::string dir = testing::TempDir() + "harness-decide-XXXXXX";
  ASSERT_NE(::mkdtemp(dir.data()), nullptr);
  ASSERT_TRUE(
      AppendToFile(dir + "/identity.md", "You grew up near Razor Hill."));
  DecideOptions options = Options();
  options.identity_path = dir + "/identity.md";
  options.current_goal = "collect Cactus Apples";
  LlmRequest seen;
  std::string report;
  EXPECT_EQ(RunDecide(options, Replying(kGoodPlan, &seen), &report), 0);
  EXPECT_EQ(seen.system.find("Razor Hill"), std::string::npos);
  EXPECT_NE(seen.user.find("You grew up near Razor Hill."), std::string::npos);
  EXPECT_NE(seen.user.find("Current goal: collect Cactus Apples."),
            std::string::npos);
  ::unlink((dir + "/identity.md").c_str());
  ::rmdir(dir.c_str());
}

TEST(DecideTest, TheDecisionCanBeLogged) {
  std::string dir = testing::TempDir() + "harness-decide-XXXXXX";
  ASSERT_NE(::mkdtemp(dir.data()), nullptr);
  DecideOptions options = Options();
  options.log_dir = dir;
  std::string report;
  EXPECT_EQ(RunDecide(options, Replying(kGoodPlan), &report), 0);
  const size_t at = report.find("== logged in ");
  ASSERT_NE(at, std::string::npos) << report;
  std::string path = report.substr(at + 13);
  path = path.substr(0, path.find('\n'));
  const std::optional<std::string> line = ReadFile(path);
  ASSERT_TRUE(line);
  EXPECT_NE(line->find("\"bot\":\"Tess\""), std::string::npos) << *line;
  EXPECT_NE(line->find("goal: kill the boars"), std::string::npos);
  ::unlink(path.c_str());
  ::rmdir((dir + "/Tess/events").c_str());
  ::rmdir((dir + "/Tess").c_str());
  ::rmdir(dir.c_str());
}

TEST(DecideTest, JsonOutputCarriesParsedPlanWithoutRegexes) {
  DecideOptions options = Options();
  options.json_output = true;
  std::string report;
  EXPECT_EQ(RunDecide(options, Replying(kGoodPlan), &report), 0);
  EXPECT_NE(report.find("\"valid\":true"), std::string::npos) << report;
  EXPECT_NE(report.find("\"goal\":\"kill the boars\""), std::string::npos)
      << report;
  EXPECT_NE(report.find("\"look_s\":90"), std::string::npos) << report;
  EXPECT_NE(report.find("\"steps\":[{"), std::string::npos) << report;

  report.clear();
  EXPECT_EQ(RunDecide(options, Replying("not a plan"), &report), 1);
  EXPECT_NE(report.find("\"valid\":false"), std::string::npos) << report;
}

TEST(DecideTest, OrderNamesAndBossNotesGateGrammarAndEmitOrdersInJson) {
  DecideOptions options = Options();
  options.json_output = true;
  options.notes =
      "The Lich King, Defile: black pool under target, run out; use Lay on "
      "Hands.";
  const std::string reply_with_order =
      "do: 1\n"
      "expect: 1\n"
      "watch: none\n"
      "look: 90\n"
      "order: Defile on me -> use Lay on Hands on Mottled Boar\n";
  LlmRequest seen;
  std::string report;
  EXPECT_EQ(RunDecide(options, Replying(reply_with_order, &seen), &report), 0);
  EXPECT_NE(seen.user.find("Your boss notes:"), std::string::npos);
  EXPECT_NE(seen.grammar.find("\"Defile\""), std::string::npos);
  EXPECT_NE(seen.grammar.find("\"Lay on Hands\""), std::string::npos);
  EXPECT_NE(seen.grammar.find("\"Mottled Boar\""), std::string::npos);
  EXPECT_NE(
      report.find(
          "\"orders\":[\"Defile on me -> use Lay on Hands on Mottled Boar\"]"),
      std::string::npos)
      << report;

  // Without notes, screen names ("Mottled Boar") are offered, while unknown
  // boss mechanics ("Defile") are absent from the grammar.
  DecideOptions without_notes = Options();
  LlmRequest seen_plain;
  std::string report_plain;
  EXPECT_EQ(
      RunDecide(without_notes, Replying(kGoodPlan, &seen_plain), &report_plain),
      0);
  EXPECT_EQ(seen_plain.user.find("Your boss notes:"), std::string::npos);
  EXPECT_NE(seen_plain.grammar.find("\"Mottled Boar\""), std::string::npos);
  EXPECT_EQ(seen_plain.grammar.find("Defile"), std::string::npos);
}

}  // namespace
}  // namespace harness
