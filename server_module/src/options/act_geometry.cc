// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/act_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
    float bot_radius_yd) {
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
  if (preferred_dir_pos) {
    const float pref = BearingFromTo(hazard.pos, *preferred_dir_pos, base);
    if (cur_dist < 1.0f || AngleDiffRad(base, pref) <= 0.5f * kPi) {
      base = pref;
    }
  }
  static constexpr float kOffsets[] = {0.0f,    30.0f,  -30.0f,  60.0f,
                                       -60.0f,  90.0f,  -90.0f,  120.0f,
                                       -120.0f, 150.0f, -150.0f, 180.0f};
  std::vector<pb::Vec3> spots;
  AppendFan(&spots, hazard.pos, base, target_dist, kOffsets,
            sizeof(kOffsets) / sizeof(kOffsets[0]));
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

}  // namespace llm_brain
