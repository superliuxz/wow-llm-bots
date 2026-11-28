// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/act_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDegToRad = kPi / 180.0f;

pb::Vec3 PointAt(const pb::Vec3& origin, float angle_rad, float dist_yd) {
  return {origin.x + std::cos(angle_rad) * dist_yd,
          origin.y + std::sin(angle_rad) * dist_yd, origin.z};
}

float BearingFromTo(const pb::Vec3& from,
                    const pb::Vec3& to,
                    float fallback_rad) {
  const float dx = to.x - from.x;
  const float dy = to.y - from.y;
  if (dx * dx + dy * dy < 1e-4f) {
    return fallback_rad;
  }
  return std::atan2(dy, dx);
}

float AngleDiffRad(float a, float b) {
  float d = std::fmod(a - b + kPi, 2.0f * kPi);
  if (d < 0.0f) {
    d += 2.0f * kPi;
  }
  return std::abs(d - kPi);
}

void AppendFan(std::vector<pb::Vec3>* out,
               const pb::Vec3& origin,
               float base_angle,
               float dist_yd,
               const float* deg_offsets,
               size_t count) {
  for (size_t i = 0; i < count; ++i) {
    out->push_back(
        PointAt(origin, base_angle + deg_offsets[i] * kDegToRad, dist_yd));
  }
}

}  // namespace

