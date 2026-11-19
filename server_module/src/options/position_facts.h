// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTIONS_POSITION_FACTS_H_
#define MOD_LLM_BRAIN_OPTIONS_POSITION_FACTS_H_

#include <span>
#include <vector>

#include "snapshot/snapshot.h"

// The parts of the Explore / Go to facts that need no game server
// (option_facts.h computes them from the world): unit-tested.
namespace llm_brain {

inline constexpr float kMinimapRangeYd = 150.0f;
// Explore spots and Go to places nearer or farther than this are not offered.
inline constexpr float kMinTravelDistYd = 25.0f;
inline constexpr float kMaxTravelDistYd = 2500.0f;

float Dist(const wlb::snapshot::Vec3& a, float x, float y, float z);
wlb::snapshot::Direction DirFrom(const wlb::snapshot::Vec3& from,
                                 float x,
                                 float y);

// Compass angles (8 directions) and probe distances (yd) used when falling
// back to walkable ray probing in dungeons/cities with no grind buckets.
std::span<const float> ExploreRayAngles();
std::span<const float> ExploreRayDistancesYd();

// The Explore spots and Go to places seen from one position. Distances and
// directions are filled for the position they were computed at; RefreshFrom
// re-measures them from a nearby one (cheap), so a bot recomputes them only
// when it has moved or time has passed (SnapshotBuilder).
struct PositionFacts {
  std::vector<wlb::snapshot::OptionFacts::Spot> spots;
  std::vector<wlb::snapshot::OptionFacts::Place> places;
  // Hubs near enough to be on the minimap soon (TOWN / CAMP; `rel.position`
  // only: AddPositionFacts measures them).
  std::vector<wlb::snapshot::Place> minimap_hubs;
};

// Re-measures distance and direction from `pos`, dropping spots and places
// now nearer than the travel minimum or farther than the maximum.
void RefreshFrom(const wlb::snapshot::Vec3& pos, PositionFacts* facts);

// Adds the facts of `position` to `out` (spots, places) and the hubs within
// minimap range of `pos` to the minimap, then the nearest quest turn-in and
// objective from `quest_tracker` as places. The same code live
// (SnapshotBuilder) and offline (test/facts_dump.cc).
void AddPositionFacts(
    const PositionFacts& position,
    const wlb::snapshot::Vec3& pos,
    const std::vector<wlb::snapshot::Objective>& quest_tracker,
    wlb::snapshot::OptionFacts* out,
    wlb::snapshot::Minimap* minimap);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_POSITION_FACTS_H_
