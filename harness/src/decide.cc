// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "decide.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "decision_log.h"
#include "file_util.h"
#include "llm_client.h"
#include "plan.h"
#include "prompt.h"
#include "proto/snapshot.h"
#include "screen_renderer.h"

namespace harness {
namespace {

// "--name=value" or "--flag": the part after "--", split at '='.
bool SplitFlag(const std::string& arg, std::string* name, std::string* value) {
  const std::optional<std::string_view> rest = base::RemovePrefix(arg, "--");
  if (!rest) {
    return false;
  }
  const size_t eq = rest->find('=');
  *name = std::string(rest->substr(0, eq));
  *value = eq == rest->npos ? "" : std::string(rest->substr(eq + 1));
  return true;
}

// What a plan's steps mean on this screen: "3x4 attack, 5 rest".
std::string DescribeSteps(const Plan& plan, const Screen& screen) {
  std::vector<std::string> parts;
  for (const PlanStep& step : plan.steps) {
    const std::string_view kind =
        OptionKindName(screen.options[step.option - 1]);
    parts.push_back(
        step.times > 1
            ? base::StrCat({base::NumberToString(step.option), "x",
                            base::NumberToString(step.times), " ", kind})
            : base::StrCat({base::NumberToString(step.option), " ", kind}));
  }
  return base::JoinString(parts, ", ");
}

}  // namespace

std::optional<DecideOptions> ParseDecideArgs(
    const std::vector<std::string>& args,
    std::string* error) {
  DecideOptions options;
  for (const std::string& arg : args) {
    std::string name;
    std::string value;
    if (!SplitFlag(arg, &name, &value)) {
      if (!options.snapshot_path.empty()) {
        *error = base::StrCat({"one snapshot only; got a second: ", arg});
        return std::nullopt;
      }
      options.snapshot_path = arg;
      continue;
    }
    int number = 0;
    const bool is_number = base::StringToInt(value, &number);
    double real = 0;
    const bool is_real =
        base::StringToDouble(value, &real) && std::isfinite(real);
    if (name == "name") {
      options.bot_name = value;
    } else if (name == "goal") {
      options.current_goal = value;
    } else if (name == "reason") {
      options.wake_reason = value;
    } else if (name == "rules") {
      options.rules_path = value;
    } else if (name == "identity") {
      options.identity_path = value;
    } else if (name == "log-dir") {
      options.log_dir = value;
    } else if (name == "host") {
      options.endpoint.host = value;
    } else if (name == "port" && is_number && number > 0 && number < 65536) {
      options.endpoint.port = number;
    } else if (name == "slot" && is_number && number >= -1) {
      options.slot = number;
    } else if (name == "temperature" && is_real && real >= 0.0 && real <= 2.0) {
      options.temperature = static_cast<float>(real);
    } else if (name == "dry-run" && value.empty()) {
      options.dry_run = true;
    } else if (name == "print-request" && value.empty()) {
      options.print_request = true;
    } else if (name == "whisperers") {
      options.whisperers = base::SplitString(value, ",", base::TRIM_WHITESPACE,
                                             base::SPLIT_WANT_NONEMPTY);
    } else {
      *error = base::StrCat({"unknown or invalid option: ", arg});
      return std::nullopt;
    }
  }
  if (options.snapshot_path.empty()) {
    *error = "which snapshot? harness decide <snapshot.json> [options]";
    return std::nullopt;
  }
  return options;
}

int RunDecide(const DecideOptions& options,
              const AskFn& ask,
              std::string* report) {
  const std::optional<std::string> snapshot_bytes =
      ReadFile(options.snapshot_path);
  if (!snapshot_bytes) {
    base::StrAppend(report,
                    {"can't read the snapshot ", options.snapshot_path, "\n"});
    return 1;
  }
  wlb::snapshot::Snapshot snapshot;
  if (!wlb::snapshot::ReadJson(*snapshot_bytes, &snapshot) &&
      !wlb::snapshot::ReadBeve(*snapshot_bytes, &snapshot)) {
    base::StrAppend(report, {options.snapshot_path, " is not a snapshot\n"});
    return 1;
  }
  const std::optional<std::string> rules = ReadFile(options.rules_path);
  if (!rules) {
    base::StrAppend(report,
                    {"can't read the rules ", options.rules_path, "\n"});
    return 1;
  }
  PromptInput input;
  input.rules = *rules;
  input.bot_name = !options.bot_name.empty() ? options.bot_name
                   : !snapshot.bot.empty()   ? snapshot.bot
                                             : "bot";
  if (!options.identity_path.empty()) {
    const std::optional<std::string> identity = ReadFile(options.identity_path);
    if (!identity) {
      base::StrAppend(
          report, {"can't read the identity ", options.identity_path, "\n"});
      return 1;
    }
    input.identity = *identity;
  }
  input.current_goal = options.current_goal;
  input.wake_reason = options.wake_reason;
  input.slot = options.slot;
  input.temperature = options.temperature;
  input.whisperers = options.whisperers;
  for (const wlb::snapshot::Event& event : snapshot.events) {
    if (event.chat && event.chat->channel == wlb::snapshot::WHISPER &&
        !event.chat->from.empty() &&
        std::find(input.whisperers.begin(), input.whisperers.end(),
                  event.chat->from) == input.whisperers.end()) {
      input.whisperers.push_back(event.chat->from);
    }
  }

  const Screen screen = RenderScreen(snapshot);
  const LlmRequest request = BuildPlanRequest(input, screen);
  if (options.print_request) {
    // Kind names are plain words (OptionKindName): no JSON escaping needed.
    std::vector<std::string> kinds;
    for (const OptionKind kind : screen.options) {
      kinds.push_back(base::StrCat({"\"", OptionKindName(kind), "\""}));
    }
    base::StrAppend(report, {"{\"kinds\":[", base::JoinString(kinds, ","),
                             "],\"body\":", BuildChatBody(request), "}\n"});
    return 0;
  }
  base::StringAppendF(report, "== screen of %s (%zu options)\n%s\n",
                      input.bot_name.c_str(), screen.options.size(),
                      screen.text.c_str());
  if (options.dry_run) {
    base::StringAppendF(
        report, "== dry run: not asked. The prompt is %zu characters.\n",
        request.system.size() + request.user.size());
    return 0;
  }

  Decision decision;
  decision.bot = input.bot_name;
  decision.reason = options.wake_reason;
  decision.request = request;
  decision.time = base::Time::Now();
  decision.result = ask.Run(request);
  if (!decision.result.reply) {
    base::StringAppendF(report, "== no reply (%.0f ms): %s\n",
                        decision.result.round_trip_ms,
                        decision.result.error.c_str());
  } else {
    const LlmReply& reply = *decision.result.reply;
    if (decision.result.first_line_ms > 0) {
      base::StringAppendF(
          report,
          "== reply (%.0f ms, first line %.0f ms; prompt %d tokens, %d read "
          "again; wrote %d)\n%s\n",
          decision.result.round_trip_ms, decision.result.first_line_ms,
          reply.prompt_tokens, reply.prompt_computed, reply.completion_tokens,
          reply.text.c_str());
    } else {
      base::StringAppendF(
          report,
          "== reply (%.0f ms; prompt %d tokens, %d read again; wrote %d)\n%s\n",
          decision.result.round_trip_ms, reply.prompt_tokens,
          reply.prompt_computed, reply.completion_tokens, reply.text.c_str());
    }
    decision.plan =
        ParsePlan(reply.text, static_cast<int>(screen.options.size()));
    if (decision.plan) {
      const std::string desc = DescribeSteps(*decision.plan, screen);
      base::StringAppendF(report, "== plan: %s; look again in %d s\n",
                          desc.c_str(), decision.plan->look_s);
    } else {
      base::StrAppend(report, {"== not a valid plan for these options\n"});
    }
  }
  if (!options.log_dir.empty()) {
    const DecisionLog log(options.log_dir);
    if (log.Record(decision)) {
      const std::string path = log.PathFor(decision.bot, decision.time);
      base::StringAppendF(report, "== logged in %s\n", path.c_str());
    } else {
      base::StrAppend(report, {"== could not write the decision log\n"});
    }
  }
  return decision.plan ? 0 : 1;
}

}  // namespace harness
