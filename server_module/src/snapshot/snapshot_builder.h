// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_
#define MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_

#include "options/options.h"
#include "proto/snapshot.h"
#include "snapshot/nearby_builder.h"

class Player;

namespace llm_brain {

// Builds what one bot could see in its client right now
// (docs/specs/snapshot.md), plus the non-screen OptionFacts needed by
// AddOptions (docs/plans/options.md). Keeps per-bot state between snapshots
// (the line-of-sight cache), so use one per bot. World thread only.
class SnapshotBuilder {
 public:
  explicit SnapshotBuilder(NearbyBuilder::Options nearby_options);
  SnapshotBuilder(const SnapshotBuilder&) = delete;
  SnapshotBuilder& operator=(const SnapshotBuilder&) = delete;
  ~SnapshotBuilder();

  // Fills `snapshot` and (when non-null) `facts`; the caller sets bot, seq and
  // time_ms.
  NearbyBuilder::Stats Build(Player* bot,
                             wlb::snapshot::Snapshot* snapshot,
                             OptionFacts* facts = nullptr);

 private:
  NearbyBuilder nearby_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_
