// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// AzerothCore script hooks that record explicit gameplay events (damage_taken,
// cast_start, loot, quest_accept, quest_turnin) identically for both observed
// human players and tracked LLM bots (docs/plans/self-improving-loop.md, F).

#ifndef MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_SCRIPTS_H_
#define MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_SCRIPTS_H_

#include <atomic>
#include <string>
#include <vector>

#include "snapshot/behavior_trace.h"

namespace llm_brain {

// Shared state between LlmBrainWorldScript and the trace event scripts.
// `bot_names` and `observe_humans` are populated once at startup before
// `writer` is published with release semantics.
struct TraceHookState {
  std::vector<std::string> bot_names;  // Sorted for binary_search.
  bool observe_humans = false;
  std::atomic<BehaviorTraceWriter*> writer{nullptr};
};

// Registers the PlayerScript and UnitScript that emit unified TraceEvent lines.
void AddLlmBrainTraceScripts(TraceHookState* state);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_SCRIPTS_H_
