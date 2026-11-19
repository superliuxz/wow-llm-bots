// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/act_executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "AiObjectContext.h"
#include "Cell.h"
#include "Creature.h"
#include "DynamicObject.h"
#include "G3D/Vector3.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "IVMapMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "LastMovementValue.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "options/act_geometry.h"
#include "options/option_executor.h"
#include "snapshot/snapshot.h"
#include "snapshot/world_util.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr float kActSearchRadiusYd = 80.0f;
constexpr float kPathEndSlackYd = 3.0f;
constexpr float kArrivalToleranceYd = 2.5f;
constexpr base::TimeDelta kRepathInterval = base::Milliseconds(250);

bool NameEq(std::string_view a, std::string_view b) {
  return !a.empty() && !b.empty() && base::EqualsCaseInsensitiveASCII(a, b);
}

pb::Vec3 PosOf(const WorldObject* obj) {
  return {obj->GetPositionX(), obj->GetPositionY(), obj->GetPositionZ()};
}

AnchorGeometry AnchorOf(const WorldObject* obj) {
  AnchorGeometry g;
  g.pos = PosOf(obj);
  g.facing_rad = obj->GetOrientation();
  if (const Unit* u = obj->ToUnit()) {
    g.radius_yd = u->GetCombatReach();
  }
  return g;
}

uint8_t RaidMarkerIconIndex(pb::RaidMarker m) {
  switch (m) {
    case pb::RaidMarker::STAR:
      return 0;
    case pb::RaidMarker::CIRCLE:
      return 1;
    case pb::RaidMarker::DIAMOND:
      return 2;
    case pb::RaidMarker::TRIANGLE:
      return 3;
    case pb::RaidMarker::MOON:
      return 4;
    case pb::RaidMarker::SQUARE:
      return 5;
    case pb::RaidMarker::CROSS:
      return 6;
    case pb::RaidMarker::SKULL:
      return 7;
    case pb::RaidMarker::RAID_MARKER_UNSPECIFIED:
      return 255;
  }
  return 255;
}

bool UnitHasAuraNamed(const Unit* unit, std::string_view name) {
  for (const auto& [spell_id, aura] : unit->GetAppliedAuras()) {
    (void)spell_id;
    if (const SpellInfo* info = aura->GetBase()->GetSpellInfo()) {
      if (NameEq(info->SpellName[0], name)) {
        return true;
      }
    }
  }
  return false;
}

// Collects nearby living units around `bot`.
std::vector<Unit*> NearbyUnits(Player* bot, float radius = kActSearchRadiusYd) {
  std::vector<Unit*> units;
  Acore::AnyUnitInObjectRangeCheck check(bot, radius);
  Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units,
                                                                     check);
  Cell::VisitObjects(bot, searcher, radius);
  return units;
}

// Collects nearby game objects around `bot`.
std::vector<GameObject*> NearbyObjects(Player* bot,
                                       float radius = kActSearchRadiusYd) {
  std::vector<GameObject*> objs;
  Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(),
                                      bot->GetPositionZ(), radius);
  Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(
      bot, objs, check);
  Cell::VisitObjects(bot, searcher, radius);
  return objs;
}

// Collects nearby dynamic objects (ground spell areas like Defile).
std::vector<DynamicObject*> NearbyAreas(Player* bot,
                                        float radius = kActSearchRadiusYd) {
  std::vector<DynamicObject*> dynos;
  Acore::AllWorldObjectsInRange check(bot, radius);
  std::list<WorldObject*> all;
  Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(
      bot, all, check);
  Cell::VisitObjects(bot, searcher, radius);
  for (WorldObject* obj : all) {
    if (DynamicObject* d = obj->ToDynObject()) {
      dynos.push_back(d);
    }
  }
  return dynos;
}

