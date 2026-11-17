// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The record of what each bot's brain was asked and what it answered: one JSON
// line per decision in <root>/<bot>/events/<date>.jsonl (root is var/bots).
// It is the raw material for the quality check, for replaying a decision, and
// for the bot's own reflection later. Dates are UTC.

#ifndef HARNESS_DECISION_LOG_H_
#define HARNESS_DECISION_LOG_H_

#include <optional>
#include <string>
#include <utility>

#include "base/time/time.h"
#include "llm_client.h"
#include "plan.h"

namespace harness {

struct Decision {
  std::string bot;
  std::string reason;        // Why the brain was woken.
  LlmRequest request;        // What was asked.
  LlmResult result;          // What came back, or why not.
  std::optional<Plan> plan;  // What was understood of it; empty: not a plan.
  base::Time time;
  // Snapshot age (`CLOCK_MONOTONIC`, from `Snapshot.monotonic_us`) in live
  // runs; empty in offline `harness decide` runs without monotonic timestamps.
  std::optional<double> age_at_ask_ms;
  std::optional<double> age_at_first_line_ms;
  std::optional<double> age_at_plan_ms;
};

// The decision as one line of JSON (no newline). The prompt is kept as the
// screen the bot saw plus the size and a stable hash of its system prompt
// (the rules and identity change rarely).
std::string DecisionJson(const Decision& decision);

class DecisionLog {
 public:
  explicit DecisionLog(std::string root) : root_(std::move(root)) {}

  // <root>/<bot>/events/<UTC date>.jsonl. Characters in the name that aren't
  // letters or digits become '_', so a name can't point elsewhere.
  std::string PathFor(const std::string& bot, base::Time time) const;

  // Appends the decision; creates the folders. False if it couldn't write.
  bool Record(const Decision& decision) const;

 private:
  const std::string root_;
};

}  // namespace harness

#endif  // HARNESS_DECISION_LOG_H_
