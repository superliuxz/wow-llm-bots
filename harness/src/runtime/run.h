// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// `harness run`: connects to `mod-llm-brain`'s Unix socket, asks `llama-server`
// when bots wake, streams `do:` steps back as `Command.Choose`, and logs
// decisions and delivery ages until stopped (`SIGINT`/`SIGTERM` or
// `--seconds`).

#ifndef HARNESS_RUNTIME_RUN_H_
#define HARNESS_RUNTIME_RUN_H_

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/functional/callback.h"
#include "base/synchronization/waitable_event.h"
#include "llm/llm_client.h"

namespace harness {

struct RunOptions {
  std::string socket_path = "out/server/run/brain.sock";
  std::string rules_path = "content/RULES.md";
  std::string bots_dir = "var/bots";
  size_t slots = 16;
  float temperature = 0.7f;
  double seconds = 0;  // 0: run until signaled to stop.
  // Optional allow-list of bot names to drive (empty: drive all bots).
  std::unordered_set<std::string> only_bots;
  LlmEndpoint endpoint;
};

std::optional<RunOptions> ParseRunArgs(const std::vector<std::string>& args,
                                       std::string* error);

// Runs the live harness loop until `stop_event` is signaled (if provided) or
// `options.seconds` elapses (when > 0). Returns 0 on clean shutdown, 1 on
// startup error (e.g. unreadable rules file).
int RunHarness(const RunOptions& options,
               base::WaitableEvent* stop_event = nullptr,
               base::RepeatingCallback<void(const std::string&)> log_fn = {});

}  // namespace harness

#endif  // HARNESS_RUNTIME_RUN_H_
