// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/position_facts.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "base/strings/stringprintf.h"
#include "snapshot/perception.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

namespace pb = wlb::snapshot;

float Dist(const pb::Vec3& a, float x, float y, float z) {
  return std::sqrt((a.x - x) * (a.x - x) + (a.y - y) * (a.y - y) +
                   (a.z - z) * (a.z - z));
}

pb::Direction DirFrom(const pb::Vec3& from, float x, float y) {
  return CompassDirection(x - from.x, y - from.y);
}

std::span<const float> ExploreRayAngles() {
  constexpr float kPi = std::numbers::pi_v<float>;
  static constexpr float kAngles[] = {0.0f, kPi / 4,      kPi / 2,  3 * kPi / 4,
                                      kPi,  -3 * kPi / 4, -kPi / 2, -kPi / 4};
  return kAngles;
}

std::span<const float> ExploreRayDistancesYd() {
  static constexpr float kDists[] = {60.0f, 35.0f, 80.0f};
  return kDists;
}

void RefreshFrom(const pb::Vec3& pos, PositionFacts* facts) {
  const auto out_of_range = [](float dist) {
    return dist < kMinTravelDistYd || dist > kMaxTravelDistYd;
  };
  std::erase_if(facts->spots, [&](pb::OptionFacts::Spot& spot) {
    spot.dist_yd = Dist(pos, spot.pos.x, spot.pos.y, spot.pos.z);
    spot.dir = DirFrom(pos, spot.pos.x, spot.pos.y);
    return out_of_range(spot.dist_yd) ||
           spot.dir == pb::DIRECTION_UNSPECIFIED || spot.dir == pb::HERE;
  });
  std::sort(facts->spots.begin(), facts->spots.end(),
            [](const pb::OptionFacts::Spot& a, const pb::OptionFacts::Spot& b) {
              return a.dist_yd < b.dist_yd;
            });
  uint32_t seen_dirs = 0;
  std::erase_if(facts->spots, [&](const pb::OptionFacts::Spot& spot) {
    const uint32_t bit = 1u << static_cast<uint8_t>(spot.dir);
    if ((seen_dirs & bit) != 0) {
      return true;
    }
    seen_dirs |= bit;
    return false;
  });
  std::erase_if(facts->places, [&](pb::OptionFacts::Place& place) {
    place.dist_yd = Dist(pos, place.pos.x, place.pos.y, place.pos.z);
    place.dir = DirFrom(pos, place.pos.x, place.pos.y);
    return out_of_range(place.dist_yd);
  });
}

namespace {

void AddMinimapHubs(const PositionFacts& position,
                    const pb::Vec3& pos,
                    pb::Minimap* minimap) {
  for (const pb::Place& hub : position.minimap_hubs) {
    const pb::Vec3& at = hub.rel.position;
    const float dist = Dist(pos, at.x, at.y, at.z);
    if (dist > kMinimapRangeYd) {
      continue;
    }
    pb::Place& place = minimap->places.emplace_back(hub);
    place.rel.dist_yd = dist;
    place.rel.dir = DirFrom(pos, at.x, at.y);
    place.rel.height = HeightOf(at.z - pos.z);
  }
}

}  // namespace

void AddPositionFacts(const PositionFacts& position,
                      const pb::Vec3& pos,
                      const std::vector<pb::Objective>& quest_tracker,
                      pb::OptionFacts* out,
                      pb::Minimap* minimap) {
  out->spots = position.spots;
  out->places = position.places;
  AddMinimapHubs(position, pos, minimap);

  // The nearest completed quest's turn-in and the nearest open objective.
  const pb::Objective* best_turnin = nullptr;
  const pb::Objective* best_objective = nullptr;
  for (const pb::Objective& obj : quest_tracker) {
    if (!obj.where || !obj.quest || obj.where->rel.dist_yd < kMinTravelDistYd) {
      continue;
    }
    if (obj.where->rel.dist_yd <= kMinimapRangeYd) {
      minimap->objective_areas.push_back(*obj.where);
    }
    const auto nearer = [&](const pb::Objective* best) {
      return !best || obj.where->rel.dist_yd < best->where->rel.dist_yd;
    };
    if (obj.quest->complete) {
      if (nearer(best_turnin)) {
        best_turnin = &obj;
      }
    } else if (!obj.done && nearer(best_objective)) {
      best_objective = &obj;
    }
  }
  for (const pb::Objective* obj : {best_turnin, best_objective}) {
    if (!obj) {
      continue;
    }
    const pb::Vec3& at = obj->where->rel.position;
    out->places.push_back(
        {base::StringPrintf("place:quest:%u:%s", obj->quest->quest_id,
                            obj->quest->complete ? "turnin" : "objective"),
         obj->where->name,
         (obj->quest->complete ? "turn in: " : "quest: ") + obj->quest->quest,
         obj->where->rel.dir, obj->where->rel.dist_yd, at});
  }
}

}  // namespace llm_brain