// Finds group/raid tank players (main tanks first, then TANK role).
std::vector<Player*> GroupTanks(Player* bot) {
  std::vector<Player*> tanks;
  Group* grp = bot->GetGroup();
  if (!grp) {
    return tanks;
  }
  auto add_if_new = [&](Player* p) {
    if (p && p->IsInWorld() && p->GetMapId() == bot->GetMapId() &&
        std::find(tanks.begin(), tanks.end(), p) == tanks.end()) {
      tanks.push_back(p);
    }
  };
  for (const Group::MemberSlot& slot : grp->GetMemberSlots()) {
    if ((slot.flags & MEMBER_FLAG_MAINTANK) != 0) {
      add_if_new(ObjectAccessor::FindPlayer(slot.guid));
    }
  }
  for (const Group::MemberSlot& slot : grp->GetMemberSlots()) {
    if ((slot.roles & lfg::PLAYER_ROLE_TANK) != 0) {
      add_if_new(ObjectAccessor::FindPlayer(slot.guid));
    }
  }
  return tanks;
}

// Resolves a `Command_Act::ThingKind` to a live `WorldObject*` (unit, object,
// or dynamic area) in the world.
WorldObject* ResolveThing(Player* bot,
                          const pb::Command_Act& act,
                          std::string_view thing_name) {
  switch (act.thing) {
    case pb::Command_Act::THING_UNSPECIFIED:
      return nullptr;
    case pb::Command_Act::ME:
      return bot;
    case pb::Command_Act::IT: {
      if (!act.it_guid.empty()) {
        ObjectGuid guid = ParseSnapshotGuid(act.it_guid);
        if (!guid.IsEmpty()) {
          if (WorldObject* obj = ObjectAccessor::GetWorldObject(*bot, guid)) {
            return obj;
          }
        }
      }
      if (!act.it_name.empty()) {
        pb::Command_Act by_name = act;
        by_name.thing = pb::Command_Act::NEAREST_NAMED;
        return ResolveThing(bot, by_name, act.it_name);
      }
      return nullptr;
    }
    case pb::Command_Act::MY_TARGET:
      return bot->GetSelectedUnit();
    case pb::Command_Act::FOCUS:
      // AzerothCore servers do not store client focus; fall back to target.
      return bot->GetSelectedUnit();
    case pb::Command_Act::TANK: {
      std::vector<Player*> tanks = GroupTanks(bot);
      return tanks.empty() ? nullptr : tanks.front();
    }
    case pb::Command_Act::OFFTANK: {
      std::vector<Player*> tanks = GroupTanks(bot);
      return tanks.size() >= 2 ? tanks[1] : nullptr;
    }
    case pb::Command_Act::HEALER: {
      Group* grp = bot->GetGroup();
      if (!grp) {
        return nullptr;
      }
      for (const Group::MemberSlot& slot : grp->GetMemberSlots()) {
        if ((slot.roles & lfg::PLAYER_ROLE_HEALER) != 0) {
          if (Player* p = ObjectAccessor::FindPlayer(slot.guid);
              p && p->IsInWorld() && p->GetMapId() == bot->GetMapId()) {
            return p;
          }
        }
      }
      return nullptr;
    }
    case pb::Command_Act::LEADER: {
      Group* grp = bot->GetGroup();
      if (!grp) {
        return nullptr;
      }
      Player* leader = ObjectAccessor::FindPlayer(grp->GetLeaderGUID());
      return (leader && leader->IsInWorld() &&
              leader->GetMapId() == bot->GetMapId())
                 ? leader
                 : nullptr;
    }
    case pb::Command_Act::BOSS: {
      Unit* best = nullptr;
      float best_dist = std::numeric_limits<float>::infinity();
      for (Unit* u : NearbyUnits(bot)) {
        if (!u->IsAlive()) {
          continue;
        }
        const Creature* c = u->ToCreature();
        if (!c || (!c->isWorldBoss() && !c->IsDungeonBoss())) {
          continue;
        }
        const float d = bot->GetDistance(u);
        if (d < best_dist) {
          best = u;
          best_dist = d;
        }
      }
      return best;
    }
    case pb::Command_Act::MARKER: {
      Group* grp = bot->GetGroup();
      const uint8_t icon = RaidMarkerIconIndex(act.marker);
      if (!grp || icon >= TARGETICONCOUNT) {
        return nullptr;
      }
      ObjectGuid guid = grp->GetTargetIcon(icon);
      return guid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, guid);
    }
    case pb::Command_Act::NEAREST_ALLY:
    case pb::Command_Act::GROUP:
    case pb::Command_Act::ALLIES:
    case pb::Command_Act::ANYONE: {
      Unit* best = nullptr;
      float best_dist = std::numeric_limits<float>::infinity();
      for (Unit* u : NearbyUnits(bot)) {
        if (u == bot || !u->IsAlive()) {
          continue;
        }
        if (act.thing == pb::Command_Act::GROUP &&
            !bot->IsInSameGroupWith(u->ToPlayer())) {
          continue;
        }
        if ((act.thing == pb::Command_Act::NEAREST_ALLY ||
             act.thing == pb::Command_Act::ALLIES) &&
            !bot->IsFriendlyTo(u)) {
          continue;
        }
        const float d = bot->GetDistance(u);
        if (d < best_dist) {
          best = u;
          best_dist = d;
        }
      }
      return best;
    }
    case pb::Command_Act::NEAREST_PLAYER: {
      Unit* best = nullptr;
      float best_dist = std::numeric_limits<float>::infinity();
      for (Unit* u : NearbyUnits(bot)) {
        if (u == bot || !u->IsAlive() || !u->IsPlayer()) {
          continue;
        }
        const float d = bot->GetDistance(u);
        if (d < best_dist) {
          best = u;
          best_dist = d;
        }
      }
      return best;
    }
    case pb::Command_Act::NEAREST_ENEMY: {
      Unit* best = nullptr;
      float best_dist = std::numeric_limits<float>::infinity();
      for (Unit* u : NearbyUnits(bot)) {
        if (u == bot || !u->IsAlive() || !bot->IsHostileTo(u)) {
          continue;
        }
        const float d = bot->GetDistance(u);
        if (d < best_dist) {
          best = u;
          best_dist = d;
        }
      }
      return best;
    }
    case pb::Command_Act::ALLY_WITH: {
      Unit* best = nullptr;
      float best_dist = std::numeric_limits<float>::infinity();
      for (Unit* u : NearbyUnits(bot)) {
        if (!u->IsAlive() || !bot->IsFriendlyTo(u) ||
            !UnitHasAuraNamed(u, thing_name)) {
          continue;
        }
        const float d = bot->GetDistance(u);
        if (d < best_dist) {
          best = u;
          best_dist = d;
        }
      }
      return best;
    }
    case pb::Command_Act::NAMED:
    case pb::Command_Act::NEAREST_NAMED: {
      WorldObject* best = nullptr;
      float best_dist = std::numeric_limits<float>::infinity();
      for (Unit* u : NearbyUnits(bot)) {
        if (!u->IsAlive() || !NameEq(u->GetName(), thing_name)) {
          continue;
        }
        const float d = bot->GetDistance(u);
        if (d < best_dist) {
          best = u;
          best_dist = d;
        }
      }
      for (GameObject* go : NearbyObjects(bot)) {
        if (!go->isSpawned() || !NameEq(go->GetName(), thing_name)) {
          continue;
        }
        const float d = bot->GetDistance(go);
        if (d < best_dist) {
          best = go;
          best_dist = d;
        }
      }
      for (DynamicObject* dyn : NearbyAreas(bot)) {
        const SpellInfo* sp = sSpellMgr->GetSpellInfo(dyn->GetSpellId());
        if (sp && NameEq(sp->SpellName[0], thing_name)) {
          const float d = bot->GetDistance(dyn);
          if (d < best_dist) {
            best = dyn;
            best_dist = d;
          }
        }
      }
      return best;
    }
  }
  return nullptr;
}

