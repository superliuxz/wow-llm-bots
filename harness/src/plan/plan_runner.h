// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Carries out a plan's `do:` steps one target and one repeat at a time against
// incoming snapshots (docs/specs/harness.md, "For H4"):
// - Sends `Command.Choose` only when the screen's option numbers came from
//   `snapshot.options` (`screen.has_module_options`).
// - Infers when an active target is done from later snapshots (the attacked
//   unit died, the looted corpse lost `lootable` or despawned), with a safety
//   timeout per option kind.
// - Re-resolves repeats (`1x4`) and later steps against the newest snapshot by
//   `(kind, name)` so dead GUIDs and shifted option numbers never misfire.

#ifndef HARNESS_PLAN_PLAN_RUNNER_H_
#define HARNESS_PLAN_PLAN_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "plan/plan.h"
#include "screen/screen_renderer.h"
#include "snapshot/snapshot.h"

namespace harness {

struct RunnerAction {
  std::optional<wlb::snapshot::Command::Choose> choose;
  // Non-empty when the plan finished or timed out and the brain should wake.
  std::string finished_reason;
  // True when the current target was done on this snapshot (a step or one of
  // its repeats completed).
  bool step_done = false;
};

class PlanRunner {
 public:
  PlanRunner() = default;

  // Starts executing `steps` chosen from `(snapshot_seq, screen)`. May return
  // the first `Choose` command immediately (including from SSE early dispatch
  // before the rest of the plan has finished streaming).
  RunnerAction Start(const std::vector<PlanStep>& steps,
                     uint32_t snapshot_seq,
                     const Screen& screen,
                     base::TimeTicks now);

  void Stop();

  bool active() const { return active_; }
  uint32_t last_command_id() const { return last_command_id_; }
  void set_last_command_id(uint32_t id) { last_command_id_ = id; }

  // Checks whether the current target finished on `snapshot` and, if so,
  // advances to the next repeat or step on `screen`.
  RunnerAction OnSnapshot(const wlb::snapshot::Snapshot& snapshot,
                          const Screen& screen,
                          base::TimeTicks now);

  // Checks per-step safety timeouts even if no snapshot arrived recently.
  std::optional<std::string> CheckTimeout(base::TimeTicks now);

 private:
  struct ResolvedStep {
    ScreenOption target;
    int times = 1;
  };

  RunnerAction LaunchCurrentFromScreen(uint32_t snapshot_seq,
                                       const Screen& screen,
                                       bool match_guid_first,
                                       base::TimeTicks now);

  std::vector<ResolvedStep> steps_;
  size_t step_index_ = 0;
  int repeats_left_ = 0;
  OptionKind active_kind_ = OptionKind::kKeep;
  std::string active_guid_;
  std::optional<float> active_target_hp_pct_;
  base::TimeTicks step_started_at_;
  uint32_t last_command_id_ = 0;
  bool active_ = false;
};

}  // namespace harness

#endif  // HARNESS_PLAN_PLAN_RUNNER_H_
