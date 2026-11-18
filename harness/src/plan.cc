// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The plan's format is harness/grammar/plan.wlb; its parser, printer and GBNF
// are generated (grammar/plan.wlb.h, docs/plans/grammar-idl.md). This file is
// the harness's API on top: the Plan its callers use, and the call's inputs
// (option count, whisperers, hover and order names) as the generated env.

#include "plan.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/strings/strcat.h"
#include "grammar/plan.wlb.h"
#include "grammar/wlb_runtime.h"
#include "orders.h"

namespace harness {
namespace {

namespace g = grammar;

g::rt::Env PlanEnv(int option_count,
                   std::vector<std::string> tell_names = {},
                   std::vector<std::string> hover_names = {},
                   std::vector<std::string> order_names = {}) {
  return {.names = {{"whisperers", std::move(tell_names)},
                    {"hover_names", std::move(hover_names)},
                    {"order_names", std::move(order_names)}},
          .counts = {{"options", option_count}}};
}

PlanStep ToStep(const g::Step& step) {
  return {step.option, step.times.value_or(1)};
}

g::Step FromStep(const PlanStep& step) {
  g::Step out;
  out.option = step.option;
  if (step.times > 1) {
    out.times = step.times;
  }
  return out;
}

Watch ToWatch(const g::Watch& w) {
  if (const auto* hp = std::get_if<g::Watch::Hp>(&w.value)) {
    return {Watch::Kind::kHealthBelow, hp->pct};
  }
  if (const auto* mana = std::get_if<g::Watch::Mana>(&w.value)) {
    return {Watch::Kind::kManaBelow, mana->pct};
  }
  if (const auto* enemy = std::get_if<g::Watch::EnemyPlayer>(&w.value)) {
    return {Watch::Kind::kEnemyPlayerWithin, enemy->yd};
  }
  if (std::holds_alternative<g::Watch::Attacked>(w.value)) {
    return {Watch::Kind::kAttacked, 0};
  }
  return {Watch::Kind::kWhisper, 0};
}

g::Watch FromWatch(const Watch& w) {
  g::Watch out;
  switch (w.kind) {
    case Watch::Kind::kHealthBelow:
      out.value = g::Watch::Hp{w.value};
      break;
    case Watch::Kind::kManaBelow:
      out.value = g::Watch::Mana{w.value};
      break;
    case Watch::Kind::kEnemyPlayerWithin:
      out.value = g::Watch::EnemyPlayer{w.value};
      break;
    case Watch::Kind::kAttacked:
      out.value = g::Watch::Attacked{};
      break;
    case Watch::Kind::kWhisper:
      out.value = g::Watch::Whisper{};
      break;
  }
  return out;
}

std::vector<std::string> ToPanels(const std::vector<g::Panel>& panels) {
  std::vector<std::string> out;
  for (g::Panel panel : panels) {
    out.push_back(g::FormatPanel(panel));
  }
  return out;
}

std::vector<g::Panel> FromPanels(const std::vector<std::string>& panels) {
  std::vector<g::Panel> out;
  for (const std::string& name : panels) {
    if (std::optional<g::Panel> panel = g::ParsePanel(name)) {
      out.push_back(*panel);
    }
  }
  return out;
}

template <typename To, typename From, typename Fn>
std::vector<To> Map(const std::vector<From>& from, Fn fn) {
  std::vector<To> out;
  out.reserve(from.size());
  for (const From& item : from) {
    out.push_back(fn(item));
  }
  return out;
}

}  // namespace

std::string PlanGrammar(int option_count,
                        const std::vector<std::string>& tell_names,
                        const std::vector<std::string>& hover_names,
                        const std::vector<std::string>& order_names) {
  // A screen always has at least "Keep doing what I'm doing"; an empty
  // alternative would be an invalid grammar (llama-server rejects it).
  // No order names, no `order:` lines (OrderGrammar): orders stay off until the
  // module can carry acts out.
  const bool orders_on = !OrderGrammar(order_names).empty();
  const std::string rules = g::PlanGrammar(
      "Plan", PlanEnv(std::max(option_count, 1), tell_names, hover_names,
                      orders_on ? order_names : std::vector<std::string>{}));
  return base::StrCat({"root ::= ", g::PlanRuleName("Plan"), "\n", rules});
}

std::optional<std::vector<PlanStep>> ParsePlanStepsLine(std::string_view line,
                                                        int option_count) {
  const std::optional<std::vector<g::Step>> steps =
      g::ParsePlanFirstLine(line, PlanEnv(option_count));
  if (!steps) {
    return std::nullopt;
  }
  return Map<PlanStep>(*steps, ToStep);
}

std::optional<Plan> ParsePlan(std::string_view text, int option_count) {
  const std::optional<g::Plan> parsed =
      g::ParsePlan(text, PlanEnv(option_count));
  if (!parsed) {
    return std::nullopt;
  }
  Plan plan;
  plan.steps = Map<PlanStep>(parsed->steps, ToStep);
  plan.expect = parsed->expect;
  plan.watches = Map<Watch>(parsed->watches, ToWatch);
  plan.look_s = parsed->look_s;
  plan.goal = parsed->goal;
  plan.say = parsed->say;
  plan.tell_to = parsed->tell_to;
  plan.tell = parsed->tell;
  plan.open = ToPanels(parsed->open);
  plan.close = ToPanels(parsed->close);
  plan.hover = parsed->hover;
  plan.orders = parsed->orders;
  return plan;
}

std::string FormatPlan(const Plan& plan) {
  g::Plan out;
  out.steps = Map<g::Step>(plan.steps, FromStep);
  out.expect = plan.expect;
  out.watches = Map<g::Watch>(plan.watches, FromWatch);
  out.look_s = plan.look_s;
  out.goal = plan.goal;
  out.say = plan.say;
  out.tell_to = plan.tell_to;
  out.tell = plan.tell;
  out.open = FromPanels(plan.open);
  out.close = FromPanels(plan.close);
  out.hover = plan.hover;
  out.orders = plan.orders;
  return g::FormatPlan(out);
}

}  // namespace harness