// Picks the first candidate spot with valid ground height, optional LoS to
// `anchor`, and a valid `PathGenerator` path, then starts moving `bot` there.
std::optional<pb::Vec3> MoveToFirstValidSpot(
    Player* bot,
    const std::vector<pb::Vec3>& candidates,
    const AnchorGeometry* los_anchor) {
  Map* map = bot->GetMap();
  if (!map) {
    return std::nullopt;
  }
  for (const pb::Vec3& cand : candidates) {
    const float ground_z =
        map->GetHeight(bot->GetPhaseMask(), cand.x, cand.y, cand.z + 2.0f);
    if (ground_z <= INVALID_HEIGHT || ground_z == VMAP_INVALID_HEIGHT_VALUE) {
      continue;
    }
    if (los_anchor &&
        !map->isInLineOfSight(
            cand.x, cand.y, ground_z + 1.5f, los_anchor->pos.x,
            los_anchor->pos.y, los_anchor->pos.z + 1.5f, bot->GetPhaseMask(),
            LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing)) {
      continue;
    }
    PathGenerator path(bot);
    path.CalculatePath(cand.x, cand.y, ground_z);
    const uint32_t type = path.GetPathType();
    const G3D::Vector3& end = path.GetActualEndPosition();
    if ((type & PATHFIND_NOT_USING_PATH) == 0 &&
        ((type & PATHFIND_NOPATH) != 0 ||
         std::hypot(end.x - cand.x, end.y - cand.y) > kPathEndSlackYd)) {
      continue;
    }

    bot->GetMotionMaster()->MovePoint(
        /*id=*/0, cand.x, cand.y, ground_z, FORCED_MOVEMENT_NONE,
        /*speed=*/0.0f, /*orientation=*/0.0f, /*generatePath=*/true,
        /*forceDestination=*/false);
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
      if (auto* last = ai->GetAiObjectContext()->GetValue<LastMovement&>(
              "last movement")) {
        const float speed = bot->GetSpeed(MOVE_RUN);
        const float ms =
            speed > 0.0f
                ? bot->GetExactDist(cand.x, cand.y, ground_z) / speed * 1000.0f
                : 1000.0f;
        last->Get().Set(bot->GetMapId(), cand.x, cand.y, ground_z,
                        bot->GetOrientation(), ms + 500.0f,
                        MovementPriority::MOVEMENT_COMBAT);
      }
    }
    return pb::Vec3{cand.x, cand.y, ground_z};
  }
  return std::nullopt;
}

