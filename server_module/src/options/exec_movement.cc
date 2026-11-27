// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_movement.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Optional.h"  // IWYU pragma: keep

#include "Cell.h"
#include "CellImpl.h"  // IWYU pragma: keep
#include "Creature.h"
#include "G3D/Vector3.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"  // IWYU pragma: keep
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "MoveSplineInitArgs.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/time/time.h"
#include "options/action_types.h"
#include "options/exec_shared.h"
#include "options/leader_travel.h"
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

Creature* FindNearbyFlightMaster(Player* bot, float radius_yd) {
  std::vector<Unit*> units;
  Acore::AnyUnitInObjectRangeCheck check(bot, radius_yd);
  Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units,
                                                                     check);
  Cell::VisitObjects(bot, searcher, radius_yd);
  Creature* best = nullptr;
  float best_dist = radius_yd + 1.0f;
  for (Unit* u : units) {
    Creature* c = u->ToCreature();
    if (!c || !c->IsAlive() || !c->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER) ||
        bot->IsHostileTo(c)) {
      continue;
    }
    const float d = bot->GetDistance(c);
    if (d < best_dist) {
      best = c;
      best_dist = d;
    }
  }
  return best;
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

  // Legitimate cross-map / flight-path arrival handlers (Milestone 5b, #7).
  if (walk->arrival.kind == TravelLegKind::kAreaTrigger) {
    const AreaTrigger* at =
        sObjectMgr->GetAreaTrigger(walk->arrival.trigger_id);
    if (!at || at->map != bot->GetMapId()) {
      stop_walk();
      return {.error = "portal trigger is not on this map"};
    }
    if (bot->IsInAreaTriggerRadius(at, 0.0f) || dist_to_dest <= 3.0f) {
      stop_walk();
      if (WorldSession* sess = bot->GetSession()) {
        WorldPacket pkt(CMSG_AREATRIGGER, 4);
        pkt << uint32_t(walk->arrival.trigger_id);
        pkt.rpos(0);
        sess->HandleAreaTriggerOpcode(pkt);
        return {.arrived = true};
      }
      return {.error = "no session for portal trigger"};
    }
  } else if (walk->arrival.kind == TravelLegKind::kPortalObject &&
             dist_to_dest <= 15.0f) {
    if (GameObject* go = bot->FindNearestGameObject(
            walk->arrival.object_entry, 20.0f, /*onlySpawned=*/true)) {
      if (IsInUseRange(bot, go)) {
        stop_walk();
        go->Use(bot);
        return {.arrived = true};
      }
      if (!bot->isMoving()) {
        WalkNextTo(bot, go);
        return {};
      }
    }
  } else if (walk->arrival.kind == TravelLegKind::kFlightMaster &&
             dist_to_dest <= 25.0f) {
    if (Creature* fm = FindNearbyFlightMaster(bot, 35.0f)) {
      if (IsInTalkRange(bot, fm)) {
        stop_walk();
        if (!bot->ActivateTaxiPathTo(walk->arrival.taxi_nodes, fm, 0)) {
          return {.error = "could not start flight"};
        }
        return {.arrived = true};
      }
      if (!bot->isMoving()) {
        WalkNextTo(bot, fm);
        return {};
      }
    }
  }

  const pb::Vec3 pos{bot->GetPositionX(), bot->GetPositionY(),
                     bot->GetPositionZ()};
  switch (CheckWalkProgress(pos, dist_to_dest, now, &walk->progress)) {
    case WalkProgressVerdict::kArrived:
      if (walk->arrival.kind == TravelLegKind::kWalk) {
        stop_walk();
        return {.arrived = true};
      }
      // For portal/trigger/flight-master legs, arriving within 10 yd is not
      // enough until the trigger/object/NPC is actually activated above; step
      // closer if not yet in range.
      break;
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
      } else if (walk->arrival.kind == TravelLegKind::kAreaTrigger &&
                 dist_to_dest <= 15.0f &&
                 end_dist_to_dest + 0.5f < dist_to_dest &&
                 bot->GetExactDist(end.x, end.y, end.z) >= 1.0f) {
        // Portal AreaTriggers can sit slightly past the navmesh edge; step to
        // the closest reachable point along the path.
        next_pt = end;
        found = true;
      }
    }
  }

  if (!found && walk->arrival.kind == TravelLegKind::kWalk &&
      dist_to_dest <= kWalkNearEnoughArrivalYd) {
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
      .arrival = ParseLeaderTravelPlaceId(action.id),
  };
  return TickLongWalk(bot, now, walk, dungeon_nav);
}

}  // namespace llm_brain
