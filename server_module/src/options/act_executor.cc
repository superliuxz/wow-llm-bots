// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/act_executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "Cell.h"
#include "CharmInfo.h"
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
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
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

std::string_view SpellLocalizedName(const SpellInfo* info) {
  if (!info) {
    return {};
  }
  const char* name = info->SpellName[sWorld->GetDefaultDbcLocale()];
  if (!name || name[0] == '\0') {
    name = info->SpellName[0];
  }
  return name ? std::string_view(name) : std::string_view();
}

bool UnitHasAuraNamed(const Unit* unit, std::string_view name) {
  for (const auto& [spell_id, aura] : unit->GetAppliedAuras()) {
    (void)spell_id;
    if (NameEq(SpellLocalizedName(aura->GetBase()->GetSpellInfo()), name)) {
      return true;
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
      // AzerothCore does not store client focus server-side: fail explicitly
      // rather than silently hitting the current target.
      return nullptr;
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
      Group* grp = bot->GetGroup();
      Unit* best = nullptr;
      float best_dist = std::numeric_limits<float>::infinity();
      for (Unit* u : NearbyUnits(bot)) {
        if (u == bot || !u->IsAlive()) {
          continue;
        }
        if (act.thing == pb::Command_Act::GROUP) {
          Player* other = u->ToPlayer();
          if (!other || !grp || !grp->SameSubGroup(bot, other)) {
            continue;
          }
        } else if (act.thing == pb::Command_Act::ANYONE) {
          if (!u->IsPlayer() || !bot->IsFriendlyTo(u)) {
            continue;
          }
        } else if (!bot->IsFriendlyTo(u)) {
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
        if (NameEq(SpellLocalizedName(sp), thing_name)) {
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

void ClearActMovement(Player* bot) {
  if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
    if (auto* last = ai->GetAiObjectContext()->GetValue<LastMovement&>(
            "last movement")) {
      last->Get().clear();
    }
  }
}

// Picks the first candidate spot with valid ground height, optional LoS to
// `anchor`, and a valid `PathGenerator` path, then starts moving `bot` there.
// Uses `MOVEMENT_FORCED` so playerbots' combat movement does not fight our
// steering while the step runs; cleared on completion/failure.
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
                        MovementPriority::MOVEMENT_FORCED);
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
    if (!info || !NameEq(SpellLocalizedName(info), name)) {
      continue;
    }
    if (!best || info->GetRank() > best->GetRank()) {
      best = info;
    }
  }
  return best;
}

// Whether a cast the client's handler was just given really started: the
// handlers return nothing, and a refused cast (range, mana, line of sight,
// global cooldown) leaves the bot idle. Started means casting, or the spell's
// cooldown or the global cooldown running now. An instant spell with neither
// would read as refused; none of the order verbs' spells is one in practice.
bool CastStarted(Player* bot, const SpellInfo* spell) {
  return bot->HasUnitState(UNIT_STATE_CASTING) ||
         (spell && (bot->HasSpellCooldown(spell->Id) ||
                    bot->GetGlobalCooldownMgr().HasGlobalCooldown(spell)));
}

// Estimates the bot's effective max spell range from its known active spells,
// with a 2 yd margin (falls back to kDefaultMaxRangeYd when no ranged spell is
// known).
float BotMaxSpellRangeYd(Player* bot) {
  float best = 0.0f;
  for (const auto& [spell_id, player_spell] : bot->GetSpellMap()) {
    if (!player_spell || player_spell->State == PLAYERSPELL_REMOVED ||
        !player_spell->Active) {
      continue;
    }
    const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
    if (!info || info->IsPassive()) {
      continue;
    }
    const float r = info->GetMaxRange(/*positive=*/false, bot);
    if (r >= 10.0f && r <= 45.0f && r > best) {
      best = r;
    }
  }
  return best >= 10.0f ? std::max(8.0f, best - 2.0f) : kDefaultMaxRangeYd;
}

// Finds a usable item in `bot`'s bags matching `name` (with bag/slot).
std::optional<BagSlotItem> FindBagItemByName(Player* bot,
                                             std::string_view name) {
  for (const BagSlotItem& entry : BagItems(bot)) {
    const ItemTemplate* proto = entry.item->GetTemplate();
    if (proto && NameEq(proto->Name1, name)) {
      return entry;
    }
  }
  return std::nullopt;
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
          ClearActMovement(bot);
          break;
        case pb::Command_Act::STOP_UNSPECIFIED:
          bot->InterruptNonMeleeSpells(false);
          bot->AttackStop();
          bot->StopMoving();
          bot->GetMotionMaster()->Clear();
          ClearActMovement(bot);
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
        if (bot->HasSpellCooldown(spell->Id)) {
          return {.error = act.name + " is on cooldown"};
        }
        Unit* cast_target = target ? target : bot;
        WorldPacket p(CMSG_CAST_SPELL);
        p << static_cast<uint8_t>(0) << spell->Id << static_cast<uint8_t>(0);
        SpellCastTargets targets;
        targets.SetUnitTarget(cast_target);
        targets.Write(p);
        p.rpos(0);
        bot->GetSession()->HandleCastSpellOpcode(p);
        if (!CastStarted(bot, spell)) {
          return {.error = "cannot cast " + act.name + " now"};
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

      if (std::optional<BagSlotItem> found = FindBagItemByName(bot, act.name)) {
        Item* item = found->item;
        const ItemTemplate* proto = item->GetTemplate();
        uint32_t item_spell_id = 0;
        for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
          if (proto->Spells[i].SpellId != 0 &&
              proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE) {
            item_spell_id = proto->Spells[i].SpellId;
            break;
          }
        }
        if (item_spell_id == 0 || bot->CanUseItem(item) != EQUIP_ERR_OK) {
          return {.error = "cannot use " + act.name};
        }
        if (bot->HasSpellCooldown(item_spell_id)) {
          return {.error = act.name + " is on cooldown"};
        }
        const uint32_t count_before = bot->GetItemCount(proto->ItemId);
        WorldPacket p(CMSG_USE_ITEM);
        p << found->bag << found->slot << static_cast<uint8_t>(0)
          << item_spell_id << item->GetGUID() << static_cast<uint32_t>(0)
          << static_cast<uint8_t>(0);
        SpellCastTargets targets;
        if (target) {
          targets.SetUnitTarget(target);
        }
        targets.Write(p);
        p.rpos(0);
        bot->GetSession()->HandleUseItemOpcode(p);
        if (bot->GetItemCount(proto->ItemId) >= count_before &&
            !CastStarted(bot, sSpellMgr->GetSpellInfo(item_spell_id))) {
          return {.error = "cannot use " + act.name + " now"};
        }
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
      const float max_range = (act.verb == pb::Command_Act::TO &&
                               act.place == pb::Command_Act::MAX_RANGE)
                                  ? BotMaxSpellRangeYd(bot)
                                  : kDefaultMaxRangeYd;
      std::vector<pb::Vec3> candidates =
          (act.verb == pb::Command_Act::TO)
              ? CandidateSpotsForPlace(act.place, bot_pos, *anchor, act.yd,
                                       max_range)
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

    case pb::Command_Act::AVOID: {
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      if (obj && bot->GetTarget() == obj->GetGUID()) {
        bot->AttackStop();
        bot->SetSelection(ObjectGuid::Empty);
      }
      return {.done = true};
    }

    case pb::Command_Act::FACE:
    case pb::Command_Act::FACE_AWAY: {
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      if (!obj) {
        return {.error = "target to face not found"};
      }
      float angle = bot->GetAngle(obj);
      if (act.verb == pb::Command_Act::FACE_AWAY) {
        angle += std::numbers::pi_v<float>;
      }
      bot->SetFacingTo(Position::NormalizeOrientation(angle));
      return {.done = true};
    }

    case pb::Command_Act::JUMP:
      bot->GetMotionMaster()->MoveJump(bot->GetPositionX(), bot->GetPositionY(),
                                       bot->GetPositionZ(), 1.0f, 8.0f);
      return {.done = true};

    case pb::Command_Act::CANCEL: {
      uint32_t cancel_id = 0;
      for (const auto& [spell_id, aura] : bot->GetAppliedAuras()) {
        const SpellInfo* info = aura->GetBase()->GetSpellInfo();
        if (info && NameEq(SpellLocalizedName(info), act.name) &&
            !info->HasAttribute(SPELL_ATTR0_NO_AURA_CANCEL)) {
          cancel_id = spell_id;
          break;
        }
      }
      if (cancel_id == 0) {
        return {.error = "cannot cancel aura: " + act.name};
      }
      bot->RemoveOwnedAura(cancel_id, ObjectGuid::Empty, 0,
                           AURA_REMOVE_BY_CANCEL);
      return {.done = true};
    }

    case pb::Command_Act::MARK: {
      Group* grp = bot->GetGroup();
      if (!grp) {
        return {.error = "not in a group"};
      }
      if (grp->isRaidGroup() && !grp->IsLeader(bot->GetGUID()) &&
          !grp->IsAssistant(bot->GetGUID())) {
        return {.error = "must be raid leader or assistant to mark"};
      }
      const uint8_t icon = RaidMarkerIconIndex(act.marker);
      if (icon >= TARGETICONCOUNT) {
        return {.error = "invalid raid marker"};
      }
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      Unit* unit = obj ? obj->ToUnit() : nullptr;
      if (!unit) {
        return {.error = "unit to mark not found"};
      }
      grp->SetTargetIcon(icon, bot->GetGUID(), unit->GetGUID());
      return {.done = true};
    }

    case pb::Command_Act::INTERACT: {
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      if (!obj) {
        return {.error = "object or npc to interact with not found"};
      }
      if (GameObject* go = obj->ToGameObject()) {
        if (IsInUseRange(bot, go)) {
          std::string err = UseObjectNow(bot, go);
          return err.empty() ? ActStartResult{.done = true}
                             : ActStartResult{.error = std::move(err)};
        }
      } else if (Creature* npc = obj->ToCreature()) {
        if (IsInTalkRange(bot, npc)) {
          TalkResult tr = TalkToNpcNow(bot, npc);
          return tr.error.empty()
                     ? ActStartResult{.done = true}
                     : ActStartResult{.error = std::move(tr.error)};
        }
      } else {
        return {.error = "cannot interact with target"};
      }
      const AnchorGeometry anchor = AnchorOf(obj);
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, {anchor.pos}, nullptr);
      if (!chosen) {
        return {.error = "no path to interact target"};
      }
      out_active->command_id = command_id;
      out_active->act = act;
      out_active->dest = *chosen;
      out_active->last_anchor = anchor;
      out_active->last_repath_at = now;
      return {.done = false};
    }

    case pb::Command_Act::BOARD: {
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      Unit* unit = obj ? obj->ToUnit() : nullptr;
      if (!unit) {
        return {.error = "vehicle to board not found"};
      }
      if (bot->IsWithinDistInMap(unit, INTERACTION_DISTANCE)) {
        WorldPacket p(CMSG_SPELLCLICK, 8);
        p << unit->GetGUID();
        p.rpos(0);
        bot->GetSession()->HandleSpellClick(p);
        return {.done = true};
      }
      const AnchorGeometry anchor = AnchorOf(unit);
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, {anchor.pos}, nullptr);
      if (!chosen) {
        return {.error = "no path to vehicle"};
      }
      out_active->command_id = command_id;
      out_active->act = act;
      out_active->dest = *chosen;
      out_active->last_anchor = anchor;
      out_active->last_repath_at = now;
      return {.done = false};
    }

    case pb::Command_Act::LEAVE: {
      if (bot->GetVehicle()) {
        WorldPacket p(CMSG_REQUEST_VEHICLE_EXIT, 0);
        bot->GetSession()->HandleRequestVehicleExit(p);
        return {.done = true};
      }
      if (bot->IsMounted()) {
        bot->Dismount();
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        return {.done = true};
      }
      return {.error = "not in a vehicle or mounted"};
    }

    case pb::Command_Act::EQUIP: {
      for (const BagSlotItem& entry : BagItems(bot)) {
        const ItemTemplate* proto = entry.item->GetTemplate();
        if (!proto || !NameEq(proto->Name1, act.name)) {
          continue;
        }
        uint16_t dest = 0;
        if (bot->CanEquipItem(NULL_SLOT, dest, entry.item, true) !=
            EQUIP_ERR_OK) {
          return {.error = "cannot equip " + act.name};
        }
        const uint16_t src = static_cast<uint16_t>(
            (static_cast<uint16_t>(entry.bag) << 8) | entry.slot);
        bot->SwapItem(src, dest);
        return {.done = true};
      }
      return {.error = "item not in bags: " + act.name};
    }

    case pb::Command_Act::PET_ATTACK: {
      Pet* pet = bot->GetPet();
      if (!pet || !pet->IsAlive()) {
        return {.error = "no active pet"};
      }
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      Unit* unit = obj ? obj->ToUnit() : nullptr;
      if (!unit) {
        return {.error = "pet target not found"};
      }
      bot->GetSession()->HandlePetActionHelper(
          pet, pet->GetGUID(), COMMAND_ATTACK, ACT_COMMAND, unit->GetGUID());
      return {.done = true};
    }

    case pb::Command_Act::PET: {
      Pet* pet = bot->GetPet();
      if (!pet || !pet->IsAlive()) {
        return {.error = "no active pet"};
      }
      uint32_t spell_id = 0;
      uint16_t flag = 0;
      switch (act.pet_cmd) {
        case pb::Command_Act::STAY:
          spell_id = COMMAND_STAY;
          flag = ACT_COMMAND;
          break;
        case pb::Command_Act::FOLLOW:
          spell_id = COMMAND_FOLLOW;
          flag = ACT_COMMAND;
          break;
        case pb::Command_Act::PASSIVE:
          spell_id = REACT_PASSIVE;
          flag = ACT_REACTION;
          break;
        case pb::Command_Act::DEFENSIVE:
          spell_id = REACT_DEFENSIVE;
          flag = ACT_REACTION;
          break;
        case pb::Command_Act::PET_CMD_UNSPECIFIED:
          return {.error = "unspecified pet command"};
      }
      bot->GetSession()->HandlePetActionHelper(pet, pet->GetGUID(), spell_id,
                                               flag, ObjectGuid::Empty);
      return {.done = true};
    }

    case pb::Command_Act::KITE: {
      std::optional<AnchorGeometry> anchor = ResolveMoveAnchor(bot, act);
      if (!anchor) {
        return {.error = "kite target not found"};
      }
      std::vector<pb::Vec3> candidates =
          CandidateSpotsForKite(PosOf(bot), *anchor);
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, candidates, nullptr);
      if (!chosen) {
        return {.error = "no path to kite"};
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
    ClearActMovement(bot);
    return {.error = "you died"};
  }
  if (active->waiting_cast) {
    if (!bot->HasUnitState(UNIT_STATE_CASTING)) {
      return {.done = true};
    }
    return {};
  }

  const pb::Command_Act& act = active->act;
  if (act.verb == pb::Command_Act::INTERACT ||
      act.verb == pb::Command_Act::BOARD) {
    const std::string_view thing_name =
        !act.target_name.empty() ? act.target_name : act.name;
    WorldObject* obj = ResolveThing(bot, act, thing_name);
    if (!obj) {
      ClearActMovement(bot);
      return {.error = "target is gone"};
    }
    if (act.verb == pb::Command_Act::INTERACT) {
      if (GameObject* go = obj->ToGameObject(); go && IsInUseRange(bot, go)) {
        ClearActMovement(bot);
        std::string err = UseObjectNow(bot, go);
        return err.empty() ? ActStepResult{.done = true}
                           : ActStepResult{.error = std::move(err)};
      }
      if (Creature* npc = obj->ToCreature(); npc && IsInTalkRange(bot, npc)) {
        ClearActMovement(bot);
        TalkResult tr = TalkToNpcNow(bot, npc);
        return tr.error.empty() ? ActStepResult{.done = true}
                                : ActStepResult{.error = std::move(tr.error)};
      }
    } else if (Unit* unit = obj->ToUnit();
               unit && bot->IsWithinDistInMap(unit, INTERACTION_DISTANCE)) {
      ClearActMovement(bot);
      WorldPacket p(CMSG_SPELLCLICK, 8);
      p << unit->GetGUID();
      p.rpos(0);
      bot->GetSession()->HandleSpellClick(p);
      return {.done = true};
    }
    if (now - active->last_repath_at >= kRepathInterval && !bot->isMoving()) {
      active->last_repath_at = now;
      const AnchorGeometry anchor = AnchorOf(obj);
      if (std::optional<pb::Vec3> chosen =
              MoveToFirstValidSpot(bot, {anchor.pos}, nullptr)) {
        active->dest = *chosen;
        active->last_anchor = anchor;
      } else {
        ClearActMovement(bot);
        return {.error = "no path to target"};
      }
    }
    return {};
  }

  if (act.verb == pb::Command_Act::KITE) {
    if (now - active->last_repath_at < kRepathInterval) {
      return {};
    }
    active->last_repath_at = now;
    std::optional<AnchorGeometry> cur_anchor = ResolveMoveAnchor(bot, act);
    if (!cur_anchor) {
      ClearActMovement(bot);
      return {.done = true};
    }
    const pb::Vec3 bot_pos = PosOf(bot);
    if (!bot->isMoving() ||
        Dist2D(bot_pos, active->dest) <= kArrivalToleranceYd + 2.0f ||
        Dist2D(bot_pos, cur_anchor->pos) < kDefaultKiteYd * 0.6f) {
      std::vector<pb::Vec3> candidates =
          CandidateSpotsForKite(bot_pos, *cur_anchor);
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, candidates, nullptr);
      if (!chosen) {
        ClearActMovement(bot);
        return {.error = "no path to kite"};
      }
      active->dest = *chosen;
      active->last_anchor = *cur_anchor;
    }
    return {};
  }

  if (act.verb != pb::Command_Act::TO && act.verb != pb::Command_Act::AWAY) {
    return {.done = true};
  }

  const pb::Vec3 bot_pos = PosOf(bot);
  if (act.verb == pb::Command_Act::AWAY) {
    const float target_yd = act.yd > 0.0f ? act.yd : kDefaultAwayYd;
    if (Dist2D(bot_pos, active->last_anchor.pos) >= target_yd ||
        Dist2D(bot_pos, active->dest) <= kArrivalToleranceYd) {
      bot->StopMoving();
      ClearActMovement(bot);
      return {.done = true};
    }
    // Keep the anchor fixed from StartAct so bare `away` (anchored on the
    // starting spot) and `away <moving mob>` finish cleanly instead of
    // chasing a shifting anchor forever.
    if (now - active->last_repath_at >= kRepathInterval && !bot->isMoving()) {
      active->last_repath_at = now;
      std::vector<pb::Vec3> candidates =
          CandidateSpotsForAway(bot_pos, active->last_anchor, act.yd);
      if (candidates.empty()) {
        ClearActMovement(bot);
        return {.done = true};
      }
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, candidates, nullptr);
      if (!chosen) {
        ClearActMovement(bot);
        return {.error = "no path to destination"};
      }
      active->dest = *chosen;
    }
    return {};
  }

  if (Dist2D(bot_pos, active->dest) <= kArrivalToleranceYd) {
    ClearActMovement(bot);
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
    ClearActMovement(bot);
    return {.error = "destination is gone"};
  }

  const bool cares_facing = PlaceCaresAboutFacing(act.place);
  if (!bot->isMoving() || AnchorMovedSignificantly(active->last_anchor,
                                                   *cur_anchor, cares_facing)) {
    const float max_range = (act.place == pb::Command_Act::MAX_RANGE)
                                ? BotMaxSpellRangeYd(bot)
                                : kDefaultMaxRangeYd;
    std::vector<pb::Vec3> candidates = CandidateSpotsForPlace(
        act.place, bot_pos, *cur_anchor, act.yd, max_range);
    if (candidates.empty()) {
      ClearActMovement(bot);
      return {.done = true};
    }
    const AnchorGeometry* los_anchor =
        PlaceNeedsLos(act.place) ? &*cur_anchor : nullptr;
    std::optional<pb::Vec3> chosen =
        MoveToFirstValidSpot(bot, candidates, los_anchor);
    if (!chosen) {
      ClearActMovement(bot);
      return {.error = "no path to destination"};
    }
    active->dest = *chosen;
    active->last_anchor = *cur_anchor;
  }
  return {};
}

}  // namespace llm_brain
