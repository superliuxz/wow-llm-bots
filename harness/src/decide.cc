// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "decide.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/strip.h"
#include "base/time/time.h"
#include "decision_log.h"
#include "file_util.h"
#include "llm_client.h"
#include "plan.h"
#include "prompt.h"
#include "proto/snapshot.pb.h"
#include "screen_renderer.h"

namespace harness {
namespace {

// "--name=value" or "--flag": the part after "--", split at '='.
bool SplitFlag(const std::string& arg, std::string* name, std::string* value) {
  std::string_view rest = arg;
  if (!absl::ConsumePrefix(&rest, "--")) {
    return false;
  }
  const size_t eq = rest.find('=');
  *name = std::string(rest.substr(0, eq));
  *value = eq == rest.npos ? "" : std::string(rest.substr(eq + 1));
  return true;
}

// What a plan's steps mean on this screen: "3x4 attack, 5 rest".
std::string DescribeSteps(const Plan& plan, const Screen& screen) {
  std::vector<std::string> parts;
  for (const PlanStep& step : plan.steps) {
    const std::string kind(OptionKindName(screen.options[step.option - 1]));
    parts.push_back(step.times > 1
                        ? absl::StrCat(step.option, "x", step.times, " ", kind)
                        : absl::StrCat(step.option, " ", kind));
  }
  return absl::StrJoin(parts, ", ");
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
        *error = absl::StrCat("one snapshot only; got a second: ", arg);
        return std::nullopt;
      }
      options.snapshot_path = arg;
      continue;
    }
    int number = 0;
    const bool is_number = absl::SimpleAtoi(value, &number);
    if (name == "name") {
      options.bot_name = value;
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
    } else if (name == "dry-run" && value.empty()) {
      options.dry_run = true;
    } else {
      *error = absl::StrCat("unknown or invalid option: ", arg);
      return std::nullopt;
    }
  }
  if (options.snapshot_path.empty()) {
    *error = "which snapshot? harness decide <snapshot.pb> [options]";
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
    absl::StrAppend(report, "can't read the snapshot ", options.snapshot_path,
                    "\n");
    return 1;
  }
  wlb::snapshot::Snapshot snapshot;
  if (!snapshot.ParseFromString(*snapshot_bytes)) {
    absl::StrAppend(report, options.snapshot_path, " is not a snapshot\n");
    return 1;
  }
  const std::optional<std::string> rules = ReadFile(options.rules_path);
  if (!rules) {
    absl::StrAppend(report, "can't read the rules ", options.rules_path, "\n");
    return 1;
  }
  PromptInput input;
  input.rules = *rules;
  input.bot_name = !options.bot_name.empty() ? options.bot_name
                   : !snapshot.bot().empty() ? std::string(snapshot.bot())
                                             : "bot";
  if (!options.identity_path.empty()) {
    const std::optional<std::string> identity = ReadFile(options.identity_path);
    if (!identity) {
      absl::StrAppend(report, "can't read the identity ", options.identity_path,
                      "\n");
      return 1;
    }
    input.identity = *identity;
  }
  input.wake_reason = options.wake_reason;
  input.slot = options.slot;

  const Screen screen = RenderScreen(snapshot);
  const LlmRequest request = BuildPlanRequest(input, screen);
  absl::StrAppendFormat(report, "== screen of %s (%d options)\n%s\n",
                        input.bot_name, screen.options.size(), screen.text);
  if (options.dry_run) {
    absl::StrAppendFormat(
        report, "== dry run: not asked. The prompt is %d characters.\n",
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
    absl::StrAppendFormat(report, "== no reply (%.0f ms): %s\n",
                          decision.result.round_trip_ms, decision.result.error);
  } else {
    const LlmReply& reply = *decision.result.reply;
    absl::StrAppendFormat(
        report,
        "== reply (%.0f ms; prompt %d tokens, %d read again; wrote %d)\n%s\n",
        decision.result.round_trip_ms, reply.prompt_tokens,
        reply.prompt_computed, reply.completion_tokens, reply.text);
    decision.plan =
        ParsePlan(reply.text, static_cast<int>(screen.options.size()));
    if (decision.plan) {
      absl::StrAppendFormat(report, "== plan: %s; look again in %d s\n",
                            DescribeSteps(*decision.plan, screen),
                            decision.plan->look_s);
    } else {
      absl::StrAppend(report, "== not a valid plan for these options\n");
    }
  }
  if (!options.log_dir.empty()) {
    const DecisionLog log(options.log_dir);
    if (log.Record(decision)) {
      absl::StrAppendFormat(report, "== logged in %s\n",
                            log.PathFor(decision.bot, decision.time));
    } else {
      absl::StrAppend(report, "== could not write the decision log\n");
    }
  }
  return decision.plan ? 0 : 1;
}

}  // namespace harness