float Dist2D(const pb::Vec3& a, const pb::Vec3& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

float EdgeDist2D(const pb::Vec3& bot_pos,
                 float bot_radius_yd,
                 const AnchorGeometry& anchor) {
  return std::max(
      0.0f, Dist2D(bot_pos, anchor.pos) - bot_radius_yd - anchor.radius_yd);
}

std::vector<pb::Vec3> CandidateSpotsForPlace(pb::Command_Act::PlaceKind place,
                                             const pb::Vec3& bot_pos,
                                             const AnchorGeometry& anchor,
                                             float yd,
                                             float max_range_yd,
                                             float bot_radius_yd) {
  const float reach_sum =
      std::max(0.0f, bot_radius_yd) + std::max(0.0f, anchor.radius_yd);
  std::vector<pb::Vec3> spots;
  switch (place) {
    case pb::Command_Act::PLACE_UNSPECIFIED:
    case pb::Command_Act::AT:
      spots.push_back(anchor.pos);
      return spots;

    case pb::Command_Act::BEHIND: {
      const float dist = std::max(reach_sum + kDefaultBehindMarginYd, 3.0f);
      const float behind_angle = anchor.facing_rad + kPi;
      static constexpr float kOffsets[] = {0.0f,   20.0f, -20.0f, 40.0f,
                                           -40.0f, 60.0f, -60.0f};
      AppendFan(&spots, anchor.pos, behind_angle, dist, kOffsets,
                sizeof(kOffsets) / sizeof(kOffsets[0]));
      return spots;
    }

    case pb::Command_Act::IN_FRONT: {
      const float dist = std::max(reach_sum + kDefaultBehindMarginYd, 3.0f);
      static constexpr float kOffsets[] = {0.0f, 20.0f, -20.0f, 40.0f, -40.0f};
      AppendFan(&spots, anchor.pos, anchor.facing_rad, dist, kOffsets,
                sizeof(kOffsets) / sizeof(kOffsets[0]));
      return spots;
    }

    case pb::Command_Act::MAX_RANGE:
    case pb::Command_Act::YD_FROM: {
      const float edge_dist =
          (place == pb::Command_Act::MAX_RANGE)
              ? (max_range_yd > 0.0f ? max_range_yd : kDefaultMaxRangeYd)
              : std::max(yd, 1.0f);
      const float dist = edge_dist + reach_sum;
      const float base =
          BearingFromTo(anchor.pos, bot_pos, anchor.facing_rad + kPi);
      static constexpr float kOffsets[] = {0.0f,    30.0f,  -30.0f,  60.0f,
                                           -60.0f,  90.0f,  -90.0f,  120.0f,
                                           -120.0f, 150.0f, -150.0f, 180.0f};
      AppendFan(&spots, anchor.pos, base, dist, kOffsets,
                sizeof(kOffsets) / sizeof(kOffsets[0]));
      return spots;
    }
  }
  return spots;
}

std::vector<pb::Vec3> CandidateSpotsForAway(const pb::Vec3& bot_pos,
                                            const AnchorGeometry& anchor,
                                            float yd,
                                            float bot_radius_yd) {
  const float reach_sum =
      std::max(0.0f, bot_radius_yd) + std::max(0.0f, anchor.radius_yd);
  const float cur_edge_dist = EdgeDist2D(bot_pos, bot_radius_yd, anchor);
  if (yd > 0.0f && cur_edge_dist >= yd) {
    return {};
  }
  const float target_edge_dist =
      yd > 0.0f ? (yd + 1.5f) : (cur_edge_dist + kDefaultAwayYd);
  const float target_dist = target_edge_dist + reach_sum;
  const float base =
      BearingFromTo(anchor.pos, bot_pos, anchor.facing_rad + kPi);
  static constexpr float kOffsets[] = {0.0f,    30.0f,  -30.0f,  60.0f,
                                       -60.0f,  90.0f,  -90.0f,  120.0f,
                                       -120.0f, 150.0f, -150.0f, 180.0f};
  std::vector<pb::Vec3> spots;
  AppendFan(&spots, anchor.pos, base, target_dist, kOffsets,
            sizeof(kOffsets) / sizeof(kOffsets[0]));
  return spots;
}

std::vector<pb::Vec3> CandidateSpotsForOutOf(
    const pb::Vec3& bot_pos,
    const AnchorGeometry& hazard,
    float hazard_radius_yd,
    std::optional<pb::Vec3> preferred_dir_pos,
    float margin_yd,
    float bot_radius_yd,
    uint32_t attempt) {
  const float cur_dist = Dist2D(bot_pos, hazard.pos);
  const float reach = std::max(0.0f, bot_radius_yd);
  const float effective_hazard_radius =
      hazard_radius_yd > 0.0f ? (hazard_radius_yd + reach) : 0.0f;
  if (effective_hazard_radius > 0.0f &&
      cur_dist >= effective_hazard_radius + 0.5f) {
    return {};
  }
  const float effective_radius =
      effective_hazard_radius > 0.0f ? effective_hazard_radius : kDefaultAwayYd;
  const float target_dist = effective_radius + std::max(0.5f, margin_yd);
  float base = BearingFromTo(hazard.pos, bot_pos, hazard.facing_rad + kPi);
  if (attempt == 0 && preferred_dir_pos) {
    const float pref = BearingFromTo(hazard.pos, *preferred_dir_pos, base);
    if (cur_dist < 1.0f || AngleDiffRad(base, pref) <= 0.5f * kPi) {
      base = pref;
    }
  } else if (attempt > 0) {
    // Rotate the primary escape bearing on retry/repath so a blocked or
    // interrupted path tries an alternate escape vector first.
    static constexpr float kRetryRotationsDeg[] = {
        0.0f, 90.0f, -90.0f, 180.0f, 45.0f, -45.0f, 135.0f, -135.0f};
    const float rot_deg =
        kRetryRotationsDeg[attempt % (sizeof(kRetryRotationsDeg) /
                                      sizeof(kRetryRotationsDeg[0]))];
    base += rot_deg * (kPi / 180.0f);
  }
  static constexpr float kOffsets[] = {0.0f,    30.0f,  -30.0f,  60.0f,
                                       -60.0f,  90.0f,  -90.0f,  120.0f,
                                       -120.0f, 150.0f, -150.0f, 180.0f};
  std::vector<pb::Vec3> spots;
  AppendFan(&spots, hazard.pos, base, target_dist, kOffsets,
            sizeof(kOffsets) / sizeof(kOffsets[0]));
  // Tight-room fallback: if every spot at `target_dist` lands inside a wall or
  // obstacle, also offer a tighter ring just outside `effective_radius`.
  const float tight_dist = effective_radius + 1.0f;
  if (target_dist > tight_dist + 0.25f) {
    static constexpr float kTightOffsets[] = {0.0f,   45.0f,  -45.0f,  90.0f,
                                              -90.0f, 135.0f, -135.0f, 180.0f};
    AppendFan(&spots, hazard.pos, base, tight_dist, kTightOffsets,
              sizeof(kTightOffsets) / sizeof(kTightOffsets[0]));
  }
  return spots;
}

std::vector<pb::Vec3> CandidateSpotsForKite(const pb::Vec3& bot_pos,
                                            const AnchorGeometry& anchor,
                                            float kite_dist_yd,
                                            float bot_radius_yd) {
  const float reach_sum =
      std::max(0.0f, bot_radius_yd) + std::max(0.0f, anchor.radius_yd);
  const float dist =
      (kite_dist_yd > 0.0f ? kite_dist_yd : kDefaultKiteYd) + reach_sum;
  const float base =
      BearingFromTo(anchor.pos, bot_pos, anchor.facing_rad + kPi);
  // Prefer tangential steps around the kiting circle (+45 / -45 / +90 / -90),
  // falling back to straight away (0) and wider arcs if blocked by walls.
  static constexpr float kOffsets[] = {45.0f, -45.0f, 90.0f,   -90.0f,
                                       0.0f,  135.0f, -135.0f, 180.0f};
  std::vector<pb::Vec3> spots;
  AppendFan(&spots, anchor.pos, base, dist, kOffsets,
            sizeof(kOffsets) / sizeof(kOffsets[0]));
  return spots;
}

std::vector<pb::Vec3> CandidateSpotsForSpread(const pb::Vec3& /*bot_pos*/,
                                              const AnchorGeometry& center,
                                              size_t slot_index,
                                              size_t total_bots,
                                              float radius_yd) {
  const size_t n = std::max<size_t>(1, total_bots);
  const size_t idx = slot_index % n;
  const float base_radius =
      radius_yd > 0.0f ? radius_yd : kDefaultSpreadRadiusYd;
  float ring_radius = base_radius;
  float base = 0.0f;
  if (n <= 6) {
    base = (2.0f * kPi * static_cast<float>(idx)) / static_cast<float>(n);
  } else {
    // Concentric rings with radial and chord spacing >= max(base_radius, 12 yd)
    // so 10-man and 25-man raids maintain >= 12 yd pairwise separation while
    // staying within 36 yd (40 yd heal/spell range) of the center.
    const float spacing = std::max(base_radius, 12.0f);
    size_t remaining_idx = idx;
    size_t remaining_total = n;
    size_t ring = 1;
    while (true) {
      const size_t cap = 6 * ring;
      const size_t count_on_ring = std::min(cap, remaining_total);
      if (remaining_idx < count_on_ring) {
        ring_radius = spacing * static_cast<float>(ring);
        const float stagger =
            (ring % 2 == 0) ? (kPi / static_cast<float>(count_on_ring)) : 0.0f;
        base = stagger + (2.0f * kPi * static_cast<float>(remaining_idx)) /
                             static_cast<float>(count_on_ring);
        break;
      }
      remaining_idx -= count_on_ring;
      remaining_total -= count_on_ring;
      ++ring;
    }
  }
  static constexpr float kPrimaryOffsets[] = {0.0f,    25.0f, -25.0f, 50.0f,
                                              -50.0f,  90.0f, -90.0f, 135.0f,
                                              -135.0f, 180.0f};
  static constexpr float kFallbackOffsets[] = {0.0f,  45.0f,  -45.0f,
                                               90.0f, -90.0f, 180.0f};
  std::vector<pb::Vec3> spots;
  AppendFan(&spots, center.pos, base, ring_radius, kPrimaryOffsets,
            sizeof(kPrimaryOffsets) / sizeof(kPrimaryOffsets[0]));
  AppendFan(&spots, center.pos, base, ring_radius * 0.65f, kFallbackOffsets,
            sizeof(kFallbackOffsets) / sizeof(kFallbackOffsets[0]));
  return spots;
}

std::vector<pb::Vec3> PreferSpotsClearOf(std::vector<pb::Vec3> spots,
                                         const pb::Vec3& bot_pos,
                                         const std::vector<pb::Vec3>& unpulled,
                                         float clear_yd,
                                         float max_extra_walk_yd) {
  if (unpulled.empty() || spots.empty()) {
    return spots;
  }
  float nearest_yd = std::numeric_limits<float>::max();
  for (const pb::Vec3& spot : spots) {
    nearest_yd = std::min(nearest_yd, Dist2D(bot_pos, spot));
  }
  std::stable_partition(spots.begin(), spots.end(), [&](const pb::Vec3& spot) {
    return Dist2D(bot_pos, spot) <= nearest_yd + max_extra_walk_yd &&
           std::none_of(unpulled.begin(), unpulled.end(),
                        [&](const pb::Vec3& mob) {
                          return Dist2D(spot, mob) < clear_yd;
                        });
  });
  return spots;
}

bool AnchorMovedSignificantly(const AnchorGeometry& prev,
                              const AnchorGeometry& cur,
                              bool cares_about_facing) {
  if (Dist2D(prev.pos, cur.pos) >= 2.0f) {
    return true;
  }
  if (cares_about_facing &&
      AngleDiffRad(prev.facing_rad, cur.facing_rad) >= 0.35f) {
    return true;
  }
  return false;
}

bool CutCastForMove(int32_t remaining_ms, bool channel_allows_move) {
  return !channel_allows_move && remaining_ms > kLetCastLandMs;
}

bool CastHoldsMoveOut(uint32_t cast_time_ms,
                      bool channeled,
                      bool channel_allows_move) {
  return channeled ? !channel_allows_move : cast_time_ms > 0;
}

std::vector<pb::Vec3> CandidateSpotsForSafeFollow(
    const pb::Vec3& /*bot_pos*/,
    const AnchorGeometry& leader,
    const std::vector<pb::Vec3>& unpulled,
    float follow_dist_yd,
    float clear_yd) {
  const float dist = std::max(1.0f, follow_dist_yd);
  const float behind_angle = leader.facing_rad + kPi;
  static constexpr float kOffsets[] = {0.0f,    30.0f,  -30.0f,  60.0f,
                                       -60.0f,  90.0f,  -90.0f,  120.0f,
                                       -120.0f, 150.0f, -150.0f, 180.0f};
  std::vector<pb::Vec3> spots;
  AppendFan(&spots, leader.pos, behind_angle, dist, kOffsets,
            sizeof(kOffsets) / sizeof(kOffsets[0]));
  if (unpulled.empty()) {
    return spots;
  }
  // A follow spot is safe against every unpulled hostile if it stays at least
  // `clear_yd` away from that hostile OR at least as far from it as the leader
  // already is (so the follower never steps closer to a pack than the leader).
  std::stable_partition(spots.begin(), spots.end(), [&](const pb::Vec3& spot) {
    return std::all_of(unpulled.begin(), unpulled.end(),
                       [&](const pb::Vec3& mob) {
                         const float spot_d = Dist2D(spot, mob);
                         const float leader_d = Dist2D(leader.pos, mob);
                         return spot_d >= clear_yd || spot_d >= leader_d;
                       });
  });
  return spots;
}

bool BotEncroachesUnpulled(const pb::Vec3& bot_pos,
                           const pb::Vec3& leader_pos,
                           const std::vector<pb::Vec3>& unpulled,
                           float guard_yd) {
  for (const pb::Vec3& mob : unpulled) {
    const float bot_d = Dist2D(bot_pos, mob);
    const float leader_d = Dist2D(leader_pos, mob);
    if (bot_d < guard_yd && bot_d + 0.5f < leader_d) {
      return true;
    }
  }
  return false;
}

bool HealerNeedsTankReposition(const HealerPositionCheck& c) {
  if (!c.bot_alive || !c.is_healer || !c.in_group || c.stay ||
      c.spread_or_position_hold || c.forced_move_active || c.casting ||
      !c.tank_alive || !c.tank_in_combat || c.any_tank_in_range_and_los ||
      c.heal_target_in_range_and_los) {
    return false;
  }
  return c.dist_to_tank_yd > kHealerRepositionDistYd || !c.in_los_of_tank;
}

}  // namespace llm_brain
