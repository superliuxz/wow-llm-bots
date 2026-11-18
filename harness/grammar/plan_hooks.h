// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The hand-written parts of plan.wlb (docs/plans/grammar-idl.md, "escape
// hatches"): what the generated plan parser calls for the slips an
// unconstrained model makes and the few irregular shapes. Each has its own
// test (plan_hooks_unittest.cc).

#ifndef HARNESS_GRAMMAR_PLAN_HOOKS_H_
#define HARNESS_GRAMMAR_PLAN_HOOKS_H_

#include <string>
#include <string_view>

namespace harness::grammar::hooks {

// `do:` / `expect:`: drops brackets ("[3]", "(3)") and writes "3 x 4" or
// "3×4" as "3x4".
std::string NormalizeSteps(std::string_view value);

// `watch:`: any case, "hp40", "hp < 40", "hp<40%", "enemy_player 40yd" ->
// "hp<40", "enemy_player<40".
std::string NormalizeWatches(std::string_view value);

// `look:`: "60", "60s", "60 seconds", "2m", "2 min", "2 minutes" -> seconds.
bool LookSeconds(std::string_view value, int* seconds);

// `tell <name>:`: a character name, 2-12 ASCII letters, capitalized as the
// game writes it ("gATDA" -> "Gatda").
bool CapitalizedName(std::string_view value, std::string* name);

}  // namespace harness::grammar::hooks

#endif  // HARNESS_GRAMMAR_PLAN_HOOKS_H_
