// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_
#define MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/time/time.h"
#include "options/option_facts.h"
#include "options/options.h"
#include "options/position_facts.h"
#include "snapshot/nearby_builder.h"
#include "snapshot/snapshot.h"
#include "snapshot/ui_state.h"

class Player;

namespace llm_brain {
class ItemSpellTexts;
}  // namespace llm_brain

namespace llm_brain {

// Builds what one bot could see in its client right now
// (docs/specs/snapshot.md), plus the non-screen OptionFacts needed by
// AddOptions (docs/specs/options.md). Keeps per-bot state between
// snapshots (the line-of-sight cache, the Explore / Go to facts, quest map
// points), so use one per bot. World thread only.
class SnapshotBuilder {
 public:
  explicit SnapshotBuilder(NearbyBuilder::Options nearby_options,
                           DungeonNavConfig dungeon_nav = {});
  SnapshotBuilder(const SnapshotBuilder&) = delete;
  SnapshotBuilder& operator=(const SnapshotBuilder&) = delete;
  ~SnapshotBuilder();

  // One quest objective's map points (objective -1: the turn-in), cached.
  struct QuestPoi {
    uint32_t quest_id = 0;
    int32_t objective = 0;
    uint32_t map_id = 0;
    std::vector<std::pair<std::string, wlb::snapshot::Vec3>> spots;
  };

  // Fills `snapshot` and (when non-null) `facts`; the caller sets bot, seq and
  // time_ms. Without `world` (the module disabled) there are no Explore spots
  // or Go to places. `ui` (what the bot has open and points at,
  // docs/specs/options.md) is aged by one snapshot; `texts` may be
  // null.
  NearbyBuilder::Stats Build(Player* bot,
                             wlb::snapshot::Snapshot* snapshot,
                             OptionFacts* facts,
                             const WorldPlaces* world,
                             UiState* ui,
                             const ItemSpellTexts* texts,
                             base::TimeTicks now);

 private:
  void FillOptionFacts(Player* bot,
                       wlb::snapshot::Snapshot* snapshot,
                       OptionFacts* facts,
                       const WorldPlaces* world,
                       base::TimeTicks now);

  NearbyBuilder nearby_;
  DungeonNavConfig dungeon_nav_;
  std::vector<QuestPoi> quest_pois_;
  std::optional<PositionFacts> position_facts_;
  Viewpoint position_view_;
  base::TimeTicks position_time_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_
