// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// C++ offline replay and golden screen verification:
//   - RenderScreens(): checks or rewrites `harness/testdata/screens/` goldens
//     directly from C++ `RenderScreen` (replacing the Python prototype).
//   - RunReplay(): replays captured `<snapshot_dir>/<bot>/<seq>.json` snapshots
//     through the real C++ TriggerTracker, RenderScreen, BuildPlanRequest, and
//     ParsePlan against llama-server (capacity or real-time load mode).

#ifndef HARNESS_RUNTIME_REPLAY_H_
#define HARNESS_RUNTIME_REPLAY_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "llm/llm_client.h"

namespace harness {

struct ReplayOptions {
  std::string snapshot_dir;
  std::string mode = "load";  // "capacity" or "load"
  std::string rules_path = "content/RULES.md";
  int bots = 50;
  double plan_s = 30.0;
  int slots = 16;
  double seconds = 90.0;
  double warmup_s = 5.0;
  float temperature = 0.7f;
  LlmEndpoint endpoint;
};

// Parses `harness replay` CLI arguments.
std::optional<ReplayOptions> ParseReplayArgs(
    const std::vector<std::string>& args,
    std::string* error);

// Checks (or rewrites when `rewrite` is true) every `*.snapshot.json` in `dir`
// against `<stem>.txt` and `<stem>.options` using C++ `RenderScreen`.
// Returns 0 on match/success, 1 on diff or I/O error.
int RenderScreenGoldens(const std::string& dir,
                        bool rewrite,
                        std::string* report);

// Runs a capacity or load replay over captured snapshots using `caller` to
// invoke the LLM (production passes `CallLlama`; unit tests pass a mock).
int RunReplay(
    const ReplayOptions& options,
    const base::RepeatingCallback<LlmResult(const LlmRequest&)>& caller,
    std::string* report);

}  // namespace harness

#endif  // HARNESS_RUNTIME_REPLAY_H_
