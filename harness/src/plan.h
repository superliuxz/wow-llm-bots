// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The brain's answer: a plan (docs/specs/triggers.md). The brain can only do
// what a player can do, so a plan is mostly numbers: the options on the bot's
// screen, in order, each optionally repeated, plus what it expects and what
// to be woken for. Free text is limited to a short goal and optional chat:
// a line said out loud and a whisper to one player.
//
//   do: 3x4, 5
//   expect: 3
//   watch: hp<40, enemy_player<40
//   look: 60
//   goal: kill boars for the quest
//   say: anyone want to group?
//   tell Gatda: sure, inviting you now
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

#include "orders.h"

namespace harness {

inline constexpr int kMaxSteps = 5;
inline constexpr int kMaxExpected = 5;
inline constexpr int kMaxWatches = 4;
inline constexpr int kMaxRepeat = 29;  // A step can repeat 2..kMaxRepeat times.
inline constexpr int kMaxGoalChars = 60;
inline constexpr int kMaxSayChars = 80;
// A character name in 3.3.5a: 2 to 12 letters.
inline constexpr int kMinNameChars = 2;
inline constexpr int kMaxNameChars = 12;
inline constexpr int kMinLookSeconds = 10;
inline constexpr int kMaxLookSeconds = 999;
inline constexpr int kMaxHoverChars = 60;

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
  std::string say;  // Out loud. Empty: nothing to say.
  // A whisper: to `tell_to` (a character name), `tell`. Empty: none.
  std::string tell_to;
  std::string tell;
  // Panels to open / close (plan.wlb's `Panel` words), and the name of a
  // thing on screen to point at for its tooltip
  // (docs/plans/options-and-panels.md). Empty: none.
  std::vector<std::string> open;
  std::vector<std::string> close;
  std::string hover;
  // Standing orders, in priority order (orders.h; docs/specs/triggers.md):
  // up to kMaxOrders `order:` lines. A new plan replaces all of them.
  std::vector<Order> orders;

  bool operator==(const Plan&) const = default;
};

// The grammar (llama.cpp GBNF) for a bot that has `option_count` options.
// `tell_names`: who a `tell` line may whisper, the players who recently
// whispered the bot (a name the bot invents, or its own, can't be written);
// none means no `tell` line at all. Names that aren't a plain character name
// (2-12 ASCII letters) are left out.
// `hover_names`: what a `hover:` line may name (items and quests on screen);
// none, no `hover:` line. `order_names`: the names `order:` lines may use
// (OrderGrammar); none, no `order:` lines.
std::string PlanGrammar(int option_count,
                        const std::vector<std::string>& tell_names = {},
                        const std::vector<std::string>& hover_names = {},
                        const std::vector<std::string>& order_names = {});

// Reads only the `do: ...` line, as soon as the first newline of a streamed
// reply arrives, so the body can start moving while the rest of the plan is
// still being written.
std::optional<std::vector<PlanStep>> ParsePlanStepsLine(std::string_view line,
                                                        int option_count);

// Reads and validates a plan. nullopt if it isn't one: no valid steps, an
// option that doesn't exist, a count out of range, no `look`, an unknown
// watch. Over-long goals and chat lines are cut. `order_names`: those the
// call's grammar offered (PlanGrammar), so an order's name that could end in
// more than one place is read as one of them.
std::optional<Plan> ParsePlan(std::string_view text,
                              int option_count,
                              const std::vector<std::string>& order_names = {});

// For tools (`harness parse-plans`): `records` are replies, each
// "<option count>\t<reply text>", separated by '\0'; one "1" (a plan
// ParsePlan reads) or "0" per record, one per line. So a benchmark counts
// valid plans with the harness's own parser, not a copy (review of c80e791,
// finding 17, the part left).
std::string CheckPlanRecords(std::string_view records);

// The plan in the form above (what the grammar produces).
std::string FormatPlan(const Plan& plan);

}  // namespace harness

#endif  // HARNESS_PLAN_H_
