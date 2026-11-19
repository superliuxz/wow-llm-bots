// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure geometry for standing-order relative places (`behind`, `in front of`,
// `max range of`, `<N> yd from`, `away [<thing>] [<N>]`). Generates ordered
// candidate world positions for the module's executor to validate against
// ground height, line of sight, and `PathGenerator`.

#ifndef MOD_LLM_BRAIN_OPTIONS_ACT_GEOMETRY_H_
#define MOD_LLM_BRAIN_OPTIONS_ACT_GEOMETRY_H_

#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

inline constexpr float kDefaultMaxRangeYd = 28.0f;
inline constexpr float kDefaultAwayYd = 12.0f;
inline constexpr float kDefaultKiteYd = 20.0f;
inline constexpr float kDefaultBehindMarginYd = 2.5f;

struct AnchorGeometry {
  wlb::snapshot::Vec3 pos;
  // AzerothCore orientation in radians (0 = +X, pi/2 = +Y).
  float facing_rad = 0.0f;
  // Combat/bounding radius of the anchor unit (0 for objects/areas).
  float radius_yd = 0.0f;
};

// 2D horizontal distance between two world points.
float Dist2D(const wlb::snapshot::Vec3& a, const wlb::snapshot::Vec3& b);

// Edge-to-edge horizontal distance between a bot (combat reach `bot_radius_yd`)
// and `anchor` (combat reach `anchor.radius_yd`), matching `Unit::GetDistance`.
float EdgeDist2D(const wlb::snapshot::Vec3& bot_pos,
                 float bot_radius_yd,
                 const AnchorGeometry& anchor);

// Ordered candidate destination points for `to <place>` relative to `anchor`.
// The first candidate is the ideal spot; subsequent candidates fan out around
// the ideal bearing so a wall or blocked LoS falls back to a valid nearby spot.
// Distances (`yd`, `max_range_yd`) are measured edge-to-edge (adding
// `bot_radius_yd + anchor.radius_yd` to the centre-to-centre radius).
std::vector<wlb::snapshot::Vec3> CandidateSpotsForPlace(
    wlb::snapshot::Command_Act::PlaceKind place,
    const wlb::snapshot::Vec3& bot_pos,
    const AnchorGeometry& anchor,
    float yd,
    float max_range_yd = kDefaultMaxRangeYd,
    float bot_radius_yd = 0.0f);

// Ordered candidate destination points for `away <thing> [<yd>]` starting from
// `bot_pos` relative to `anchor`, measured edge-to-edge. Returns an empty
// vector if `yd > 0` and the bot is already at least `yd` edge-to-edge away
// from `anchor`.
std::vector<wlb::snapshot::Vec3> CandidateSpotsForAway(
    const wlb::snapshot::Vec3& bot_pos,
    const AnchorGeometry& anchor,
    float yd,
    float bot_radius_yd = 0.0f);

// Ordered tangential kiting waypoints around `anchor` at `kite_dist_yd`
// edge-to-edge, preferring a 45-degree arc around the anchor so the bot circles
// smoothly instead of backing straight into a wall.
std::vector<wlb::snapshot::Vec3> CandidateSpotsForKite(
    const wlb::snapshot::Vec3& bot_pos,
    const AnchorGeometry& anchor,
    float kite_dist_yd = kDefaultKiteYd,
    float bot_radius_yd = 0.0f);

// Whether an anchor moved or turned enough since the last path calculation to
// justify recomputing a relative-place destination.
bool AnchorMovedSignificantly(const AnchorGeometry& prev,
                              const AnchorGeometry& cur,
                              bool cares_about_facing);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_ACT_GEOMETRY_H_
