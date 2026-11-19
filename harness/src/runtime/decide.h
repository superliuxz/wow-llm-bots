// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// `harness decide <snapshot.pb>`: one decision, offline. Reads a saved
// snapshot, writes the screen the bot would see, asks the model for a plan,
// reads the plan, prints all of it with the timings, and optionally records it
// in the decision log. It is how a decision is looked at and replayed (the
// entry point for the quality check), with no game server running.

#ifndef HARNESS_RUNTIME_DECIDE_H_
#define HARNESS_RUNTIME_DECIDE_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "llm/llm_client.h"

namespace harness {

struct DecideOptions {
  std::string snapshot_path;
  std::string bot_name;      // Default: the snapshot's bot.
  std::string current_goal;  // Optional active goal before this wake.
  std::string wake_reason = "plan done";
  std::string rules_path = "content/RULES.md";
  std::string identity_path;  // Optional.
  std::string log_dir;        // Optional: var/bots.
  int slot = -1;
  float temperature = 0.7f;
  bool dry_run = false;  // Print the prompt; don't ask the model.
  // Print only the request body the harness would send, as JSON
  // ({"kinds": [option kinds], "body": {...}}), for tools/bench; don't ask
  // the model.
  bool print_request = false;
  // Print the decision (timings + parsed plan) as one JSON object instead of
  // human-readable text, for tools/wlblib/cases/eval_plans.py.
  bool json_output = false;
  // Who recently whispered the bot (who a `tell` may answer), besides the
  // whispers in the snapshot's own events.
  std::vector<std::string> whisperers;
  // Optional boss notes text (or loaded from `notes_path`).
  std::string notes;
  std::string notes_path;
  // Known mechanic/spell/item/unit names enabled for `order:` lines.
  std::vector<std::string> order_names;
  LlmEndpoint endpoint;
};

// Reads the arguments after `decide`. On failure: nullopt, and `error` says
// what is wrong with them.
std::optional<DecideOptions> ParseDecideArgs(
    const std::vector<std::string>& args,
    std::string* error);

// How the model is asked: CallLlama in the real program, a stand-in in tests.
using AskFn = base::RepeatingCallback<LlmResult(const LlmRequest&)>;

// Runs the decision and appends what it did to `report`. Returns the exit code:
// 0 if the model answered with a valid plan (or on a dry run), else 1.
int RunDecide(const DecideOptions& options,
              const AskFn& ask,
              std::string* report);

}  // namespace harness

#endif  // HARNESS_RUNTIME_DECIDE_H_