// Finds the highest-rank spell known by `bot` matching `name`.
const SpellInfo* FindKnownSpell(Player* bot, std::string_view name) {
  const SpellInfo* best = nullptr;
  for (const auto& [spell_id, player_spell] : bot->GetSpellMap()) {
    if (!player_spell || player_spell->State == PLAYERSPELL_REMOVED ||
        !player_spell->Active) {
      continue;
    }
    const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
    if (!info || !NameEq(info->SpellName[0], name)) {
      continue;
    }
    if (!best || info->GetRank() > best->GetRank()) {
      best = info;
    }
  }
  return best;
}

// Finds a usable item in `bot`'s bags matching `name`.
Item* FindBagItemByName(Player* bot, std::string_view name) {
  for (const BagSlotItem& entry : BagItems(bot)) {
    const ItemTemplate* proto = entry.item->GetTemplate();
    if (proto && NameEq(proto->Name1, name)) {
      return entry.item;
    }
  }
  return nullptr;
}

// Resolves the anchor for `to` or `away`. For bare `away` (no `thing`), uses
// the nearest hostile dynamic area if standing near one, otherwise `bot`'s
// current spot.
std::optional<AnchorGeometry> ResolveMoveAnchor(Player* bot,
                                                const pb::Command_Act& act) {
  if (act.thing != pb::Command_Act::THING_UNSPECIFIED) {
    const std::string_view thing_name =
        !act.target_name.empty() ? act.target_name : act.name;
    WorldObject* obj = ResolveThing(bot, act, thing_name);
    if (!obj) {
      return std::nullopt;
    }
    return AnchorOf(obj);
  }
  // Bare `away`: prefer nearest dynamic ground effect (e.g. Defile).
  DynamicObject* nearest_area = nullptr;
  float best_dist = 15.0f;
  for (DynamicObject* dyn : NearbyAreas(bot, 20.0f)) {
    const float d = bot->GetDistance(dyn);
    if (d < best_dist) {
      nearest_area = dyn;
      best_dist = d;
    }
  }
  if (nearest_area) {
    return AnchorOf(nearest_area);
  }
  return AnchorOf(bot);
}

