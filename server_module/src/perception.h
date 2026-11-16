// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// What a player perceives of the world around them, as pure functions and
// small policies (no AzerothCore dependency, unit-tested). The AzerothCore side
// (nearby_builder.cc) feeds them positions and levels.

#ifndef MOD_LLM_BRAIN_PERCEPTION_H_
#define MOD_LLM_BRAIN_PERCEPTION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "base/time/time.h"
#include "proto/snapshot.pb.h"

namespace llm_brain {

// Compass direction from the bot to a point, from world-coordinate offsets
// (WoW: +x is north, +y is west). Within `here_yd` horizontally: HERE.
wlb::snapshot::Direction CompassDirection(float dx,
                                          float dy,
                                          float here_yd = 2);

// ABOVE / LEVEL / BELOW, with a few yards of slack (stairs, slopes).
wlb::snapshot::Relative::Height HeightOf(float dz);

// The colour of a unit's level as the client shows it to a player of
// `my_level` ("??" is SKULL_LEVEL: ten or more levels above).
wlb::snapshot::Unit::LevelColor LevelColor(int my_level, int their_level);

// The class name the client shows for a class id (1 Warrior ... 11 Druid);
// "" for unknown ids. (ChrClasses.dbc's name column isn't loaded by the
// server.)
const char* ClassName(uint8_t class_id);

// Rough model size from the unit's bounding radius (players ~0.3-0.4).
wlb::snapshot::Unit::Size SizeFromBoundingRadius(float radius);

// True if the client draws nothing for a creature with this display id (an
// "invisible stalker" model: generators, spawners, trigger markers).
bool IsInvisibleModel(uint32_t display_id);

// A creature a player can't see even though the server lists it as visible:
// a trigger (a mechanic marker, never shown to players) or one with an
// invisible model. These must not appear in the snapshot.
bool HiddenFromPlayers(bool is_trigger, uint32_t display_id);

// Line-of-sight results per object, refreshed on a budget: a ray cast against
// the map geometry is too expensive to do for every visible object every
// snapshot. Near objects are refreshed more often than far ones.
class LosCache {
 public:
  struct Candidate {
    uint64_t guid = 0;
    float dist_yd = 0;
  };

  // `near_yd`: objects closer than this are rechecked after `near_ttl`, others
  // after `far_ttl`.
  LosCache(float near_yd, base::TimeDelta near_ttl, base::TimeDelta far_ttl);
  LosCache(const LosCache&) = delete;
  LosCache& operator=(const LosCache&) = delete;
  ~LosCache();

  // The last known result, however old (until pruned): a few seconds' delay
  // in line of sight is fine, and far better than leaving the object out.
  // Freshness only decides what PickChecks re-checks next.
  std::optional<bool> Get(uint64_t guid) const;
  void Put(uint64_t guid, bool in_los, float dist_yd, base::TimeTicks now);

  // Which candidates to ray-cast now, at most `budget`: never-checked ones
  // first (nearest first), then stale ones (least recently checked first), so
  // a crowd standing next to the bot can't starve farther objects.
  std::vector<uint64_t> PickChecks(std::vector<Candidate> candidates,
                                   base::TimeTicks now,
                                   size_t budget) const;

  // Forgets results older than `max_age` (objects that left view).
  void Prune(base::TimeTicks now, base::TimeDelta max_age);

  // Where the bot (the viewer) is now. A result is only valid from where it
  // was taken: once the bot has moved more than `moved_yd` since the results
  // were taken (walked round a pillar) or changed map (a teleport), they are
  // all forgotten and re-checked.
  void SetViewer(uint32_t map_id, float x, float y, float z, float moved_yd);

  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    bool in_los = false;
    float dist_yd = 0;
    base::TimeTicks checked;
  };

  bool IsFresh(const Entry& entry, base::TimeTicks now) const;

  struct Viewer {
    uint32_t map_id = 0;
    float x = 0, y = 0, z = 0;
  };
  std::optional<Viewer> viewer_;

  const float near_yd_;
  const base::TimeDelta near_ttl_;
  const base::TimeDelta far_ttl_;
  std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_PERCEPTION_H_
