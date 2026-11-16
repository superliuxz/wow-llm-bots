// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "decision_log.h"

#include <time.h>

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "base/time/time.h"
#include "file_util.h"
#include "llm_client.h"
#include "nlohmann/json.hpp"
#include "plan.h"

namespace harness {
namespace {

using Json = nlohmann::json;

// 64-bit FNV-1a: stable across runs (absl::Hash is seeded per process).
uint64_t Fnv1a(std::string_view text) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : text) {
    hash = (hash ^ c) * 1099511628211ull;
  }
  return hash;
}

std::string DateUtc(base::Time time) {
  const time_t seconds =
      static_cast<time_t>(time.InMillisecondsSinceUnixEpoch() / 1000);
  struct tm parts;
  ::gmtime_r(&seconds, &parts);
  return absl::StrFormat("%04d-%02d-%02d", parts.tm_year + 1900,
                         parts.tm_mon + 1, parts.tm_mday);
}

std::string SafeName(const std::string& name) {
  std::string out;
  for (char c : name) {
    out.push_back(absl::ascii_isalnum(c) ? c : '_');
  }
  return out.empty() ? "_" : out;
}

}  // namespace

std::string DecisionJson(const Decision& decision) {
  Json json;
  json["time_ms"] = decision.time.InMillisecondsSinceUnixEpoch();
  json["bot"] = decision.bot;
  json["reason"] = decision.reason;
  json["slot"] = decision.request.slot;
  json["system_chars"] = decision.request.system.size();
  json["system_hash"] =
      absl::StrFormat("%016x", Fnv1a(decision.request.system));
  json["screen"] = decision.request.user;
  if (decision.result.reply) {
    const LlmReply& reply = *decision.result.reply;
    json["reply"] = reply.text;
    json["prompt_tokens"] = reply.prompt_tokens;
    json["prompt_computed"] = reply.prompt_computed;
    json["completion_tokens"] = reply.completion_tokens;
    json["prompt_ms"] = reply.prompt_ms;
    json["predicted_ms"] = reply.predicted_ms;
  } else {
    json["error"] = decision.result.error;
  }
  json["round_trip_ms"] = decision.result.round_trip_ms;
  json["plan"] = decision.plan ? Json(FormatPlan(*decision.plan)) : Json();
  return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string DecisionLog::PathFor(const std::string& bot,
                                 base::Time time) const {
  return absl::StrCat(root_, "/", SafeName(bot), "/events/", DateUtc(time),
                      ".jsonl");
}

bool DecisionLog::Record(const Decision& decision) const {
  const std::string path = PathFor(decision.bot, decision.time);
  if (!MakeDirectories(path.substr(0, path.rfind('/')))) {
    return false;
  }
  return AppendToFile(path, absl::StrCat(DecisionJson(decision), "\n"));
}

}  // namespace harness