bool PlaceNeedsLos(pb::Command_Act::PlaceKind place) {
  return place == pb::Command_Act::BEHIND ||
         place == pb::Command_Act::IN_FRONT ||
         place == pb::Command_Act::MAX_RANGE ||
         place == pb::Command_Act::YD_FROM;
}

bool PlaceCaresAboutFacing(pb::Command_Act::PlaceKind place) {
  return place == pb::Command_Act::BEHIND || place == pb::Command_Act::IN_FRONT;
}

}  // namespace

ActStartResult StartAct(Player* bot,
                        uint32_t command_id,
                        const pb::Command_Act& act,
                        base::TimeTicks now,
                        ActiveAct* out_active) {
  if (!bot || !bot->IsInWorld()) {
    return {.error = "bot not in world"};
  }
  if (act.verb != pb::Command_Act::WAKE && !bot->IsAlive()) {
    return {.error = "you are dead"};
  }

  switch (act.verb) {
    case pb::Command_Act::VERB_UNSPECIFIED:
      return {.error = "unspecified act"};

    case pb::Command_Act::WAKE:
      return {.done = true};

    case pb::Command_Act::STOP:
      switch (act.stop) {
        case pb::Command_Act::CASTING:
          bot->InterruptNonMeleeSpells(false);
          break;
        case pb::Command_Act::ATTACKING:
          bot->AttackStop();
          break;
        case pb::Command_Act::MOVING:
          bot->StopMoving();
          bot->GetMotionMaster()->Clear();
          break;
        case pb::Command_Act::STOP_UNSPECIFIED:
          bot->InterruptNonMeleeSpells(false);
          bot->AttackStop();
          bot->StopMoving();
          bot->GetMotionMaster()->Clear();
          break;
      }
      return {.done = true};

    case pb::Command_Act::TARGET: {
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      Unit* unit = obj ? obj->ToUnit() : nullptr;
      if (!unit) {
        return {.error = "target not found"};
      }
      // Same call as WorldSession::HandleSetSelectionOpcode.
      bot->SetSelection(unit->GetGUID());
      return {.done = true};
    }

    case pb::Command_Act::USE: {
      Unit* target = nullptr;
      if (act.thing != pb::Command_Act::THING_UNSPECIFIED) {
        const std::string_view thing_name =
            !act.target_name.empty() ? act.target_name : act.name;
        WorldObject* obj = ResolveThing(bot, act, thing_name);
        target = obj ? obj->ToUnit() : nullptr;
        if (!target) {
          return {.error = "spell target not found"};
        }
      } else {
        target = bot->GetSelectedUnit();
      }

      if (const SpellInfo* spell = FindKnownSpell(bot, act.name)) {
        Unit* cast_target = target ? target : bot;
        const SpellCastResult res =
            bot->CastSpell(cast_target, spell, TRIGGERED_NONE);
        if (res != SPELL_CAST_OK) {
          return {.error = "cannot cast " + act.name};
        }
        if (bot->HasUnitState(UNIT_STATE_CASTING)) {
          out_active->command_id = command_id;
          out_active->act = act;
          out_active->waiting_cast = true;
          out_active->cast_spell_id = spell->Id;
          return {.done = false};
        }
        return {.done = true};
      }

      if (Item* item = FindBagItemByName(bot, act.name)) {
        if (bot->CanUseItem(item) != EQUIP_ERR_OK) {
          return {.error = "cannot use " + act.name};
        }
        SpellCastTargets targets;
        if (target) {
          targets.SetUnitTarget(target);
        }
        bot->CastItemUseSpell(item, targets, /*cast_count=*/0,
                              /*glyphIndex=*/0);
        if (bot->HasUnitState(UNIT_STATE_CASTING)) {
          out_active->command_id = command_id;
          out_active->act = act;
          out_active->waiting_cast = true;
          return {.done = false};
        }
        return {.done = true};
      }

      return {.error = "unknown spell or item: " + act.name};
    }

    case pb::Command_Act::TO:
    case pb::Command_Act::AWAY: {
      std::optional<AnchorGeometry> anchor = ResolveMoveAnchor(bot, act);
      if (!anchor) {
        return {.error = "destination not found"};
      }
      const pb::Vec3 bot_pos = PosOf(bot);
      std::vector<pb::Vec3> candidates =
          (act.verb == pb::Command_Act::TO)
              ? CandidateSpotsForPlace(act.place, bot_pos, *anchor, act.yd)
              : CandidateSpotsForAway(bot_pos, *anchor, act.yd);
      if (candidates.empty()) {
        // Already far enough away.
        return {.done = true};
      }
      // Already at the ideal destination.
      if (Dist2D(bot_pos, candidates.front()) <= kArrivalToleranceYd) {
        return {.done = true};
      }
      const AnchorGeometry* los_anchor =
          (act.verb == pb::Command_Act::TO && PlaceNeedsLos(act.place))
              ? &*anchor
              : nullptr;
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, candidates, los_anchor);
      if (!chosen) {
        return {.error = "no path to destination"};
      }
      out_active->command_id = command_id;
      out_active->act = act;
      out_active->dest = *chosen;
      out_active->last_anchor = *anchor;
      out_active->last_repath_at = now;
      out_active->waiting_cast = false;
      return {.done = false};
    }
  }
  return {.error = "unsupported act"};
}

