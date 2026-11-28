// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_
#define MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "options/option_facts.h"
#include "options/options.h"
#include "options/position_facts.h"
#include "snapshot/nearby_builder.h"
#include "snapshot/raid_meta.h"
#include "snapshot/snapshot.h"
#include "snapshot/ui_state.h"

class Player;
class SpellInfo;

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

  // One active, non-passive, non-tradeskill spell from the bot's spellbook,
  // cached across snapshots so 4 Hz combat builds only refresh cooldowns and
  // usability instead of rescanning the full spell map.
  struct CachedSpell {
    base::raw_ptr<const SpellInfo> info = nullptr;
    uint32_t id = 0;
    std::string name;
    uint32_t rank = 0;
    float cooldown_s = 0.0f;
    bool ground_targeted = false;
  };

 private:
  void FillActionBars(Player* bot,
                      wlb::snapshot::Extensions* ext,
                      base::TimeTicks now);
  void FillOptionFacts(Player* bot,
                       wlb::snapshot::Snapshot* snapshot,
                       OptionFacts* facts,
                       const WorldPlaces* world,
                       bool quest_log_open,
                       base::TimeTicks now);

  NearbyBuilder nearby_;
  DungeonNavConfig dungeon_nav_;
  std::vector<QuestPoi> quest_pois_;
  std::vector<CachedSpell> spell_cache_;
  uint32_t friendly_dispel_mask_ = 0;
  size_t spell_cache_map_size_ = 0;
  uint8_t spell_cache_spec_mask_ = 0;
  base::TimeTicks spell_cache_time_;
  BossTimerTracker boss_timers_;
  std::optional<PositionFacts> position_facts_;
  Viewpoint position_view_;
  base::TimeTicks position_time_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_SNAPSHOT_BUILDER_H_
