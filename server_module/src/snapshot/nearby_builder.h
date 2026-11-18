// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_NEARBY_BUILDER_H_
#define MOD_LLM_BRAIN_SNAPSHOT_NEARBY_BUILDER_H_

#include <cstddef>

#include "snapshot/perception.h"
#include "snapshot/snapshot.h"

class Player;
class Unit;
class WorldObject;

namespace llm_brain {

// Fills base.nearby with everything in the bot's client visibility set
// (docs/specs/snapshot.md §4): units, objects and ground effects. No caps;
// detail by distance like the client: nameplate fields within name/nameplate
// range, model-level fields beyond it, and far objects out of line of sight
// left out. Line of sight is checked on a per-snapshot ray-cast budget through
// a per-bot cache.
//
// One per bot; world thread only.
class NearbyBuilder {
 public:
  struct Options {
    float nameplate_range_yd = 20;
    size_t raycasts_per_snapshot = 32;
  };
  // What one Build() did (for metrics).
  struct Stats {
    size_t units = 0;
    size_t objects = 0;
    size_t areas = 0;
    size_t raycasts = 0;
    // Left out because a player can't see it: far and out of line of sight,
    // or a trigger or invisible model.
    size_t hidden = 0;
  };

  explicit NearbyBuilder(Options options);
  NearbyBuilder(const NearbyBuilder&) = delete;
  NearbyBuilder& operator=(const NearbyBuilder&) = delete;
  ~NearbyBuilder();

  Stats Build(Player* bot, wlb::snapshot::Nearby* nearby);

  // Fills `out` with model, relative position, cached LoS, and nameplate
  // fields (for unit frames: target, attackers).
  void FillUnit(Player* bot, Unit* unit, wlb::snapshot::Unit* out);

  // Fills `rel` with distance, compass direction, height, coordinates, and
  // cached LoS from `bot` to `object`.
  void FillUnitRelative(Player* bot,
                        WorldObject* object,
                        wlb::snapshot::Relative* rel);

 private:
  const Options options_;
  LosCache los_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_NEARBY_BUILDER_H_