ActStepResult StepActiveAct(Player* bot,
                            base::TimeTicks now,
                            ActiveAct* active) {
  if (!bot || !bot->IsInWorld() || !bot->IsAlive()) {
    return {.error = "you died"};
  }
  if (active->waiting_cast) {
    if (!bot->HasUnitState(UNIT_STATE_CASTING)) {
      return {.done = true};
    }
    return {};
  }

  const pb::Command_Act& act = active->act;
  if (act.verb != pb::Command_Act::TO && act.verb != pb::Command_Act::AWAY) {
    return {.done = true};
  }

  const pb::Vec3 bot_pos = PosOf(bot);
  if (act.verb == pb::Command_Act::AWAY && act.yd > 0.0f &&
      Dist2D(bot_pos, active->last_anchor.pos) >= act.yd) {
    bot->StopMoving();
    return {.done = true};
  }
  if (Dist2D(bot_pos, active->dest) <= kArrivalToleranceYd) {
    return {.done = true};
  }

  // Recompute relative spot if the anchor moved/turned (e.g. boss rotated)
  // or if the bot stopped moving before arriving.
  if (now - active->last_repath_at < kRepathInterval) {
    return {};
  }
  active->last_repath_at = now;

  std::optional<AnchorGeometry> cur_anchor = ResolveMoveAnchor(bot, act);
  if (!cur_anchor) {
    // Anchor vanished while moving; if we were moving away, we're safe!
    if (act.verb == pb::Command_Act::AWAY) {
      return {.done = true};
    }
    return {.error = "destination is gone"};
  }

  const bool cares_facing =
      act.verb == pb::Command_Act::TO && PlaceCaresAboutFacing(act.place);
  if (!bot->isMoving() || AnchorMovedSignificantly(active->last_anchor,
                                                   *cur_anchor, cares_facing)) {
    std::vector<pb::Vec3> candidates =
        (act.verb == pb::Command_Act::TO)
            ? CandidateSpotsForPlace(act.place, bot_pos, *cur_anchor, act.yd)
            : CandidateSpotsForAway(bot_pos, *cur_anchor, act.yd);
    if (candidates.empty()) {
      return {.done = true};
    }
    const AnchorGeometry* los_anchor =
        (act.verb == pb::Command_Act::TO && PlaceNeedsLos(act.place))
            ? &*cur_anchor
            : nullptr;
    std::optional<pb::Vec3> chosen =
        MoveToFirstValidSpot(bot, candidates, los_anchor);
    if (!chosen) {
      return {.error = "no path to destination"};
    }
    active->dest = *chosen;
    active->last_anchor = *cur_anchor;
  }
  return {};
}

}  // namespace llm_brain
