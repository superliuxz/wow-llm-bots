// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_movement.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Optional.h"  // IWYU pragma: keep

#include "G3D/Vector3.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "MoveSplineInitArgs.h"
#include "PathGenerator.h"
#include "Player.h"
#include "UnitDefines.h"
#include "base/time/time.h"
#include "options/action_types.h"
#include "options/exec_shared.h"
#include "options/option_facts.h"
#include "options/position_facts.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr float kWalkStepYd = 70.0f;
constexpr float kWalkStepAdvanceYd = 10.0f;
constexpr float kWalkNearEnoughArrivalYd = 12.0f;
constexpr base::TimeDelta kWalkRepathEvery = base::Seconds(1);

G3D::Vector3 PointAlongPath(const Movement::PointsArray& points,
                            float step_yd) {
  if (points.empty()) {
    return G3D::Vector3();
  }
  float acc = 0.0f;
  for (size_t i = 1; i < points.size(); ++i) {
    const G3D::Vector3 seg = points[i] - points[i - 1];
    const float len = seg.length();
    if (acc + len >= step_yd && len > 0.0f) {
      const float t = (step_yd - acc) / len;
      return points[i - 1] + seg * t;
    }
    acc += len;
  }
  return points.back();
}

}  // namespace

WalkStepResult TickLongWalk(Player* bot,
                            base::TimeTicks now,
                            PendingWalk* walk,
                            const DungeonNavConfig& dungeon_nav) {
  const auto stop_walk = [&]() {
    ClearLastMovement(bot);
    if (bot->isMoving()) {
      bot->StopMoving();
    }
  };
  if (!walk) {
    return {.error = "walk interrupted"};
  }
  if (!bot->IsAlive()) {
    stop_walk();
    return {.error = "walk interrupted: you died"};
  }
  if (bot->IsInCombat()) {
    stop_walk();
    return {.error = "walk interrupted: you are in combat"};
  }
  if (bot->GetMapId() != walk->map_id) {
    stop_walk();
    return {.error = "walk interrupted: you changed map"};
  }
  const float dist_to_dest =
      bot->GetExactDist(walk->dest.x, walk->dest.y, walk->dest.z);
  const pb::Vec3 pos{bot->GetPositionX(), bot->GetPositionY(),
                     bot->GetPositionZ()};
  switch (CheckWalkProgress(pos, dist_to_dest, now, &walk->progress)) {
    case WalkProgressVerdict::kArrived:
      stop_walk();
      return {.arrived = true};
    case WalkProgressVerdict::kStuck:
      stop_walk();
      return {.error = "stuck"};
    case WalkProgressVerdict::kContinue:
      break;
  }

  if (walk->has_step && bot->isMoving()) {
    const float dist_to_step = bot->GetExactDist(
        walk->step_dest.x, walk->step_dest.y, walk->step_dest.z);
    const float advance_yd =
        walk->consecutive_seam_hops > 0 ? 2.0f : kWalkStepAdvanceYd;
    if (dist_to_step > advance_yd) {
      return {};
    }
  }
  if (!walk->last_path_at.is_null() &&
      now - walk->last_path_at < kWalkRepathEvery) {
    return {};
  }
  walk->last_path_at = now;

  const base::TimeTicks path_start = base::TimeTicks::Now();
  constexpr uint32_t kTypeOk =
      PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
  G3D::Vector3 next_pt;
  bool found = false;

  {
    PathGenerator path(bot);
    path.CalculatePath(walk->dest.x, walk->dest.y, walk->dest.z);
    const uint32_t path_type = path.GetPathType();
    if ((path_type & ~kTypeOk) == 0 && path.GetPath().size() >= 2) {
      const G3D::Vector3 cand = PointAlongPath(path.GetPath(), kWalkStepYd);
      const G3D::Vector3& end = path.GetActualEndPosition();
      const float end_dist_to_dest =
          std::hypot(end.x - walk->dest.x, end.y - walk->dest.y);
      if (bot->GetExactDist(cand.x, cand.y, cand.z) >= 2.0f &&
          ((path_type & PATHFIND_NORMAL) != 0 ||
           end_dist_to_dest + 2.0f < dist_to_dest)) {
        next_pt = cand;
        found = true;
      }
    }
  }

  if (!found && dist_to_dest <= kWalkNearEnoughArrivalYd) {
    stop_walk();
    return {.arrived = true, .path_time = base::TimeTicks::Now() - path_start};
  }

  if (!found) {
    Map* map = bot->GetMap();
    const float base_angle = bot->GetAngle(walk->dest.x, walk->dest.y);
    const float step_len = std::min(kWalkStepYd, dist_to_dest);
    static constexpr float kOffsets[] = {0.0f, 0.7f, -0.7f};
    for (float offset : kOffsets) {
      const float angle = base_angle + offset;
      const float tx = bot->GetPositionX() + std::cos(angle) * step_len;
      const float ty = bot->GetPositionY() + std::sin(angle) * step_len;
      float tz = walk->dest.z;
      if (map) {
        const float h = map->GetHeight(bot->GetPhaseMask(), tx, ty,
                                       bot->GetPositionZ() + 10.0f);
        if (h != INVALID_HEIGHT && h != VMAP_INVALID_HEIGHT_VALUE) {
          tz = h;
        }
      }
      PathGenerator path(bot);
      path.CalculatePath(tx, ty, tz);
      if ((path.GetPathType() & ~kTypeOk) == 0 && path.GetPath().size() >= 2) {
        const G3D::Vector3 cand = PointAlongPath(path.GetPath(), kWalkStepYd);
        if (bot->GetExactDist(cand.x, cand.y, cand.z) >= 3.0f) {
          next_pt = cand;
          found = true;
          break;
        }
      }
    }
  }

  const base::TimeDelta path_time = base::TimeTicks::Now() - path_start;
  if (!found) {
    constexpr uint8_t kMaxConsecutiveSeamHops = 2;
    if (walk->consecutive_seam_hops < kMaxConsecutiveSeamHops) {
      pb::Vec3 hop;
      std::vector<pb::Vec3> hop_path;
      if (FindDungeonSeamHop(bot->GetMap(), bot->GetPhaseMask(), pos,
                             bot->GetAngle(walk->dest.x, walk->dest.y),
                             dist_to_dest, dungeon_nav, &hop, &hop_path)) {
        if (!CanClearBotMovement(bot) ||
            bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) ||
            bot->HasUnitState(UNIT_STATE_NOT_MOVE) ||
            bot->IsMovementPreventedByCasting()) {
          return {.path_time = path_time};
        }
        bot->GetMotionMaster()->Clear(/*reset=*/false);
        Movement::MoveSplineInit init(bot);
        if (hop_path.size() >= 2) {
          Movement::PointsArray points;
          points.reserve(hop_path.size());
          for (const pb::Vec3& p : hop_path) {
            points.emplace_back(p.x, p.y, p.z);
          }
          init.MovebyPath(points);
        } else {
          init.MoveTo(hop.x, hop.y, hop.z, /*generatePath=*/false);
        }
        init.SetWalk(false);
        if (init.Launch() > 0) {
          ++walk->consecutive_seam_hops;
          walk->step_dest = hop;
          walk->has_step = true;
          RecordLastMovement(bot, hop.x, hop.y, hop.z);
          return {.path_time = path_time};
        }
      }
    }
    ClearLastMovement(bot);
    if (bot->isMoving()) {
      bot->StopMoving();
    }
    return {.error = "stuck", .path_time = path_time};
  }

  walk->consecutive_seam_hops = 0;
  walk->step_dest = {next_pt.x, next_pt.y, next_pt.z};
  walk->has_step = true;
  if (!WalkTo(bot, next_pt.x, next_pt.y, next_pt.z)) {
    return {.error = "stuck", .path_time = path_time};
  }
  return {.path_time = path_time};
}

WalkStepResult StartWalkAction(Player* bot,
                               const WalkAction& action,
                               PendingWalk* walk,
                               const DungeonNavConfig& dungeon_nav) {
  if (bot->IsInCombat()) {
    return {.error = "you are in combat"};
  }
  if (!action.dest || !walk) {
    return {.error = "no destination for " + action.name};
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  *walk = PendingWalk{
      .map_id = bot->GetMapId(),
      .dest = *action.dest,
      .step_dest = {},
      .has_step = false,
      .progress =
          {
              .progress_pos = {bot->GetPositionX(), bot->GetPositionY(),
                               bot->GetPositionZ()},
              .last_progress_at = now,
              .best_dist_to_dest = bot->GetExactDist(
                  action.dest->x, action.dest->y, action.dest->z),
              .last_closer_at = now,
          },
      .last_path_at = {},
  };
  return TickLongWalk(bot, now, walk, dungeon_nav);
}

}  // namespace llm_brain
