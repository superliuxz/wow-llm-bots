// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_BUILDER_H_
#define MOD_LLM_BRAIN_SNAPSHOT_BUILDER_H_

#include "proto/snapshot.pb.h"

class Player;

namespace llm_brain {

// World thread. Fills `snapshot` with what `bot` could see in its client right
// now (docs/specs/snapshot.md). So far: base.self and base.where. The caller
// sets bot, seq and time_ms.
void BuildSnapshot(Player* bot, wlb::snapshot::Snapshot* snapshot);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_BUILDER_H_
