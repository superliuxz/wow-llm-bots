// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan_runner.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "plan.h"
#include "proto/snapshot.h"
#include "screen_renderer.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

std::optional<base::TimeDelta> TimeoutForKind(OptionKind kind) {
  switch (kind) {
    case OptionKind::kAttack:
      return base::Seconds(45);
    case OptionKind::kLoot:
    case OptionKind::kGather:
    case OptionKind::kUse:
      return base::Seconds(15);
    case OptionKind::kTalk:
      return base::Seconds(30);
    case OptionKind::kRest:
      return base::Seconds(60);
    case OptionKind::kExplore:
    case OptionKind::kTown:
      // The module's 10 s stall check is the real guard; this is a backstop.
      return base::Minutes(10);
    case OptionKind::kKeep:
      // `Keep` runs until the plan's own `look` timer or a surprise trigger.
      return std::nullopt;
  }
  return std::nullopt;
}

const pb::Unit* FindUnitByGuid(const pb::Snapshot& snapshot,
                               std::string_view guid) {
  if (guid.empty()) {
    return nullptr;
  }
  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if (u.guid == guid) {
      return &u;
    }
  }
  return nullptr;
}

const pb::GameObject* FindObjectByGuid(const pb::Snapshot& snapshot,
                                       std::string_view guid) {
  if (guid.empty()) {
    return nullptr;
  }
  for (const pb::GameObject& go : snapshot.base.nearby.objects) {
    if (go.guid == guid) {
      return &go;
    }
  }
  return nullptr;
}

bool IsTargetDone(OptionKind kind,
                  std::string_view guid,
                  const pb::Snapshot& snapshot) {
  switch (kind) {
    case OptionKind::kAttack: {
      const pb::Unit* unit = FindUnitByGuid(snapshot, guid);
      if (unit != nullptr) {
        return unit->dead;
      }
      return !snapshot.base.self.in_combat;
    }
    case OptionKind::kLoot: {
      const pb::Unit* unit = FindUnitByGuid(snapshot, guid);
      return unit == nullptr || !unit->lootable;
    }
    case OptionKind::kGather:
    case OptionKind::kUse: {
      const pb::GameObject* obj = FindObjectByGuid(snapshot, guid);
      return obj == nullptr || !obj->usable;
    }
    case OptionKind::kTalk:
      return snapshot.ext.interaction.has_value();
    case OptionKind::kRest: {
      const pb::Self& self = snapshot.base.self;
      const bool full_hp =
          self.health_max > 0 && self.health >= self.health_max;
      const bool full_mana = self.power_type != pb::Self::MANA ||
                             self.power_max == 0 ||
                             self.power >= self.power_max;
      return full_hp && full_mana && !self.resting;
    }
    case OptionKind::kKeep:
    case OptionKind::kExplore:
    case OptionKind::kTown:
      return false;
  }
  return false;
}

