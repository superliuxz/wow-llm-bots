// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure geometry for standing-order relative places (`behind`, `in front of`,
// `max range of`, `<N> yd from`, `away [<thing>] [<N>]`). Generates ordered
// candidate world positions for the module's executor to validate against
// ground height, line of sight, and `PathGenerator`, and the cast-vs-move
// rules of a moving act.

#ifndef MOD_LLM_BRAIN_OPTIONS_ACT_GEOMETRY_H_
#define MOD_LLM_BRAIN_OPTIONS_ACT_GEOMETRY_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

inline constexpr float kDefaultMaxRangeYd = 28.0f;
inline constexpr float kDefaultAwayYd = 12.0f;
inline constexpr float kDefaultKiteYd = 20.0f;
inline constexpr float kDefaultSpreadRadiusYd = 10.0f;
inline constexpr float kDefaultBehindMarginYd = 2.5f;
inline constexpr float kDefaultOutOfMarginYd = 2.5f;
// A dodge prefers spots at least this far from mobs nobody has pulled (about
// where a mob a level or two above the bot notices it).
inline constexpr float kUnpulledClearYd = 18.0f;
// ... but not at the price of more than this much extra walking.
inline constexpr float kUnpulledMaxExtraWalkYd = 10.0f;

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

// Ordered candidate destination points for `out of <thing>`: moves just past
// `hazard_radius_yd + bot_radius_yd + margin_yd` from `hazard.pos`, biasing
// the escape bearing toward `preferred_dir_pos` (e.g. tank or group centroid)
// when it does not cross back through the hazard center. Returns empty if
// already outside (accounting for the bot's combat reach).
std::vector<wlb::snapshot::Vec3> CandidateSpotsForOutOf(
    const wlb::snapshot::Vec3& bot_pos,
    const AnchorGeometry& hazard,
    float hazard_radius_yd,
    std::optional<wlb::snapshot::Vec3> preferred_dir_pos = std::nullopt,
    float margin_yd = kDefaultOutOfMarginYd,
    float bot_radius_yd = 0.0f);

// Ordered tangential kiting waypoints around `anchor` at `kite_dist_yd`
// edge-to-edge, preferring a 45-degree arc around the anchor so the bot circles
// smoothly instead of backing straight into a wall.
std::vector<wlb::snapshot::Vec3> CandidateSpotsForKite(
    const wlb::snapshot::Vec3& bot_pos,
    const AnchorGeometry& anchor,
    float kite_dist_yd = kDefaultKiteYd,
    float bot_radius_yd = 0.0f);

// Ordered spread waypoints around `center` for bot `slot_index` of
// `total_bots`, assigning evenly spaced bearings (and two concentric rings
// when `total_bots > 8`) so a whole raid fans out without stacking on one spot.
std::vector<wlb::snapshot::Vec3> CandidateSpotsForSpread(
    const wlb::snapshot::Vec3& bot_pos,
    const AnchorGeometry& center,
    size_t slot_index,
    size_t total_bots,
    float radius_yd = kDefaultSpreadRadiusYd);

// Two tiers of dodge spots: those at least `clear_yd` from every point in
// `unpulled` (mobs nobody has pulled) and at most `max_extra_walk_yd` farther
// from `bot_pos` than the nearest spot come first, then the rest, each tier in
// its original order. Nothing is dropped, so a bot boxed in by packs still has
// every way out of a hazard, and a clear spot never costs a long walk through
// it.
std::vector<wlb::snapshot::Vec3> PreferSpotsClearOf(
    std::vector<wlb::snapshot::Vec3> spots,
    const wlb::snapshot::Vec3& bot_pos,
    const std::vector<wlb::snapshot::Vec3>& unpulled,
    float clear_yd = kUnpulledClearYd,
    float max_extra_walk_yd = kUnpulledMaxExtraWalkYd);

// Whether an anchor moved or turned enough since the last path calculation to
// justify recomputing a relative-place destination.
bool AnchorMovedSignificantly(const AnchorGeometry& prev,
                              const AnchorGeometry& cur,
                              bool cares_about_facing);

// The server holds a caster still while a cast (or a channel that doesn't
// allow moving) runs, so a move started mid-cast waits for it. A move act
// (`out of`, `away`, `kite`) cuts a cast or channel with more than
// kLetCastLandMs left and lets a nearly finished one land first.
inline constexpr int32_t kLetCastLandMs = 300;
bool CutCastForMove(int32_t remaining_ms, bool channel_allows_move);

// While a dodge moves out, a reflex spell that would hold the bot still (a
// cast time, or a channel that doesn't allow moving) waits until it is out.
bool CastHoldsMoveOut(uint32_t cast_time_ms,
                      bool channeled,
                      bool channel_allows_move);

// Safe group-follow geometry (Companion-lessons Gap 1): keeps followers behind
// the leader and off the aggro edge of unpulled hostile packs.
inline constexpr float kSafeFollowDistYd = 3.0f;
inline constexpr float kSafeFollowTriggerYd = 5.5f;

// Candidate follow spots around `leader` at `follow_dist_yd`, ordered behind
// the leader first, then partitioned so spots that are safe from every point
// in `unpulled` (at least `clear_yd` away, or at least as far from the pack as
// `leader.pos` is) come first. Never returns empty.
std::vector<wlb::snapshot::Vec3> CandidateSpotsForSafeFollow(
    const wlb::snapshot::Vec3& bot_pos,
    const AnchorGeometry& leader,
    const std::vector<wlb::snapshot::Vec3>& unpulled,
    float follow_dist_yd = kSafeFollowDistYd,
    float clear_yd = kUnpulledClearYd);

// True when `bot_pos` is within `guard_yd` of at least one unpulled hostile
// AND closer to it than `leader_pos` is (by >= 0.5 yd), meaning the follower
// is encroaching on a pack's aggro edge that the leader stayed clear of.
bool BotEncroachesUnpulled(const wlb::snapshot::Vec3& bot_pos,
                           const wlb::snapshot::Vec3& leader_pos,
                           const std::vector<wlb::snapshot::Vec3>& unpulled,
                           float guard_yd = kUnpulledClearYd);

// Healer range/LoS guard during a pull (Companion-lessons Gap 1, M1b): keeps a
// grouped healer within 40 yd (triggering at 34 yd for hysteresis, stopping
// around 28 yd) and line of sight of a combat tank, while yielding to stay
// policies, spread/combat position holds, active casts, brain forced-move acts
// (e.g. boss dodges), two-tank coverage (already in range & LoS of another
// living combat tank), and active friendly heal targets (e.g. Valithria
// Dreamwalker or an injured ally in range & LoS).
inline constexpr float kHealerRepositionDistYd = 34.0f;
inline constexpr float kHealerStopDistYd = 28.0f;

struct HealerPositionCheck {
  bool bot_alive = false;
  bool is_healer = false;
  bool in_group = false;
  bool stay = false;
  bool spread_or_position_hold = false;
  bool forced_move_active = false;
  bool casting = false;
  bool tank_alive = false;
  bool tank_in_combat = false;
  bool any_tank_in_range_and_los = false;
  bool heal_target_in_range_and_los = false;
  float dist_to_tank_yd = 0.0f;
  bool in_los_of_tank = true;
};

bool HealerNeedsTankReposition(const HealerPositionCheck& check);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_ACT_GEOMETRY_H_
