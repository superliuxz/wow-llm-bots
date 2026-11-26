// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "runtime/decide.h"

#include <cmath>
#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "llm/llm_client.h"
#include "nlohmann/json.hpp"
#include "plan/orders.h"
#include "plan/plan.h"
#include "runtime/decision_log.h"
#include "runtime/file_util.h"
#include "screen/knowledge.h"
#include "screen/prompt.h"
#include "screen/screen_renderer.h"
#include "snapshot/snapshot.h"

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
    } else if (name == "memory") {
      options.memory_path = value;
    } else if (name == "relationships") {
      options.relationships_path = value;
    } else if (name == "remembered") {
      options.remembered.push_back(value);
    } else if (name == "party-muted" && value.empty()) {
      options.party_reply_muted = true;
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
    } else if (name == "inspect-prompt" && value.empty()) {
      options.inspect_prompt = true;
    } else if (name == "print-request" && value.empty()) {
      options.print_request = true;
    } else if (name == "json" && value.empty()) {
      options.json_output = true;
    } else if (name == "whisperers") {
      options.whisperers = base::SplitString(value, ",", base::TRIM_WHITESPACE,
                                             base::SPLIT_WANT_NONEMPTY);
    } else if (name == "notes") {
      options.notes = value;
    } else if (name == "notes-file") {
      options.notes_path = value;
    } else if (name == "knowledge") {
      options.knowledge_path = value;
    } else if (name == "order-names") {
      options.order_names = base::SplitString(value, ",", base::TRIM_WHITESPACE,
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
  PromptAssemblyContext ctx;
  ctx.rules = *rules;
  ctx.bot_name = options.bot_name;
  if (!options.identity_path.empty()) {
    const std::optional<std::string> identity = ReadFile(options.identity_path);
    if (!identity) {
      base::StrAppend(
          report, {"can't read the identity ", options.identity_path, "\n"});
      return 1;
    }
    ctx.identity = *identity;
  }
  if (!options.memory_path.empty()) {
    const std::optional<std::string> memory = ReadFile(options.memory_path);
    if (!memory) {
      base::StrAppend(report,
                      {"can't read memory ", options.memory_path, "\n"});
      return 1;
    }
    ctx.raw_memory = *memory;
  }
  if (!options.relationships_path.empty()) {
    const std::optional<std::string> rels =
        ReadFile(options.relationships_path);
    if (!rels) {
      base::StrAppend(report, {"can't read relationships ",
                               options.relationships_path, "\n"});
      return 1;
    }
    ctx.raw_relationships = *rels;
  }
  ctx.current_goal = options.current_goal;
  ctx.wake_reason = options.wake_reason;
  ctx.party_reply_muted = options.party_reply_muted;
  ctx.remembered = options.remembered;
  if (!options.notes.empty()) {
    ctx.raw_notes = options.notes;
  } else if (!options.notes_path.empty()) {
    std::optional<std::string> notes = ReadFile(options.notes_path);
    if (!notes) {
      base::StrAppend(report,
                      {"cannot read notes: ", options.notes_path, "\n"});
      return 1;
    }
    ctx.raw_notes = std::move(*notes);
  }
  std::optional<WorldKnowledge> wk;
  if (!options.knowledge_path.empty()) {
    std::string kerr;
    wk = WorldKnowledge::LoadFile(options.knowledge_path, &kerr);
    if (!wk) {
      base::StrAppend(report, {kerr, "\n"});
      return 1;
    }
  }
  ctx.whisperers = options.whisperers;
  ctx.extra_order_names = options.order_names;
  ctx.slot = options.slot;
  ctx.temperature = options.temperature;

  const PromptInput input =
      AssemblePromptInput(ctx, snapshot, wk ? &*wk : nullptr);
  const Screen screen = RenderScreen(snapshot);
  if (options.inspect_prompt) {
    base::StrAppend(report, {FormatPromptInspection(input, screen)});
    return 0;
  }
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
  if (!options.json_output) {
    base::StringAppendF(report, "== screen of %s (%zu options)\n%s\n",
                        input.bot_name.c_str(), screen.options.size(),
                        screen.text.c_str());
  }
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
  if (decision.result.reply) {
    decision.plan =
        ParsePlan(StripTruncatedLastLine(decision.result.reply->text,
                                         decision.result.reply->finish_reason),
                  static_cast<int>(screen.options.size()), input.order_names);
  }
  if (options.json_output) {
    nlohmann::json out;
    out["valid"] = decision.plan.has_value();
    out["ms"] = decision.result.round_trip_ms;
    out["first_ms"] = decision.result.first_line_ms > 0
                          ? decision.result.first_line_ms
                          : decision.result.round_trip_ms;
    out["prompt_tokens"] =
        decision.result.reply ? decision.result.reply->prompt_tokens : 0;
    out["prompt_computed"] =
        decision.result.reply ? decision.result.reply->prompt_computed : 0;
    out["raw"] = decision.result.reply ? decision.result.reply->text
                                       : decision.result.error;
    nlohmann::json steps = nlohmann::json::array();
    if (decision.plan) {
      const Plan& p = *decision.plan;
      for (const PlanStep& s : p.steps) {
        const size_t idx = static_cast<size_t>(s.option - 1);
        const std::string_view kind = OptionKindName(screen.options[idx]);
        const std::string text = idx < snapshot.options.size()
                                     ? snapshot.options[idx].text
                                     : std::string(kind);
        steps.push_back({{"option", s.option},
                         {"times", s.times},
                         {"kind", std::string(kind)},
                         {"text", text}});
      }
      out["plan_desc"] = DescribeSteps(p, screen);
      out["look_s"] = p.look_s;
      out["goal"] = p.goal;
      out["tell_to"] = p.tell_to;
      out["tell"] = p.tell;
      out["open"] = p.open;
      out["close"] = p.close;
      out["hover"] = p.hover;
      nlohmann::json orders = nlohmann::json::array();
      for (const Order& o : p.orders) {
        orders.push_back(FormatOrder(o));
      }
      out["orders"] = std::move(orders);
      nlohmann::json acts = nlohmann::json::array();
      for (const Act& a : p.acts) {
        acts.push_back(FormatAct(a));
      }
      out["acts"] = std::move(acts);
    } else {
      out["plan_desc"] = "";
      out["look_s"] = 0;
      out["goal"] = "";
      out["tell_to"] = "";
      out["tell"] = "";
      out["open"] = nlohmann::json::array();
      out["close"] = nlohmann::json::array();
      out["hover"] = "";
      out["orders"] = nlohmann::json::array();
      out["acts"] = nlohmann::json::array();
    }
    out["steps"] = std::move(steps);
    base::StrAppend(report, {out.dump(), "\n"});
  } else if (!decision.result.reply) {
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
