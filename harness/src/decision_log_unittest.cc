// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "decision_log.h"

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

#include "base/time/time.h"
#include "file_util.h"
#include "gtest/gtest.h"
#include "llm_client.h"
#include "nlohmann/json.hpp"
#include "plan.h"

namespace harness {
namespace {

using Json = nlohmann::json;

// 2026-11-16 01:23:45 UTC.
constexpr int64_t kSomeTimeMs = 1794792225000;

Decision SampleDecision() {
  Decision decision;
  decision.bot = "Tess";
  decision.reason = "plan done";
  decision.request.system = "RULES";
  decision.request.user = "Woken: plan done.\n\nYour screen now:\nme: Tess";
  decision.request.slot = 4;
  LlmReply reply;
  reply.text = "do: 1\nlook: 60";
  reply.prompt_tokens = 700;
  reply.prompt_computed = 120;
  reply.completion_tokens = 8;
  reply.prompt_ms = 12.5;
  reply.predicted_ms = 30;
  decision.result.reply = reply;
  decision.result.first_line_ms = 18.25;
  decision.result.round_trip_ms = 55.5;
  decision.plan = ParsePlan(reply.text, 4);
  decision.time = base::Time::FromMillisecondsSinceUnixEpoch(kSomeTimeMs);
  return decision;
}

TEST(DecisionLogTest, TheJsonHasWhatWasAskedAndAnswered) {
  const Json json = Json::parse(DecisionJson(SampleDecision()));
  EXPECT_EQ(json["time_ms"], kSomeTimeMs);
  EXPECT_EQ(json["bot"], "Tess");
  EXPECT_EQ(json["reason"], "plan done");
  EXPECT_EQ(json["slot"], 4);
  EXPECT_EQ(json["system_chars"], 5);
  EXPECT_EQ(json["screen"], "Woken: plan done.\n\nYour screen now:\nme: Tess");
  EXPECT_EQ(json["reply"], "do: 1\nlook: 60");
  EXPECT_EQ(json["prompt_tokens"], 700);
  EXPECT_EQ(json["prompt_computed"], 120);
  EXPECT_EQ(json["completion_tokens"], 8);
  EXPECT_DOUBLE_EQ(json["first_line_ms"].get<double>(), 18.25);
  EXPECT_DOUBLE_EQ(json["round_trip_ms"].get<double>(), 55.5);
  EXPECT_EQ(json["plan"], "do: 1\nexpect: none\nwatch: none\nlook: 60");
  EXPECT_FALSE(json.contains("error"));
}

TEST(DecisionLogTest, TheSystemPromptIsHashedNotStoredAndTheHashIsStable) {
  const Json a = Json::parse(DecisionJson(SampleDecision()));
  // FNV-1a of "RULES", the same on every run.
  EXPECT_EQ(a["system_hash"], "66fb77dd3931d7e0");
  Decision other = SampleDecision();
  other.request.system = "RULES, edited";
  EXPECT_NE(Json::parse(DecisionJson(other))["system_hash"], a["system_hash"]);
  EXPECT_FALSE(a.contains("system"));
}

TEST(DecisionLogTest, AFailureIsRecordedWithItsError) {
  Decision decision = SampleDecision();
  decision.result.reply.reset();
  decision.result.error = "could not reach llama-server";
  decision.plan.reset();
  const Json json = Json::parse(DecisionJson(decision));
  EXPECT_EQ(json["error"], "could not reach llama-server");
  EXPECT_FALSE(json.contains("reply"));
  EXPECT_TRUE(json["plan"].is_null());
}

TEST(DecisionLogTest, ARepliesThatIsNotAPlanKeepsTheTextAndNoPlan) {
  Decision decision = SampleDecision();
  decision.result.reply->text = "I would attack the boar";
  decision.plan.reset();
  const Json json = Json::parse(DecisionJson(decision));
  EXPECT_EQ(json["reply"], "I would attack the boar");
  EXPECT_TRUE(json["plan"].is_null());
}

TEST(DecisionLogTest, TheFileIsPerBotAndPerUtcDay) {
  const DecisionLog log("/var-bots");
  const base::Time time =
      base::Time::FromMillisecondsSinceUnixEpoch(kSomeTimeMs);
  EXPECT_EQ(log.PathFor("Tess", time),
            "/var-bots/Tess/events/2026-11-16.jsonl");
  // Just before midnight UTC is still the day before.
  EXPECT_EQ(log.PathFor("Tess", time - base::Seconds(3600 + 23 * 60 + 46)),
            "/var-bots/Tess/events/2026-11-15.jsonl");
}

TEST(DecisionLogTest, ANameCannotPointElsewhere) {
  const DecisionLog log("/r");
  const base::Time time =
      base::Time::FromMillisecondsSinceUnixEpoch(kSomeTimeMs);
  EXPECT_EQ(log.PathFor("../../etc", time),
            "/r/______etc/events/2026-11-16.jsonl");
  EXPECT_EQ(log.PathFor("", time), "/r/_/events/2026-11-16.jsonl");
}

TEST(DecisionLogTest, RecordAppendsOneLinePerDecision) {
  std::string pattern = testing::TempDir() + "harness-log-XXXXXX";
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const DecisionLog log(pattern);
  const Decision decision = SampleDecision();
  ASSERT_TRUE(log.Record(decision));
  ASSERT_TRUE(log.Record(decision));
  const std::string path = log.PathFor("Tess", decision.time);
  const std::optional<std::string> text = ReadFile(path);
  ASSERT_TRUE(text);
  size_t lines = 0;
  for (char c : *text) {
    lines += c == '\n';
  }
  EXPECT_EQ(lines, 2u);
  EXPECT_EQ(Json::parse(text->substr(0, text->find('\n')))["bot"], "Tess");
  ::unlink(path.c_str());
  ::rmdir((pattern + "/Tess/events").c_str());
  ::rmdir((pattern + "/Tess").c_str());
  ::rmdir(pattern.c_str());
}

TEST(DecisionLogTest, RecordFailsQuietlyWhenItCannotWrite) {
  const DecisionLog log("/proc/not-writable");
  EXPECT_FALSE(log.Record(SampleDecision()));
}

}  // namespace
}  // namespace harness
