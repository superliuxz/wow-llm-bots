// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_BUILDER_H_
#define MOD_LLM_BRAIN_SNAPSHOT_BUILDER_H_

#include "nearby_builder.h"
#include "proto/snapshot.pb.h"

class Player;

namespace llm_brain {

// Builds what one bot could see in its client right now
// (docs/specs/snapshot.md). So far: base.self, base.where, base.nearby. Keeps
// per-bot state between snapshots (the line-of-sight cache), so use one per
// bot. World thread only.
class SnapshotBuilder {
 public:
  explicit SnapshotBuilder(NearbyBuilder::Options nearby_options);
  SnapshotBuilder(const SnapshotBuilder&) = delete;
  SnapshotBuilder& operator=(const SnapshotBuilder&) = delete;
  ~SnapshotBuilder();

  // Fills `snapshot`; the caller sets bot, seq and time_ms.
  NearbyBuilder::Stats Build(Player* bot, wlb::snapshot::Snapshot* snapshot);

 private:
  NearbyBuilder nearby_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_BUILDER_H_