std::optional<size_t> MatchOptionInScreen(const ScreenOption& wanted,
                                          const Screen& screen,
                                          bool match_guid_first) {
  if (match_guid_first && !wanted.guid.empty()) {
    for (size_t i = 0; i < screen.details.size(); ++i) {
      if (screen.details[i].kind == wanted.kind &&
          screen.details[i].guid == wanted.guid) {
        return i;
      }
    }
  }
  for (size_t i = 0; i < screen.details.size(); ++i) {
    if (screen.details[i].kind == wanted.kind &&
        screen.details[i].name == wanted.name) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace

void PlanRunner::Stop() {
  steps_.clear();
  step_index_ = 0;
  repeats_left_ = 0;
  active_guid_.clear();
  last_command_id_ = 0;
  active_ = false;
}

RunnerAction PlanRunner::Start(const std::vector<PlanStep>& steps,
                               uint32_t snapshot_seq,
                               const Screen& screen,
                               base::TimeTicks now) {
  Stop();
  if (steps.empty()) {
    return {};
  }
  for (const PlanStep& step : steps) {
    if (step.option < 1 ||
        static_cast<size_t>(step.option) > screen.details.size()) {
      return {std::nullopt, "invalid option in plan"};
    }
    const ScreenOption& detail =
        screen.details[static_cast<size_t>(step.option - 1)];
    // Skip a redundant `Keep` only when followed by an active step.
    if (detail.kind == OptionKind::kKeep && &step != &steps.back()) {
      continue;
    }
    steps_.push_back({detail, step.times});
  }
  if (steps_.empty()) {
    return {};
  }
  active_ = true;
  step_index_ = 0;
  repeats_left_ = steps_[0].times;
  active_kind_ = steps_[0].target.kind;
  active_guid_ = steps_[0].target.guid;
  step_started_at_ = now;

  if (!screen.has_module_options) {
    // Offline/derived screen: never guess module option numbers.
    return {};
  }
  return LaunchCurrentFromScreen(snapshot_seq, screen,
                                 /*match_guid_first=*/true, now);
}

RunnerAction PlanRunner::LaunchCurrentFromScreen(uint32_t snapshot_seq,
                                                 const Screen& screen,
                                                 bool match_guid_first,
                                                 base::TimeTicks now) {
  while (step_index_ < steps_.size()) {
    const std::optional<size_t> idx = MatchOptionInScreen(
        steps_[step_index_].target, screen, match_guid_first);
    if (idx) {
      active_kind_ = screen.details[*idx].kind;
      active_guid_ = screen.details[*idx].guid;
      step_started_at_ = now;
      pb::Command::Choose choose;
      choose.seq = snapshot_seq;
      choose.option = static_cast<uint32_t>(*idx + 1);
      return {choose, ""};
    }
    // No matching target left on screen (e.g. fewer boars than 1x4): move on to
    // the next step if one exists, otherwise finish the plan so the brain
    // looks.
    ++step_index_;
    match_guid_first = true;
    if (step_index_ < steps_.size()) {
      repeats_left_ = steps_[step_index_].times;
    }
  }
  Stop();
  return {std::nullopt, "plan done"};
}

RunnerAction PlanRunner::OnSnapshot(const pb::Snapshot& snapshot,
                                    const Screen& screen,
                                    base::TimeTicks now) {
  if (!active_) {
    return {};
  }
  if (!screen.has_module_options) {
    if (const auto timed_out = CheckTimeout(now)) {
      return {std::nullopt, *timed_out};
    }
    return {};
  }
  // The body says it finished this step's command, or the target shows it
  // (some steps, like using a door, only end by the first).
  const bool body_done = active_kind_ != OptionKind::kKeep &&
                         last_command_id_ != 0 &&
                         snapshot.done_command_id == last_command_id_;
  if (!body_done && !IsTargetDone(active_kind_, active_guid_, snapshot)) {
    if (const auto timed_out = CheckTimeout(now)) {
      return {std::nullopt, *timed_out};
    }
    return {};
  }

  --repeats_left_;
  bool match_guid_first = false;
  if (repeats_left_ <= 0) {
    ++step_index_;
    match_guid_first = true;
    if (step_index_ >= steps_.size()) {
      Stop();
      return {std::nullopt, "plan done", /*step_done=*/true};
    }
    repeats_left_ = steps_[step_index_].times;
  }
  RunnerAction action =
      LaunchCurrentFromScreen(snapshot.seq, screen, match_guid_first, now);
  action.step_done = true;
  return action;
}

std::optional<std::string> PlanRunner::CheckTimeout(base::TimeTicks now) {
  if (!active_) {
    return std::nullopt;
  }
  const std::optional<base::TimeDelta> limit = TimeoutForKind(active_kind_);
  if (limit && now - step_started_at_ >= *limit) {
    Stop();
    return "step timed out";
  }
  return std::nullopt;
}

}  // namespace harness
