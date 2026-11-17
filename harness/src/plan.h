// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The brain's answer: a plan (docs/specs/triggers.md). The brain can only do
// what a player can do, so a plan is mostly numbers: the options on the bot's
// screen, in order, each optionally repeated, plus what it expects and what
// to be woken for. Free text is limited to a short goal and an optional chat
// line.
//
//   do: 3x4, 5
//   expect: 3
//   watch: hp<40, enemy_player<40
//   look: 60
//   goal: kill boars for the quest
//   say: anyone want to group?
//
// PlanGrammar() builds the grammar that makes llama-server write only valid
// plans for a bot's current options; ParsePlan() reads the text back (and
// forgives formatting slips, so a model that ignores the grammar still works).

#ifndef HARNESS_PLAN_H_
#define HARNESS_PLAN_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace harness {

inline constexpr int kMaxSteps = 5;
inline constexpr int kMaxExpected = 5;
inline constexpr int kMaxWatches = 4;
inline constexpr int kMaxRepeat = 29;  // A step can repeat 2..kMaxRepeat times.
inline constexpr int kMaxGoalChars = 60;
inline constexpr int kMaxSayChars = 80;
inline constexpr int kMinLookSeconds = 10;
inline constexpr int kMaxLookSeconds = 999;

// One option, done `times` times in a row (3x4: option 3, four times).
struct PlanStep {
  int option = 0;  // 1-based, as numbered on the screen.
  int times = 1;

  bool operator==(const PlanStep&) const = default;
};

// An extra reason to wake the brain (the defaults are in triggers.md).
struct Watch {
  enum class Kind {
    kHealthBelow,        // value: percent
    kManaBelow,          // value: percent
    kEnemyPlayerWithin,  // value: yards
    kAttacked,
    kWhisper,
  };
  Kind kind = Kind::kAttacked;
  int value = 0;

  bool operator==(const Watch&) const = default;
};

struct Plan {
  std::string goal;  // Empty: the goal is unchanged.
  std::vector<PlanStep> steps;
  std::vector<int>
      expect;  // Options this plan deals with (don't wake for them).
  std::vector<Watch> watches;
  int look_s = 0;   // Wake anyway after this many seconds.
  std::string say;  // Empty: nothing to say.

  bool operator==(const Plan&) const = default;
};

// The grammar (llama.cpp GBNF) for a bot that has `option_count` options.
std::string PlanGrammar(int option_count);

// Reads only the `do: ...` line, as soon as the first newline of a streamed
// reply arrives, so the body can start moving while the rest of the plan is
// still being written.
std::optional<std::vector<PlanStep>> ParsePlanStepsLine(std::string_view line,
                                                        int option_count);

// Reads and validates a plan. nullopt if it isn't one: no valid steps, an
// option that doesn't exist, a count out of range, no `look`, an unknown
// watch. Over-long goals and chat lines are cut.
std::optional<Plan> ParsePlan(std::string_view text, int option_count);

// The plan in the form above (what the grammar produces).
std::string FormatPlan(const Plan& plan);

}  // namespace harness

#endif  // HARNESS_PLAN_H_
