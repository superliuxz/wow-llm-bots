// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/act_executor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
#include "GroupReference.h"
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
#include "PetDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Position.h"
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
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "bot_manager/spell_miss_packet.h"
#include "bot_session.h"
#include "options/act_geometry.h"
#include "options/exec_npc.h"
#include "options/exec_shared.h"
#include "options/exec_social.h"
#include "snapshot/perception.h"
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
    if (NameEq(SpellLocalizedName(aura->GetBase()->GetSpellInfo()), name)) {
      return true;
    }
  }
  return false;
}

// Whether `obj` is visible to `bot` under the same rules as the snapshot
// (stealth/invisibility, triggers, and invisible display models are hidden).
bool IsVisibleToBot(Player* bot, WorldObject* obj) {
  if (!obj) {
    return false;
  }
  if (obj == bot) {
    return true;
  }
  if (!bot->CanSeeOrDetect(obj)) {
    return false;
  }
  if (const Creature* c = obj->ToCreature()) {
    if (HiddenFromPlayers(c->IsTrigger(), c->GetDisplayId())) {
      return false;
    }
  }
  return true;
}

// Collects nearby living units visible to `bot`.
std::vector<Unit*> NearbyUnits(Player* bot, float radius = kActSearchRadiusYd) {
  std::vector<Unit*> units;
  Acore::AnyUnitInObjectRangeCheck check(bot, radius);
  Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units,
                                                                     check);
  Cell::VisitObjects(bot, searcher, radius);
  std::erase_if(units, [&](Unit* u) { return !IsVisibleToBot(bot, u); });
  return units;
}

// A dead party member named `name` within `radius` (a body to resurrect):
// NearbyUnits() sees living units only.
Player* DeadGroupMemberNamed(Player* bot,
                             std::string_view name,
                             float radius = kActSearchRadiusYd) {
  Group* group = bot->GetGroup();
  if (!group) {
    return nullptr;
  }
  for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next()) {
    Player* member = ref->GetSource();
    if (member && member != bot && !member->IsAlive() &&
        NameEq(member->GetName(), name) && member->IsInMap(bot) &&
        bot->IsWithinDistInMap(member, radius) && IsVisibleToBot(bot, member)) {
      return member;
    }
  }
  return nullptr;
}

// Collects nearby living hidden trigger creatures (carriers of visible ground
// or orb spell hazards such as Flame Strike, Arcane Sphere, Coldflame, Defile).
std::vector<Creature*> NearbyHiddenTriggers(Player* bot,
                                            float radius = kActSearchRadiusYd) {
  std::vector<Unit*> units;
  Acore::AnyUnitInObjectRangeCheck check(bot, radius);
  Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units,
                                                                     check);
  Cell::VisitObjects(bot, searcher, radius);
  std::vector<Creature*> triggers;
  for (Unit* u : units) {
    if (Creature* c = u->ToCreature();
        c && c->IsAlive() && bot->CanSeeOrDetect(c) &&
        HiddenFromPlayers(c->IsTrigger(), c->GetDisplayId())) {
      triggers.push_back(c);
    }
  }
  return triggers;
}

// Where the mobs near `bot` that nobody has pulled stand: alive, visible,
// aggressive and hostile to it, not in combat. A dodge prefers spots clear of
// them (PreferSpotsClearOf).
std::vector<pb::Vec3> NearbyUnpulledHostiles(Player* bot) {
  constexpr float kRadiusYd = 40.0f;
  std::vector<Unit*> units;
  Acore::AnyUnitInObjectRangeCheck check(bot, kRadiusYd);
  Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units,
                                                                     check);
  Cell::VisitObjects(bot, searcher, kRadiusYd);
  std::vector<pb::Vec3> out;
  for (Unit* u : units) {
    Creature* c = u->ToCreature();
    if (c && c->IsAlive() && !c->IsInCombat() &&
        c->HasReactState(REACT_AGGRESSIVE) && bot->IsHostileTo(c) &&
        bot->CanSeeOrDetect(c) &&
        !HiddenFromPlayers(c->IsTrigger(), c->GetDisplayId())) {
      out.push_back(PosOf(c));
    }
  }
  return out;
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

// Resolves a synthetic `"trigger:<guid>"` id to a live, visible-hazard hidden
// trigger creature. When `required_name` is set, requires the hazard's
// player-visible spell name to match `*required_name`.
Creature* ResolveHiddenTriggerByGuid(
    Player* bot,
    std::string_view guid_str,
    std::optional<std::string_view> required_name = std::nullopt) {
  constexpr std::string_view kPrefix = "trigger:";
  if (!guid_str.starts_with(kPrefix)) {
    return nullptr;
  }
  ObjectGuid trigger_guid = ParseSnapshotGuid(guid_str.substr(kPrefix.size()));
  if (trigger_guid.IsEmpty()) {
    return nullptr;
  }
  Creature* trigger = ObjectAccessor::GetCreature(*bot, trigger_guid);
  if (!trigger || !trigger->IsAlive() || !bot->CanSeeOrDetect(trigger) ||
      !HiddenFromPlayers(trigger->IsTrigger(), trigger->GetDisplayId())) {
    return nullptr;
  }
  std::optional<TriggerHazardInfo> hz = InspectTriggerHazard(trigger);
  if (!hz) {
    return nullptr;
  }
  if (required_name && !NameEq(hz->name, *required_name)) {
    return nullptr;
  }
  return trigger;
}

// The party's minimap ping as a move anchor: a point on the ground (no
// radius, no facing), or nullopt while there is none to use.
std::optional<AnchorGeometry> PingAnchor(Player* bot) {
  const std::optional<PartyPingSpot> ping = CurrentPartyPing(bot);
  if (!ping) {
    return std::nullopt;
  }
  AnchorGeometry g;
  g.pos = {ping->x, ping->y, ping->z};
  return g;
}

bool IsToPing(const pb::Command_Act& act) {
  return act.verb == pb::Command_Act::TO && act.thing == pb::Command_Act::PING;
}

// Starts (where the bot stands, on arriving at the ping) or ends the bot's
// `to ping` hold (BotSession::ping_stay, BotManager::PingStayHolds).
void SetPingStay(Player* bot, bool hold) {
  std::optional<BotSession::PingStay> stay;
  if (hold) {
    stay = BotSession::PingStay{.map_id = bot->GetMapId(),
                                .instance_id = bot->GetInstanceId(),
                                .x = bot->GetPositionX(),
                                .y = bot->GetPositionY(),
                                .z = bot->GetPositionZ()};
  }
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(),
      [&stay](BotSession& s) { s.ping_stay = stay; });
}

// Resolves a `Command_Act::ThingKind` to a live `WorldObject*` (unit, object,
// or dynamic area) in the world.
WorldObject* ResolveThing(Player* bot,
                          const pb::Command_Act& act,
                          std::string_view thing_name,
                          bool allow_hidden_triggers = false) {
  // Prefer the exact GUID chosen by the harness when present so condition
  // evaluation and act execution always land on the same world entity.
  if (act.thing == pb::Command_Act::PING) {
    return nullptr;  // A spot on the ground, never a unit (PingAnchor).
  }
  if (act.thing != pb::Command_Act::THING_UNSPECIFIED &&
      act.thing != pb::Command_Act::ME && act.thing != pb::Command_Act::FOCUS &&
      !act.it_guid.empty()) {
    if (allow_hidden_triggers) {
      if (act.thing == pb::Command_Act::IT) {
        if (Creature* trigger = ResolveHiddenTriggerByGuid(bot, act.it_guid)) {
          return trigger;
        }
      } else if (act.thing == pb::Command_Act::NAMED ||
                 act.thing == pb::Command_Act::NEAREST_NAMED) {
        if (Creature* trigger =
                ResolveHiddenTriggerByGuid(bot, act.it_guid, thing_name)) {
          return trigger;
        }
      }
    }
    ObjectGuid guid = ParseSnapshotGuid(act.it_guid);
    if (!guid.IsEmpty()) {
      if (WorldObject* obj = ObjectAccessor::GetWorldObject(*bot, guid);
          obj && IsVisibleToBot(bot, obj)) {
        return obj;
      }
    }
  }
  switch (act.thing) {
    case pb::Command_Act::THING_UNSPECIFIED:
    case pb::Command_Act::PING:
      return nullptr;
    case pb::Command_Act::ME:
      return bot;
    case pb::Command_Act::IT: {
      if (!act.it_name.empty()) {
        pb::Command_Act by_name = act;
        by_name.it_guid.clear();
        by_name.thing = pb::Command_Act::NEAREST_NAMED;
        return ResolveThing(bot, by_name, act.it_name, allow_hidden_triggers);
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
      if (allow_hidden_triggers) {
        for (Creature* trigger : NearbyHiddenTriggers(bot)) {
          std::optional<TriggerHazardInfo> hz = InspectTriggerHazard(trigger);
          if (!hz) {
            continue;
          }
          if (NameEq(hz->name, thing_name)) {
            const float d = bot->GetDistance(trigger);
            if (d < best_dist) {
              best = trigger;
              best_dist = d;
            }
          }
        }
      }
      if (!best) {
        // Nothing living has the name: a dead party member's body (a
        // resurrection's target).
        best = DeadGroupMemberNamed(bot, thing_name);
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

    const bool used_path = (type & PATHFIND_NOT_USING_PATH) == 0;
    const pb::Vec3 dest = used_path ? pb::Vec3{end.x, end.y, end.z}
                                    : pb::Vec3{cand.x, cand.y, ground_z};

    bot->GetMotionMaster()->MovePoint(
        /*id=*/0, dest.x, dest.y, dest.z, FORCED_MOVEMENT_NONE,
        /*speed=*/0.0f, /*orientation=*/0.0f, /*generatePath=*/true,
        /*forceDestination=*/false);
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
      if (auto* last = ai->GetAiObjectContext()->GetValue<LastMovement&>(
              "last movement")) {
        const float speed = bot->GetSpeed(MOVE_RUN);
        const float ms =
            speed > 0.0f
                ? bot->GetExactDist(dest.x, dest.y, dest.z) / speed * 1000.0f
                : 1000.0f;
        last->Get().Set(bot->GetMapId(), dest.x, dest.y, dest.z,
                        bot->GetOrientation(), ms + 500.0f,
                        MovementPriority::MOVEMENT_FORCED);
      }
    }
    return dest;
  }
  return std::nullopt;
}

// A move act starting mid-cast (its walk already queued): cuts a cast or
// channel that would hold the bot still for more than kLetCastLandMs
// (CutCastForMove); after a nearly finished one lands, the walk starts.
void CutCastsForMove(Player* bot) {
  for (CurrentSpellTypes slot :
       {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL}) {
    Spell* sp = bot->GetCurrentSpell(slot);
    if (!sp || !sp->GetSpellInfo() ||
        (slot == CURRENT_GENERIC_SPELL
             ? sp->getState() != SPELL_STATE_PREPARING
             : sp->getState() == SPELL_STATE_FINISHED)) {
      continue;
    }
    const bool allows_move = slot == CURRENT_CHANNELED_SPELL &&
                             sp->GetSpellInfo()->IsActionAllowedChannel();
    if (CutCastForMove(sp->GetCastTimeRemaining(), allows_move)) {
      bot->InterruptSpell(slot, /*withDelayed=*/false);
    }
  }
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

// Captures cast-relevant state right before a client cast/use opcode so we can
// tell whether THIS spell/item actually started rather than seeing an existing
// global cooldown or another cast from playerbots.
struct CastBeforeState {
  bool on_cd = false;
  bool gcd = false;
  uint32_t mana = 0;
  bool target_had_aura = false;
  size_t queued_count = 0;
  Spell* prev_spells[CURRENT_MAX_SPELL] = {};
  uint32_t prev_spell_ids[CURRENT_MAX_SPELL] = {};
};

size_t QueuedSpellCount(const Player* bot, uint32_t spell_id) {
  if (!bot || spell_id == 0) {
    return 0;
  }
  size_t count = 0;
  for (const PendingSpellCastRequest& req : bot->SpellQueue) {
    if (!req.cancelInProgress && req.spellId == spell_id) {
      ++count;
    }
  }
  return count;
}

uint32_t CurrentSpellId(Player* bot, CurrentSpellTypes slot) {
  if (Spell* sp = bot->GetCurrentSpell(slot)) {
    if (const SpellInfo* info = sp->GetSpellInfo()) {
      return info->Id;
    }
  }
  return 0;
}

bool IsCurrentlyCastingSpell(Player* bot, uint32_t spell_id) {
  if (spell_id == 0) {
    return false;
  }
  if (QueuedSpellCount(bot, spell_id) > 0) {
    return true;
  }
  if (Spell* g = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      g && g->getState() == SPELL_STATE_PREPARING && g->GetSpellInfo() &&
      g->GetSpellInfo()->Id == spell_id) {
    return true;
  }
  if (Spell* c = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
      c && c->getState() != SPELL_STATE_FINISHED && c->GetSpellInfo() &&
      c->GetSpellInfo()->Id == spell_id) {
    return true;
  }
  return false;
}

CastBeforeState CaptureCastBefore(Player* bot,
                                  const SpellInfo* spell,
                                  Unit* target) {
  CastBeforeState s;
  s.mana = bot->GetPower(POWER_MANA);
  for (uint8_t i = 0; i < CURRENT_MAX_SPELL; ++i) {
    const auto slot = static_cast<CurrentSpellTypes>(i);
    s.prev_spells[i] = bot->GetCurrentSpell(slot);
    s.prev_spell_ids[i] = CurrentSpellId(bot, slot);
  }
  if (spell) {
    s.on_cd = bot->HasSpellCooldown(spell->Id);
    s.gcd = bot->GetGlobalCooldownMgr().HasGlobalCooldown(spell);
    s.target_had_aura = target && target->HasAura(spell->Id, bot->GetGUID());
    s.queued_count = QueuedSpellCount(bot, spell->Id);
  }
  return s;
}

bool CastStartedAfter(Player* bot,
                      const SpellInfo* spell,
                      Unit* target,
                      const CastBeforeState& before) {
  if (!spell) {
    return false;
  }
  // AzerothCore queues spells sent near the end of a cast/GCD into
  // `Player::SpellQueue` without populating `GetCurrentSpell` yet.
  if (QueuedSpellCount(bot, spell->Id) > before.queued_count) {
    return true;
  }
  // Check all current spell slots (including queued CURRENT_MELEE_SPELL and
  // CURRENT_AUTOREPEAT_SPELL), and compare pointer + spell id against before
  // so an already-running cast of the same spell does not read as newly
  // started.
  for (uint8_t i = 0; i < CURRENT_MAX_SPELL; ++i) {
    const auto slot = static_cast<CurrentSpellTypes>(i);
    Spell* cur = bot->GetCurrentSpell(slot);
    if (cur && cur->GetSpellInfo() && cur->GetSpellInfo()->Id == spell->Id &&
        (cur != before.prev_spells[i] ||
         before.prev_spell_ids[i] != spell->Id)) {
      return true;
    }
  }
  if (!before.on_cd && bot->HasSpellCooldown(spell->Id)) {
    return true;
  }
  if (!before.gcd && bot->GetGlobalCooldownMgr().HasGlobalCooldown(spell)) {
    return true;
  }
  if (bot->GetPower(POWER_MANA) < before.mana) {
    return true;
  }
  if (target && !before.target_had_aura &&
      target->HasAura(spell->Id, bot->GetGUID())) {
    return true;
  }
  return false;
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

void SaveLastInterrupted(uint64_t bot_guid,
                         const ActiveAct::InterruptedActivity& info) {
  BotSessionRegistry::Instance().WithSession(bot_guid, [&](BotSession& s) {
    s.last_interrupted = {info.active,       info.map_id,
                          info.instance_id,  info.target_guid,
                          info.prev_pos,     info.prev_target_dist_yd,
                          info.was_attacking};
  });
}

std::optional<ActiveAct::InterruptedActivity>
LoadLastInterrupted(uint64_t bot_guid, uint32_t map_id, uint32_t instance_id) {
  return BotSessionRegistry::Instance().WithSession(
      bot_guid,
      [map_id, instance_id](
          BotSession& s) -> std::optional<ActiveAct::InterruptedActivity> {
        if (!s.last_interrupted.active) {
          return std::nullopt;
        }
        if (s.last_interrupted.map_id != map_id ||
            s.last_interrupted.instance_id != instance_id) {
          s.last_interrupted = {};  // Never reuse old-map coordinates (A-L1).
          return std::nullopt;
        }
        const BotSession::InterruptedSpot& p = s.last_interrupted;
        return ActiveAct::InterruptedActivity{
            p.active,       p.map_id,   p.instance_id,
            p.target_guid,  p.prev_pos, p.prev_target_dist_yd,
            p.was_attacking};
      });
}

ActiveAct::InterruptedActivity CaptureInterruptedActivity(Player* bot) {
  ActiveAct::InterruptedActivity info;
  info.active = true;
  info.map_id = bot->GetMapId();
  info.instance_id = bot->GetInstanceId();
  info.prev_pos = PosOf(bot);
  Unit* target = bot->GetVictim();
  info.was_attacking = (target != nullptr);
  if (!target) {
    target = bot->GetSelectedUnit();
  }
  if (target && target->IsAlive() && target != bot &&
      bot->IsHostileTo(target)) {
    info.target_guid = target->GetGUID().GetRawValue();
    info.prev_target_dist_yd = bot->GetDistance(target);
  }
  return info;
}

bool IsCasterStillCasting(Player* bot,
                          uint64_t caster_guid,
                          uint32_t spell_id) {
  if (caster_guid == 0 || spell_id == 0) {
    return false;
  }
  Unit* caster = ObjectAccessor::GetUnit(*bot, ObjectGuid(caster_guid));
  if (!caster || !caster->IsAlive()) {
    return false;
  }
  if (Spell* sp = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      sp && sp->getState() == SPELL_STATE_PREPARING && sp->GetSpellInfo() &&
      sp->GetSpellInfo()->Id == spell_id) {
    return true;
  }
  if (Spell* ch = caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
      ch && ch->getState() != SPELL_STATE_FINISHED && ch->GetSpellInfo() &&
      ch->GetSpellInfo()->Id == spell_id) {
    return true;
  }
  return false;
}

struct HostileCircle {
  pb::Vec3 pos;
  float radius_yd = 0.0f;
};

std::vector<HostileCircle> CollectHostileAreas(Player* bot,
                                               float search_radius = 40.0f) {
  std::vector<HostileCircle> out;
  const float bot_reach = bot->GetCombatReach();
  for (DynamicObject* dyn : NearbyAreas(bot, search_radius)) {
    Unit* caster = dyn->GetCaster();
    if (!caster || !bot->IsHostileTo(caster)) {
      continue;
    }
    const float r = std::max(dyn->GetRadius(), 3.0f) + bot_reach + 1.0f;
    out.push_back({PosOf(dyn), r});
  }
  for (Creature* trigger : NearbyHiddenTriggers(bot, search_radius)) {
    if (bot->IsFriendlyTo(trigger)) {
      continue;
    }
    std::optional<TriggerHazardInfo> hz = InspectTriggerHazard(trigger);
    if (!hz) {
      continue;
    }
    const float r =
        std::max(hz->radius_yd > 0.0f ? hz->radius_yd : 8.0f, 3.0f) +
        bot_reach + 1.0f;
    out.push_back({PosOf(trigger), r});
  }
  return out;
}

bool SpotInsideHostileAreas(const std::vector<HostileCircle>& areas,
                            const pb::Vec3& spot) {
  for (const HostileCircle& a : areas) {
    if (Dist2D(spot, a.pos) <= a.radius_yd) {
      return true;
    }
  }
  return false;
}

bool SpotInsideHostileArea(Player* bot, const pb::Vec3& spot) {
  return SpotInsideHostileAreas(CollectHostileAreas(bot), spot);
}

// The group's tank as party frames show it: the main tank flag, the Dungeon
// Finder role when one was picked, else the spec's (a group made by invite
// has no roles).
bool IsTankSlot(const Group::MemberSlot& slot, Player* member) {
  if ((slot.flags & MEMBER_FLAG_MAINTANK) != 0) {
    return true;
  }
  if (slot.roles != 0) {
    return (slot.roles & lfg::PLAYER_ROLE_TANK) != 0;
  }
  return PlayerbotAI::IsTank(member);
}

// Which way out of a hazard keeps the group together: toward the tank, else
// the group's leader, else the middle of the living members (all outside the
// hazard). A tank itself steps straight out: walking to the group would bring
// what it holds onto them.
std::optional<pb::Vec3> PreferredEscapePos(Player* bot,
                                           const pb::Vec3& hazard_pos,
                                           float hazard_radius_yd) {
  Group* grp = bot->GetGroup();
  if (!grp) {
    return std::nullopt;
  }
  float sum_x = 0.0f;
  float sum_y = 0.0f;
  float sum_z = 0.0f;
  int count = 0;
  std::optional<pb::Vec3> leader_pos;
  for (const Group::MemberSlot& slot : grp->GetMemberSlots()) {
    Player* member = ObjectAccessor::FindPlayer(slot.guid);
    if (slot.guid == bot->GetGUID()) {
      if (member && IsTankSlot(slot, member)) {
        return std::nullopt;
      }
      continue;
    }
    if (!member || !member->IsInWorld() || !member->IsAlive() ||
        member->GetMapId() != bot->GetMapId() ||
        bot->GetDistance(member) > 60.0f) {
      continue;
    }
    const pb::Vec3 mpos = PosOf(member);
    if (hazard_radius_yd > 0.0f &&
        Dist2D(mpos, hazard_pos) < hazard_radius_yd + 1.0f) {
      continue;
    }
    if (IsTankSlot(slot, member)) {
      return mpos;
    }
    if (slot.guid == grp->GetLeaderGUID()) {
      leader_pos = mpos;
    }
    sum_x += mpos.x;
    sum_y += mpos.y;
    sum_z += mpos.z;
    ++count;
  }
  if (leader_pos) {
    return leader_pos;
  }
  if (count > 0) {
    return pb::Vec3{sum_x / static_cast<float>(count),
                    sum_y / static_cast<float>(count),
                    sum_z / static_cast<float>(count)};
  }
  return std::nullopt;
}

struct HazardInfo {
  AnchorGeometry anchor;
  float radius_yd = 0.0f;
  float lands_in_s = 0.0f;
  uint64_t caster_guid = 0;
  uint32_t spell_id = 0;
  bool is_ground_area = false;
};

HazardInfo MakeTriggerHazardInfo(Creature* trigger) {
  HazardInfo h;
  h.anchor = {PosOf(trigger), 0.0f, 0.0f};
  h.is_ground_area = true;
  if (std::optional<TriggerHazardInfo> hz = InspectTriggerHazard(trigger)) {
    h.radius_yd = hz->radius_yd;
    if (hz->lands_in_s) {
      h.lands_in_s = *hz->lands_in_s;
    }
    if (hz->is_preparing_cast) {
      h.caster_guid = trigger->GetGUID().GetRawValue();
      h.spell_id = hz->spell_id;
    }
  }
  if (h.radius_yd <= 0.0f) {
    h.radius_yd = 8.0f;
  }
  return h;
}

// Resolves a synthetic `"trigger:<guid>"` id on `act` when `act.thing` refers
// to the bound trigger (`THING_UNSPECIFIED`, `IT`, or `NAMED`/`NEAREST_NAMED`
// matching the trigger's visible hazard name).
std::optional<HazardInfo> ResolveTriggerHazard(Player* bot,
                                               const pb::Command_Act& act) {
  if (act.it_guid.empty()) {
    return std::nullopt;
  }
  Creature* trigger = nullptr;
  if (act.thing == pb::Command_Act::THING_UNSPECIFIED ||
      act.thing == pb::Command_Act::IT) {
    trigger = ResolveHiddenTriggerByGuid(bot, act.it_guid);
  } else if (act.thing == pb::Command_Act::NAMED ||
             act.thing == pb::Command_Act::NEAREST_NAMED) {
    const std::string_view thing_name =
        !act.target_name.empty() ? act.target_name : act.name;
    trigger = ResolveHiddenTriggerByGuid(bot, act.it_guid, thing_name);
  }
  if (!trigger) {
    return std::nullopt;
  }
  return MakeTriggerHazardInfo(trigger);
}

// Resolves a synthetic `"impact:<caster_guid>"` id (from `nearby_builder.cc`)
// to the ground-targeted cast's landing spot.
std::optional<AnchorGeometry> ResolveImpactAnchor(Player* bot,
                                                  std::string_view guid_str) {
  constexpr std::string_view kPrefix = "impact:";
  if (!guid_str.starts_with(kPrefix)) {
    return std::nullopt;
  }
  ObjectGuid caster_guid = ParseSnapshotGuid(guid_str.substr(kPrefix.size()));
  if (caster_guid.IsEmpty()) {
    return std::nullopt;
  }
  Unit* caster = ObjectAccessor::GetUnit(*bot, caster_guid);
  if (!caster || !caster->IsAlive()) {
    return std::nullopt;
  }
  if (Spell* cast = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      cast && cast->getState() == SPELL_STATE_PREPARING &&
      cast->m_targets.HasDst()) {
    if (const WorldLocation* dst = cast->m_targets.GetDstPos()) {
      return AnchorGeometry{
          {dst->GetPositionX(), dst->GetPositionY(), dst->GetPositionZ()},
          0.0f};
    }
  }
  return std::nullopt;
}

std::optional<HazardInfo> ResolveHazard(Player* bot,
                                        const pb::Command_Act& act) {
  if (!act.it_guid.empty()) {
    if (std::optional<HazardInfo> th = ResolveTriggerHazard(bot, act)) {
      return th;
    }
    constexpr std::string_view kImpactPrefix = "impact:";
    if (act.it_guid.starts_with(kImpactPrefix)) {
      ObjectGuid caster_guid =
          ParseSnapshotGuid(act.it_guid.substr(kImpactPrefix.size()));
      if (!caster_guid.IsEmpty()) {
        if (Unit* caster = ObjectAccessor::GetUnit(*bot, caster_guid);
            caster && caster->IsAlive()) {
          if (Spell* cast = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
              cast && cast->getState() == SPELL_STATE_PREPARING &&
              cast->m_targets.HasDst()) {
            if (const WorldLocation* dst = cast->m_targets.GetDstPos()) {
              const SpellInfo* info = cast->GetSpellInfo();
              HazardInfo h;
              h.anchor = {{dst->GetPositionX(), dst->GetPositionY(),
                           dst->GetPositionZ()},
                          0.0f,
                          0.0f};
              h.radius_yd = MaxSpellRadiusYd(info, caster, cast);
              if (h.radius_yd <= 0.0f) {
                h.radius_yd = 5.0f;
              }
              h.lands_in_s = static_cast<float>(
                                 std::max(0, cast->GetCastTimeRemaining())) /
                             1000.0f;
              h.caster_guid = caster->GetGUID().GetRawValue();
              h.spell_id = info ? info->Id : 0;
              return h;
            }
          }
        }
      }
    }
  }
  WorldObject* obj = nullptr;
  if (act.thing != pb::Command_Act::THING_UNSPECIFIED) {
    const std::string_view thing_name =
        !act.target_name.empty() ? act.target_name : act.name;
    obj = ResolveThing(bot, act, thing_name, /*allow_hidden_triggers=*/true);
  } else if (!act.it_guid.empty() || !act.it_name.empty()) {
    pb::Command_Act it_act = act;
    it_act.thing = pb::Command_Act::IT;
    obj =
        ResolveThing(bot, it_act, act.it_name, /*allow_hidden_triggers=*/true);
  }
  if (obj) {
    if (DynamicObject* dyn = obj->ToDynObject()) {
      HazardInfo h;
      h.anchor = {PosOf(dyn), 0.0f, 0.0f};
      h.radius_yd = dyn->GetRadius() > 0.0f ? dyn->GetRadius() : 5.0f;
      h.is_ground_area = true;
      return h;
    }
    if (Creature* c = obj->ToCreature();
        c && HiddenFromPlayers(c->IsTrigger(), c->GetDisplayId())) {
      return MakeTriggerHazardInfo(c);
    }
    if (Unit* u = obj->ToUnit()) {
      HazardInfo h;
      h.anchor = AnchorOf(u);
      Spell* sp = u->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      if (!sp || sp->getState() != SPELL_STATE_PREPARING) {
        sp = u->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (sp && sp->getState() == SPELL_STATE_FINISHED) {
          sp = nullptr;
        }
      }
      if (sp) {
        const SpellInfo* info = sp->GetSpellInfo();
        const float r = MaxSpellRadiusYd(info, u, sp);
        if (sp->m_targets.HasDst()) {
          if (const WorldLocation* dst = sp->m_targets.GetDstPos()) {
            h.anchor.pos = {dst->GetPositionX(), dst->GetPositionY(),
                            dst->GetPositionZ()};
            h.anchor.radius_yd = 0.0f;
          }
        }
        if (r > 0.0f) {
          h.radius_yd = r;
        }
        h.lands_in_s =
            static_cast<float>(std::max(0, sp->GetCastTimeRemaining())) /
            1000.0f;
        h.caster_guid = u->GetGUID().GetRawValue();
        h.spell_id = info ? info->Id : 0;
      }
      if (h.radius_yd <= 0.0f) {
        h.radius_yd = act.yd > 0.0f ? act.yd : kDefaultAwayYd;
      }
      return h;
    }
    HazardInfo h;
    h.anchor = AnchorOf(obj);
    h.radius_yd = act.yd > 0.0f ? act.yd : kDefaultAwayYd;
    return h;
  }
  return std::nullopt;
}

bool IsTargetAvoidedByBot(const Player* bot, uint64_t target_guid) {
  if (!bot || target_guid == 0) {
    return false;
  }
  return BotSessionRegistry::Instance().InspectSession(
      bot->GetGUID().GetRawValue(), [target_guid](const BotSession* s) {
        if (!s) {
          return false;
        }
        return std::find(s->avoided_targets.begin(), s->avoided_targets.end(),
                         target_guid) != s->avoided_targets.end();
      });
}

// Finishes a dodge (`away` / `out of`) without walking back to the pre-dodge
// spot: releases movement override and restores the interrupted attack target
// selection so normal combat resumes immediately. Returning to the pre-dodge
// spot happens only when an order explicitly specifies `back`.
void FinishDodgeAct(Player* bot, const ActiveAct& active) {
  bot->StopMoving();
  ClearActMovement(bot);
  const ActiveAct::InterruptedActivity& intr = active.interrupted;
  if (intr.active && intr.target_guid != 0 &&
      !IsTargetAvoidedByBot(bot, intr.target_guid)) {
    if (Unit* target =
            ObjectAccessor::GetUnit(*bot, ObjectGuid(intr.target_guid));
        target && target->IsAlive() && bot->IsHostileTo(target)) {
      if (bot->GetTarget().IsEmpty()) {
        bot->SetSelection(target->GetGUID());
      }
    }
  }
}

// Starts returning the bot to its interrupted engagement range/position after
// an explicit `back` act. Returns true if already in range / done (no return
// walk needed), false if a return walk is now active (`active->dodge_phase =
// kReturning`).
bool StartReturnToInterrupted(Player* bot,
                              ActiveAct* active,
                              base::TimeTicks now) {
  const ActiveAct::InterruptedActivity& intr = active->interrupted;
  if (!intr.active) {
    bot->StopMoving();
    ClearActMovement(bot);
    return true;
  }
  const pb::Vec3 bot_pos = PosOf(bot);
  const float bot_reach = bot->GetCombatReach();
  Unit* target = nullptr;
  if (intr.target_guid != 0 && !IsTargetAvoidedByBot(bot, intr.target_guid)) {
    target = ObjectAccessor::GetUnit(*bot, ObjectGuid(intr.target_guid));
    if (target && (!target->IsAlive() || !bot->IsHostileTo(target))) {
      target = nullptr;
    }
  }
  if (target && bot->GetTarget().IsEmpty()) {
    bot->SetSelection(target->GetGUID());
  }
  if (target) {
    const float max_range = BotMaxSpellRangeYd(bot);
    const float desired_dist = std::clamp(intr.prev_target_dist_yd, 2.0f,
                                          std::max(5.0f, max_range - 3.0f));
    const float cur_dist = bot->GetDistance(target);
    if (cur_dist <= desired_dist + 2.5f) {
      bot->StopMoving();
      ClearActMovement(bot);
      return true;
    }
    const AnchorGeometry target_anchor = AnchorOf(target);
    std::vector<pb::Vec3> candidates =
        CandidateSpotsForPlace(pb::Command_Act::YD_FROM, bot_pos, target_anchor,
                               desired_dist, max_range, bot_reach);
    std::vector<pb::Vec3> safe_candidates;
    safe_candidates.reserve(candidates.size());
    const std::vector<HostileCircle> hostile_areas = CollectHostileAreas(bot);
    for (const pb::Vec3& c : candidates) {
      if (!SpotInsideHostileAreas(hostile_areas, c)) {
        safe_candidates.push_back(c);
      }
    }
    if (!safe_candidates.empty()) {
      if (std::optional<pb::Vec3> chosen =
              MoveToFirstValidSpot(bot, safe_candidates, &target_anchor)) {
        active->dodge_phase = ActiveAct::DodgePhase::kReturning;
        active->dest = *chosen;
        active->last_repath_at = now;
        return false;
      }
    }
    bot->StopMoving();
    ClearActMovement(bot);
    return true;
  }
  if (!active->hazard_is_ground_area &&
      Dist2D(bot_pos, intr.prev_pos) > kArrivalToleranceYd + 1.0f &&
      !SpotInsideHostileArea(bot, intr.prev_pos)) {
    if (std::optional<pb::Vec3> chosen =
            MoveToFirstValidSpot(bot, {intr.prev_pos}, nullptr)) {
      active->dodge_phase = ActiveAct::DodgePhase::kReturning;
      active->dest = *chosen;
      active->last_repath_at = now;
      return false;
    }
  }
  bot->StopMoving();
  ClearActMovement(bot);
  return true;
}

// Resolves the anchor for `to` or `away`. For bare `away` (no `thing`), uses
// `it` when bound (including ground-spell impact spots), otherwise the nearest
// hostile dynamic area if standing near one, otherwise `bot`'s current spot.
std::optional<AnchorGeometry> ResolveMoveAnchor(Player* bot,
                                                const pb::Command_Act& act) {
  // `ping` is a spot, whatever unit an order's `when` bound to `it`.
  if (act.thing == pb::Command_Act::PING) {
    return PingAnchor(bot);
  }
  if (!act.it_guid.empty()) {
    if (std::optional<HazardInfo> th = ResolveTriggerHazard(bot, act)) {
      return th->anchor;
    }
    if (std::optional<AnchorGeometry> impact =
            ResolveImpactAnchor(bot, act.it_guid)) {
      return impact;
    }
  }
  if (act.thing != pb::Command_Act::THING_UNSPECIFIED) {
    const std::string_view thing_name =
        !act.target_name.empty() ? act.target_name : act.name;
    WorldObject* obj =
        ResolveThing(bot, act, thing_name, /*allow_hidden_triggers=*/true);
    if (!obj) {
      return std::nullopt;
    }
    return AnchorOf(obj);
  }
  // Bare `away`: prefer `it` when the order bound a target or impact spot.
  if (!act.it_guid.empty() || !act.it_name.empty()) {
    if (std::optional<AnchorGeometry> impact =
            ResolveImpactAnchor(bot, act.it_guid)) {
      return impact;
    }
    pb::Command_Act it_act = act;
    it_act.thing = pb::Command_Act::IT;
    if (WorldObject* obj = ResolveThing(bot, it_act, act.it_name,
                                        /*allow_hidden_triggers=*/true)) {
      return AnchorOf(obj);
    }
  }
  // Otherwise prefer nearest hostile dynamic ground effect (e.g. Defile),
  // ignoring friendly areas like friendly Consecration.
  DynamicObject* nearest_area = nullptr;
  float best_dist = 15.0f;
  for (DynamicObject* dyn : NearbyAreas(bot, 20.0f)) {
    Unit* caster = dyn->GetCaster();
    if (!caster || !bot->IsHostileTo(caster)) {
      continue;
    }
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

// How long a stealth cast may take to put its aura on before a spell usable
// only in stealth gives up.
constexpr base::TimeDelta kStealthSettleTime = base::Milliseconds(1500);
// A `use` walks to this far inside its spell's max range.
constexpr float kCastRangeMarginYd = 2.0f;

// Keeps playerbots' own movement (follow, back to a `stay` spot) off the bot
// for `ms` (MOVEMENT_FORCED where it stands), e.g. while it casts.
void HoldStill(Player* bot, float ms) {
  if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
    if (auto* last = ai->GetAiObjectContext()->GetValue<LastMovement&>(
            "last movement")) {
      last->Get().Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                      bot->GetPositionZ(), bot->GetOrientation(), ms,
                      MovementPriority::MOVEMENT_FORCED);
    }
  }
}

// Casts `spell` like a player's click (CMSG_CAST_SPELL): at `spot` (a ground
// spell on the ping), on the ground under `cast_target` (a ground spell on a
// unit), or on `cast_target`. A cast-time spell fills `*out_active` to wait
// for it.
ActStartResult CastSpellNow(Player* bot,
                            uint32_t command_id,
                            const pb::Command_Act& act,
                            const SpellInfo* spell,
                            Unit* cast_target,
                            const std::optional<pb::Vec3>& spot,
                            base::TimeTicks now,
                            ActiveAct* out_active) {
  // A spell with a cast time can't be cast on the move (the client stops
  // first); the bot then stands still until it lands.
  const uint32_t cast_time_ms = spell->CalcCastTime(bot);
  if (cast_time_ms > 0 && bot->isMoving() && CanClearBotMovement(bot)) {
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
  }
  // A spell cast at a unit in front (Spell.dbc FacingCasterFlags) turns the
  // bot to face it first, as a player turns before the cast.
  if (!spot && cast_target && cast_target != bot &&
      (spell->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) != 0 &&
      !bot->HasInArc(std::numbers::pi_v<float>, cast_target)) {
    if (bot->isMoving() && CanClearBotMovement(bot)) {
      bot->StopMoving();
      bot->GetMotionMaster()->Clear();
    }
    bot->SetOrientation(bot->GetAngle(cast_target));
    if (!bot->IsRooted()) {
      bot->SendMovementFlagUpdate(true);
    }
  }
  const CastBeforeState before = CaptureCastBefore(bot, spell, cast_target);
  const uint64_t bot_key = bot->GetGUID().GetRawValue();
  uint32_t miss_seq_before = 0;
  uint32_t failure_seq_before = 0;
  BotSessionRegistry::Instance().InspectSession(
      bot_key, [&](const BotSession* s) {
        if (s) {
          miss_seq_before = s->spell_miss_seq;
          failure_seq_before = s->cast_failure_seq;
        }
      });
  WorldPacket p(CMSG_CAST_SPELL);
  p << static_cast<uint8_t>(0) << spell->Id << static_cast<uint8_t>(0);
  SpellCastTargets targets;
  if (spot) {
    targets.SetDst(spot->x, spot->y, spot->z, bot->GetOrientation(),
                   bot->GetMapId());
  } else if ((spell->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION) !=
             0) {
    targets.SetDst(*cast_target);
  } else {
    targets.SetUnitTarget(cast_target);
  }
  targets.Write(p);
  p.rpos(0);
  bot->GetSession()->HandleCastSpellOpcode(p);
  std::optional<BotSession::SpellMiss> new_miss;
  std::optional<BotSession::CastFailure> new_failure;
  BotSessionRegistry::Instance().InspectSession(
      bot_key, [&](const BotSession* s) {
        if (!s) {
          return;
        }
        if (s->spell_miss && s->spell_miss->seq > miss_seq_before &&
            s->spell_miss->spell_id == spell->Id) {
          new_miss = *s->spell_miss;
        }
        if (s->cast_failure && s->cast_failure->seq > failure_seq_before &&
            s->cast_failure->spell_id == spell->Id) {
          new_failure = *s->cast_failure;
        }
      });
  if (new_miss) {
    if (new_miss->target.empty() && cast_target != nullptr) {
      new_miss->target = cast_target->GetName();
      BotSessionRegistry::Instance().WithSession(bot_key, [&](BotSession& s) {
        if (s.spell_miss && s.spell_miss->seq == new_miss->seq &&
            s.spell_miss->target.empty()) {
          s.spell_miss->target = new_miss->target;
        }
      });
    }
    return {.error = FormatSpellMissLine(new_miss->spell, new_miss->target,
                                         new_miss->reason)};
  }
  if (!CastStartedAfter(bot, spell, cast_target, before)) {
    if (new_failure) {
      if (const std::string_view why = CastFailureReason(new_failure->result);
          !why.empty()) {
        return {.error = base::StrCat({act.name, ": ", why})};
      }
    }
    return {.error = "cannot cast " + act.name + " now"};
  }
  if (!IsCurrentlyCastingSpell(bot, spell->Id)) {
    return {.done = true};
  }
  const pb::Command_Act act_copy = act;
  out_active->command_id = command_id;
  out_active->map_id = bot->GetMapId();
  out_active->instance_id = bot->GetInstanceId();
  out_active->act = act_copy;
  out_active->approach_cast = false;
  out_active->waiting_cast = true;
  out_active->cast_spell_id = spell->Id;
  out_active->cast_mana_before = before.mana;
  out_active->cast_cd_before = before.on_cd;
  out_active->cast_miss_seq_before = miss_seq_before;
  out_active->cast_failure_seq_before = failure_seq_before;
  out_active->cast_target_name = cast_target ? cast_target->GetName() : "";
  int32_t cast_ms = 0;
  if (Spell* cur = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      cur && cur->GetSpellInfo() && cur->GetSpellInfo()->Id == spell->Id) {
    cast_ms = cur->GetCastTime();
  } else if (Spell* ch = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
             ch && ch->GetSpellInfo() && ch->GetSpellInfo()->Id == spell->Id) {
    cast_ms = ch->GetCastTime();
  }
  out_active->cast_expected_end =
      now + base::Milliseconds(std::max<int32_t>(0, cast_ms));
  if (cast_ms > 0) {
    // Playerbots' own walks (follow, back to a `stay` spot) would interrupt
    // it: the cast owns the body until it lands.
    HoldStill(bot, static_cast<float>(cast_ms) + 500.0f);
  }
  return {.done = false};
}

// A known spell that puts a stealth aura on the bot (Spell.dbc: the aura,
// not a list of spells) and that moving doesn't break, highest rank first;
// nullptr when it knows none ready.
const SpellInfo* FindStealthSpell(Player* bot) {
  const SpellInfo* best = nullptr;
  for (const auto& [spell_id, player_spell] : bot->GetSpellMap()) {
    if (!player_spell || player_spell->State == PLAYERSPELL_REMOVED ||
        !player_spell->Active) {
      continue;
    }
    const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
    if (!info || info->IsPassive() || !info->HasAura(SPELL_AURA_MOD_STEALTH) ||
        (info->AuraInterruptFlags & AURA_INTERRUPT_FLAG_MOVE) != 0 ||
        bot->HasSpellCooldown(info->Id)) {
      continue;
    }
    if (!best || info->GetRank() > best->GetRank()) {
      best = info;
    }
  }
  return best;
}

// Where `spell` reaches from: whether the bot can cast it on `target` (or at
// `spot`) from where it stands, and the anchor to walk towards otherwise.
struct SpellReach {
  bool in_reach = true;
  float max_range_yd = 0.0f;
  AnchorGeometry anchor;
};

// `margin_yd`: how far inside the range counts (a walk in goes
// kCastRangeMarginYd inside, so a target stepping back doesn't break the
// cast; standing still, anything in range does).
SpellReach SpellReachFor(Player* bot,
                         const SpellInfo* spell,
                         Unit* target,
                         const std::optional<pb::Vec3>& spot,
                         float margin_yd) {
  SpellReach reach;
  if (!spot && (!target || target == bot)) {
    return reach;
  }
  const bool positive = !spot && bot->IsFriendlyTo(target);
  reach.max_range_yd = spell->GetMaxRange(positive, bot);
  if (reach.max_range_yd <= 0.0f) {
    return reach;
  }
  const bool ignores_los =
      spell->HasAttribute(SPELL_ATTR2_IGNORE_LINE_OF_SIGHT);
  float dist = 0.0f;
  bool los = true;
  if (spot) {
    reach.anchor.pos = *spot;
    dist = bot->GetDistance(spot->x, spot->y, spot->z);
    los = ignores_los || bot->IsWithinLOS(spot->x, spot->y, spot->z + 1.0f);
  } else {
    reach.anchor = AnchorOf(target);
    dist = bot->GetDistance(target);
    los = ignores_los || bot->IsWithinLOSInMap(target);
  }
  reach.in_reach = los && dist <= reach.max_range_yd - margin_yd;
  return reach;
}

// Spots `kCastRangeMarginYd` (plus `closer_yd`) inside `reach`'s range, in
// sight of its anchor. The range is taken centre to centre (whatever the
// server adds for combat reach, this is inside it).
std::optional<pb::Vec3> MoveIntoSpellReach(Player* bot,
                                           const SpellInfo* spell,
                                           const SpellReach& reach,
                                           float closer_yd = 0.0f) {
  const float yd =
      std::max(1.0f, reach.max_range_yd - kCastRangeMarginYd - closer_yd -
                         bot->GetCombatReach() - reach.anchor.radius_yd);
  const std::vector<pb::Vec3> candidates =
      CandidateSpotsForPlace(pb::Command_Act::YD_FROM, PosOf(bot), reach.anchor,
                             yd, yd, bot->GetCombatReach());
  const bool ignores_los =
      spell->HasAttribute(SPELL_ATTR2_IGNORE_LINE_OF_SIGHT);
  return MoveToFirstValidSpot(bot, candidates,
                              ignores_los ? nullptr : &reach.anchor);
}

// Uses a known spell the way a player has to (pure Spell.dbc facts): a spell
// usable only in stealth enters stealth first, and a target or spot out of
// range or sight is walked into range before the cast.
ActStartResult StartSpellUse(Player* bot,
                             uint32_t command_id,
                             const pb::Command_Act& act,
                             const SpellInfo* spell,
                             Unit* cast_target,
                             const std::optional<pb::Vec3>& spot,
                             base::TimeTicks now,
                             ActiveAct* out_active) {
  uint32_t stealth_id = 0;
  if (spell->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) &&
      !bot->HasStealthAura()) {
    const SpellInfo* stealth = FindStealthSpell(bot);
    if (!stealth) {
      return {.error = act.name + " needs stealth"};
    }
    const CastBeforeState before = CaptureCastBefore(bot, stealth, bot);
    WorldPacket p(CMSG_CAST_SPELL);
    p << static_cast<uint8_t>(0) << stealth->Id << static_cast<uint8_t>(0);
    SpellCastTargets targets;
    targets.SetUnitTarget(bot);
    targets.Write(p);
    p.rpos(0);
    bot->GetSession()->HandleCastSpellOpcode(p);
    if (!bot->HasStealthAura() &&
        !CastStartedAfter(bot, stealth, bot, before)) {
      return {.error = base::StrCat({act.name, " needs stealth: cannot use ",
                                     SpellLocalizedName(stealth), " now"})};
    }
    stealth_id = stealth->Id;
  }
  const SpellReach reach =
      SpellReachFor(bot, spell, cast_target, spot, /*margin_yd=*/0.5f);
  if (reach.in_reach && stealth_id == 0) {
    return CastSpellNow(bot, command_id, act, spell, cast_target, spot, now,
                        out_active);
  }
  // Walk in (or, in reach, just let the stealth settle) and cast from
  // StepActiveAct.
  pb::Vec3 dest = PosOf(bot);
  if (!reach.in_reach) {
    std::optional<pb::Vec3> chosen = MoveIntoSpellReach(bot, spell, reach);
    if (!chosen) {
      return {.error = "no path into range for " + act.name};
    }
    dest = *chosen;
  }
  out_active->command_id = command_id;
  out_active->map_id = bot->GetMapId();
  out_active->instance_id = bot->GetInstanceId();
  out_active->act = act;
  out_active->dest = dest;
  out_active->last_anchor = reach.anchor;
  out_active->last_repath_at = now;
  out_active->waiting_cast = false;
  out_active->approach_cast = true;
  out_active->approach_spell_id = spell->Id;
  out_active->approach_target_guid =
      spot ? 0 : cast_target->GetGUID().GetRawValue();
  out_active->approach_spot = spot;
  out_active->stealth_spell_id = stealth_id;
  out_active->stealth_started_at = now;
  return {.done = false};
}

// One tick of a `use` walking into its spell's reach (StartSpellUse).
ActStepResult StepSpellApproach(Player* bot,
                                base::TimeTicks now,
                                ActiveAct* active) {
  const SpellInfo* spell = sSpellMgr->GetSpellInfo(active->approach_spell_id);
  if (!spell || !bot->HasSpell(spell->Id)) {
    ClearActMovement(bot);
    return {.error = "you don't know that spell"};
  }
  Unit* target = bot;
  if (!active->approach_spot) {
    target =
        ObjectAccessor::GetUnit(*bot, ObjectGuid(active->approach_target_guid));
    if (!target || !IsVisibleToBot(bot, target)) {
      ClearActMovement(bot);
      return {.error = "spell target is gone"};
    }
  }
  if (spell->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED) &&
      !bot->HasStealthAura()) {
    if (active->stealth_spell_id == 0 ||
        (!IsCurrentlyCastingSpell(bot, active->stealth_spell_id) &&
         now - active->stealth_started_at >= kStealthSettleTime)) {
      ClearActMovement(bot);
      return {.error = active->act.name + " needs stealth: stealth broke"};
    }
    return {};  // The stealth cast hasn't put its aura on yet.
  }
  const SpellReach reach = SpellReachFor(
      bot, spell, target, active->approach_spot, kCastRangeMarginYd);
  if (reach.in_reach) {
    if (bot->isMoving() && CanClearBotMovement(bot)) {
      bot->StopMoving();
      bot->GetMotionMaster()->Clear();
    }
    HoldStill(bot, 1000.0f);
    if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(spell) ||
        bot->HasSpellCooldown(spell->Id)) {
      if (bot->HasSpellCooldown(spell->Id) &&
          !bot->GetGlobalCooldownMgr().HasGlobalCooldown(spell)) {
        return {.error = active->act.name + " is on cooldown"};
      }
      return {};  // Wait out the global cooldown (the stealth's, say).
    }
    const pb::Command_Act act = active->act;
    const std::optional<pb::Vec3> spot = active->approach_spot;
    active->approach_cast = false;
    const ActStartResult r = CastSpellNow(bot, active->command_id, act, spell,
                                          target, spot, now, active);
    if (!r.error.empty()) {
      return {.error = r.error};
    }
    return {.done = r.done};
  }
  if (now - active->last_repath_at < kRepathInterval) {
    return {};
  }
  const bool arrived = Dist2D(PosOf(bot), active->dest) <= kArrivalToleranceYd;
  if (bot->isMoving() && !arrived &&
      !AnchorMovedSignificantly(active->last_anchor, reach.anchor,
                                /*cares_facing=*/false)) {
    return {};
  }
  active->last_repath_at = now;
  if (arrived) {
    // Got there and still out of reach (sight, or a range measured another
    // way): the next spot is closer.
    active->approach_closer_yd += kCastRangeMarginYd;
    if (active->approach_closer_yd > reach.max_range_yd) {
      ClearActMovement(bot);
      return {.error = "cannot get into range for " + active->act.name};
    }
  }
  std::optional<pb::Vec3> chosen =
      MoveIntoSpellReach(bot, spell, reach, active->approach_closer_yd);
  if (!chosen) {
    ClearActMovement(bot);
    return {.error = "no path into range for " + active->act.name};
  }
  active->dest = *chosen;
  active->last_anchor = reach.anchor;
  return {};
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
  if (GET_PLAYERBOT_AI(bot) == nullptr) {
    return {.error = "not a bot"};
  }
  if (act.verb != pb::Command_Act::WAKE && !bot->IsAlive()) {
    return {.error = "you are dead"};
  }
  // Movement can't start on a flight path or while the bot's body isn't its
  // own (server H2): a queued spline would fight the taxi or the fear.
  const bool moves =
      act.verb == pb::Command_Act::TO || act.verb == pb::Command_Act::AWAY ||
      act.verb == pb::Command_Act::KITE ||
      act.verb == pb::Command_Act::OUT_OF ||
      act.verb == pb::Command_Act::BACK || act.verb == pb::Command_Act::AVOID;
  if (moves && bot->IsInFlight()) {
    return {.error = "cannot move while on a flight path"};
  }
  if (moves && bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED |
                                 UNIT_STATE_FLEEING | UNIT_STATE_CHARMED |
                                 UNIT_STATE_POSSESSED)) {
    return {.error = "cannot move right now (stunned, feared or controlled)"};
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
          if (CanClearBotMovement(bot)) {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
          }
          ClearActMovement(bot);
          break;
        case pb::Command_Act::STOP_UNSPECIFIED:
          bot->InterruptNonMeleeSpells(false);
          bot->AttackStop();
          if (CanClearBotMovement(bot)) {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
          }
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
      ClearAvoidedTargetForBot(bot->GetGUID().GetRawValue(),
                               unit->GetGUID().GetRawValue());
      // Same call as WorldSession::HandleSetSelectionOpcode.
      bot->SetSelection(unit->GetGUID());
      return {.done = true};
    }

    case pb::Command_Act::USE: {
      Unit* target = nullptr;
      std::optional<pb::Vec3> spot;
      if (act.thing == pb::Command_Act::PING) {
        const std::optional<AnchorGeometry> ping = PingAnchor(bot);
        if (!ping) {
          return {.error = "no minimap ping to use " + act.name + " on"};
        }
        spot = ping->pos;
      } else if (act.thing != pb::Command_Act::THING_UNSPECIFIED) {
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
        if (spot &&
            (spell->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION) == 0) {
          return {.error = act.name + " is not cast on the ground"};
        }
        Unit* cast_target = spot ? bot : (target ? target : bot);
        // Match WoW auto-self-cast: when no `on <target>` was given and a
        // helpful spell is cast while an enemy is selected, cast it on self.
        if (act.thing == pb::Command_Act::THING_UNSPECIFIED &&
            spell->IsPositive() && cast_target != bot &&
            !bot->IsFriendlyTo(cast_target)) {
          cast_target = bot;
        }
        return StartSpellUse(bot, command_id, act, spell, cast_target, spot,
                             now, out_active);
      }

      if (spot) {
        return {.error = "unknown spell: " + act.name};
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
        const SpellInfo* item_spell = sSpellMgr->GetSpellInfo(item_spell_id);
        Unit* item_target = target;
        if (act.thing == pb::Command_Act::THING_UNSPECIFIED && item_spell &&
            item_spell->IsPositive() && item_target && item_target != bot &&
            !bot->IsFriendlyTo(item_target)) {
          item_target = bot;
        }
        const uint32_t count_before = bot->GetItemCount(proto->ItemId);
        const CastBeforeState before =
            CaptureCastBefore(bot, item_spell, item_target ? item_target : bot);
        WorldPacket p(CMSG_USE_ITEM);
        p << found->bag << found->slot << static_cast<uint8_t>(0)
          << item_spell_id << item->GetGUID() << static_cast<uint32_t>(0)
          << static_cast<uint8_t>(0);
        SpellCastTargets targets;
        if (item_spell && (item_spell->GetExplicitTargetMask() &
                           TARGET_FLAG_DEST_LOCATION) != 0) {
          targets.SetDst(*(item_target ? item_target : bot));
        } else if (item_target) {
          targets.SetUnitTarget(item_target);
        }
        targets.Write(p);
        p.rpos(0);
        bot->GetSession()->HandleUseItemOpcode(p);
        if (bot->GetItemCount(proto->ItemId) >= count_before &&
            !CastStartedAfter(bot, item_spell, item_target ? item_target : bot,
                              before)) {
          return {.error = "cannot use " + act.name + " now"};
        }
        if (IsCurrentlyCastingSpell(bot, item_spell_id)) {
          out_active->command_id = command_id;
          out_active->map_id = bot->GetMapId();
          out_active->instance_id = bot->GetInstanceId();
          out_active->act = act;
          out_active->waiting_cast = true;
          out_active->cast_spell_id = item_spell_id;
          out_active->cast_mana_before = before.mana;
          out_active->cast_cd_before = before.on_cd;
          out_active->cast_expected_end = now;
          return {.done = false};
        }
        return {.done = true};
      }

      return {.error = "unknown spell or item: " + act.name};
    }

    case pb::Command_Act::OUT_OF: {
      std::optional<HazardInfo> hazard = ResolveHazard(bot, act);
      if (!hazard) {
        return {.error = "hazard not found"};
      }
      const pb::Vec3 bot_pos = PosOf(bot);
      const float bot_reach = bot->GetCombatReach();
      const float cur_dist = Dist2D(bot_pos, hazard->anchor.pos);
      const float effective_radius =
          hazard->radius_yd > 0.0f ? (hazard->radius_yd + bot_reach) : 0.0f;
      if (effective_radius > 0.0f && cur_dist >= effective_radius + 0.5f) {
        // Already outside the hazard radius (including bot combat reach).
        return {.done = true};
      }
      std::vector<pb::Vec3> candidates = CandidateSpotsForOutOf(
          bot_pos, hazard->anchor, hazard->radius_yd,
          PreferredEscapePos(bot, hazard->anchor.pos, effective_radius),
          kDefaultOutOfMarginYd, bot_reach);
      if (candidates.empty()) {
        return {.done = true};
      }
      if (Dist2D(bot_pos, candidates.front()) <= kArrivalToleranceYd) {
        return {.done = true};
      }
      std::optional<pb::Vec3> chosen = MoveToFirstValidSpot(
          bot,
          PreferSpotsClearOf(std::move(candidates), bot_pos,
                             NearbyUnpulledHostiles(bot)),
          nullptr);
      if (!chosen) {
        return {.error = "no path out of hazard"};
      }
      CutCastsForMove(bot);
      const ActiveAct::InterruptedActivity intr =
          CaptureInterruptedActivity(bot);
      SaveLastInterrupted(bot->GetGUID().GetRawValue(), intr);
      out_active->command_id = command_id;
      out_active->map_id = bot->GetMapId();
      out_active->instance_id = bot->GetInstanceId();
      out_active->act = act;
      out_active->dest = *chosen;
      out_active->last_anchor = hazard->anchor;
      out_active->away_target_edge_yd =
          effective_radius + kDefaultOutOfMarginYd;
      out_active->hazard_radius_yd = hazard->radius_yd;
      out_active->hazard_is_ground_area = hazard->is_ground_area;
      out_active->hazard_caster_guid = hazard->caster_guid;
      out_active->hazard_spell_id = hazard->spell_id;
      out_active->dodge_phase = ActiveAct::DodgePhase::kMovingOut;
      out_active->hold_deadline =
          now +
          base::Milliseconds(static_cast<int64_t>(
                                 std::max(0.0f, hazard->lands_in_s) * 1000.0f) +
                             1000);
      out_active->interrupted = intr;
      out_active->last_repath_at = now;
      out_active->waiting_cast = false;
      return {.done = false};
    }

    case pb::Command_Act::BACK: {
      std::optional<ActiveAct::InterruptedActivity> intr = LoadLastInterrupted(
          bot->GetGUID().GetRawValue(), bot->GetMapId(), bot->GetInstanceId());
      if (!intr) {
        return {.done = true};
      }
      out_active->command_id = command_id;
      out_active->map_id = bot->GetMapId();
      out_active->instance_id = bot->GetInstanceId();
      out_active->act = act;
      out_active->interrupted = *intr;
      out_active->last_repath_at = now;
      out_active->waiting_cast = false;
      if (StartReturnToInterrupted(bot, out_active, now)) {
        return {.done = true};
      }
      return {.done = false};
    }

    case pb::Command_Act::TO:
    case pb::Command_Act::AWAY: {
      std::optional<AnchorGeometry> anchor = ResolveMoveAnchor(bot, act);
      if (!anchor) {
        return {.error = act.thing == pb::Command_Act::PING
                             ? "no minimap ping to go to"
                             : "destination not found"};
      }
      if (act.verb == pb::Command_Act::TO) {
        // Going anywhere ends a `to ping` hold (a new one starts on arrival:
        // playerbots' `stay` would stop the walk there).
        SetPingStay(bot, false);
      }
      const pb::Vec3 bot_pos = PosOf(bot);
      const float bot_reach = bot->GetCombatReach();
      const float max_range = (act.verb == pb::Command_Act::TO &&
                               act.place == pb::Command_Act::MAX_RANGE)
                                  ? BotMaxSpellRangeYd(bot)
                                  : kDefaultMaxRangeYd;
      std::optional<HazardInfo> hazard;
      if (act.verb == pb::Command_Act::AWAY) {
        hazard = ResolveHazard(bot, act);
      }
      const float away_yd =
          (act.verb == pb::Command_Act::AWAY && act.yd <= 0.0f && hazard &&
           hazard->radius_yd > 0.0f)
              ? (hazard->radius_yd + kDefaultOutOfMarginYd)
              : act.yd;
      const float away_target_edge_yd =
          away_yd > 0.0f
              ? away_yd
              : (EdgeDist2D(bot_pos, bot_reach, *anchor) + kDefaultAwayYd);
      std::vector<pb::Vec3> candidates =
          (act.verb == pb::Command_Act::TO)
              ? CandidateSpotsForPlace(act.place, bot_pos, *anchor, act.yd,
                                       max_range, bot_reach)
              : CandidateSpotsForAway(bot_pos, *anchor, away_yd, bot_reach);
      if (candidates.empty()) {
        // Already far enough away.
        SetPingStay(bot, IsToPing(act));
        return {.done = true};
      }
      // Already at the ideal destination.
      if (Dist2D(bot_pos, candidates.front()) <= kArrivalToleranceYd) {
        SetPingStay(bot, IsToPing(act));
        return {.done = true};
      }
      const AnchorGeometry* los_anchor =
          (act.verb == pb::Command_Act::TO && PlaceNeedsLos(act.place))
              ? &*anchor
              : nullptr;
      if (act.verb == pb::Command_Act::AWAY) {
        candidates = PreferSpotsClearOf(std::move(candidates), bot_pos,
                                        NearbyUnpulledHostiles(bot));
      }
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, candidates, los_anchor);
      if (!chosen) {
        return {.error = "no path to destination"};
      }
      if (act.verb == pb::Command_Act::AWAY) {
        CutCastsForMove(bot);
      }
      out_active->command_id = command_id;
      out_active->map_id = bot->GetMapId();
      out_active->instance_id = bot->GetInstanceId();
      out_active->act = act;
      out_active->dest = *chosen;
      out_active->last_anchor = *anchor;
      out_active->away_target_edge_yd = away_target_edge_yd;
      out_active->last_repath_at = now;
      out_active->waiting_cast = false;
      if (act.verb == pb::Command_Act::AWAY) {
        const ActiveAct::InterruptedActivity intr =
            CaptureInterruptedActivity(bot);
        SaveLastInterrupted(bot->GetGUID().GetRawValue(), intr);
        out_active->interrupted = intr;
        out_active->dodge_phase = ActiveAct::DodgePhase::kMovingOut;
        if (hazard) {
          out_active->hazard_radius_yd = hazard->radius_yd;
          out_active->hazard_is_ground_area = hazard->is_ground_area;
          out_active->hazard_caster_guid = hazard->caster_guid;
          out_active->hazard_spell_id = hazard->spell_id;
          out_active->hold_deadline =
              now + base::Milliseconds(
                        static_cast<int64_t>(
                            std::max(0.0f, hazard->lands_in_s) * 1000.0f) +
                        1000);
        }
      }
      return {.done = false};
    }

    case pb::Command_Act::AVOID: {
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      if (obj) {
        const uint64_t raw = obj->GetGUID().GetRawValue();
        BotSessionRegistry::Instance().WithSession(
            bot->GetGUID().GetRawValue(), [raw](BotSession& s) {
              if (std::find(s.avoided_targets.begin(), s.avoided_targets.end(),
                            raw) == s.avoided_targets.end()) {
                s.avoided_targets.push_back(raw);
              }
            });
        if (bot->GetTarget() == obj->GetGUID()) {
          bot->AttackStop();
          bot->SetSelection(ObjectGuid::Empty);
        }
      }
      return {.done = true};
    }

    case pb::Command_Act::FACE:
    case pb::Command_Act::FACE_AWAY: {
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj =
          ResolveThing(bot, act, thing_name, /*allow_hidden_triggers=*/true);
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

    case pb::Command_Act::JUMP: {
      if (bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED |
                            UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING |
                            UNIT_STATE_POSSESSED) ||
          bot->HasUnitMovementFlag(MOVEMENTFLAG_ROOT) || bot->IsFalling() ||
          bot->IsInFlight()) {
        return {.error = "cannot jump right now"};
      }
      // Match real WoW jump airborne time (~0.83 s for speedZ = 8.0 yd/s under
      // 19.29 yd/s^2 gravity) while keeping SegLength > 0 so MoveSplineInit
      // does not clamp to a 1 ms orientation-only spline.
      constexpr float kJumpSpeedZ = 8.0f;
      constexpr float kJumpAirTimeS = 0.83f;
      constexpr float kStepYd = 0.5f;
      const float o = bot->GetOrientation();
      float jx = bot->GetPositionX() + kStepYd * std::cos(o);
      float jy = bot->GetPositionY() + kStepYd * std::sin(o);
      float jz = bot->GetPositionZ();
      bot->UpdateAllowedPositionZ(jx, jy, jz);
      if (std::abs(jz - bot->GetPositionZ()) > 1.5f ||
          !bot->IsWithinLOS(jx, jy, jz + 0.5f)) {
        // Forward step blocked by wall or cliff: try a small backward hop
        // checked with the same LoS/Z guards rather than nudging past a ledge.
        jx = bot->GetPositionX() - kStepYd * std::cos(o);
        jy = bot->GetPositionY() - kStepYd * std::sin(o);
        jz = bot->GetPositionZ();
        bot->UpdateAllowedPositionZ(jx, jy, jz);
        if (std::abs(jz - bot->GetPositionZ()) > 1.5f ||
            !bot->IsWithinLOS(jx, jy, jz + 0.5f)) {
          return {.error = "blocked from jumping"};
        }
      }
      const float dist_3d = std::hypot(kStepYd, jz - bot->GetPositionZ());
      const float speed_xy = std::max(0.15f, dist_3d / kJumpAirTimeS);
      bot->GetMotionMaster()->MoveJump(jx, jy, jz, speed_xy, kJumpSpeedZ);
      return {.done = true};
    }

    case pb::Command_Act::CANCEL: {
      uint32_t cancel_id = 0;
      for (const auto& [spell_id, aura] : bot->GetAppliedAuras()) {
        const SpellInfo* info = aura->GetBase()->GetSpellInfo();
        if (info && NameEq(SpellLocalizedName(info), act.name)) {
          cancel_id = spell_id;
          break;
        }
      }
      if (cancel_id == 0) {
        return {.error = "cannot cancel aura: " + act.name};
      }
      // Go through the client's cancel-aura handler so debuffs and
      // uncancellable/passive auras are rejected just as in the game client.
      WorldPacket p(CMSG_CANCEL_AURA, 4);
      p << cancel_id;
      p.rpos(0);
      bot->GetSession()->HandleCancelAuraOpcode(p);
      if (bot->HasAura(cancel_id)) {
        return {.error = "cannot cancel aura: " + act.name};
      }
      return {.done = true};
    }

    case pb::Command_Act::MARK: {
      const uint8_t icon = RaidMarkerIconIndex(act.marker);
      const std::string_view thing_name =
          !act.target_name.empty() ? act.target_name : act.name;
      WorldObject* obj = ResolveThing(bot, act, thing_name);
      Unit* unit = obj ? obj->ToUnit() : nullptr;
      if (!unit) {
        return {.error = "unit to mark not found"};
      }
      ActionOutcome res = MarkUnitWithIcon(bot, unit, icon);
      return res.error.empty() ? ActStartResult{.done = true}
                               : ActStartResult{.error = std::move(res.error)};
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
      out_active->map_id = bot->GetMapId();
      out_active->instance_id = bot->GetInstanceId();
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
        if (!bot->GetVehicle()) {
          return {.error = "could not board vehicle"};
        }
        return {.done = true};
      }
      const AnchorGeometry anchor = AnchorOf(unit);
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, {anchor.pos}, nullptr);
      if (!chosen) {
        return {.error = "no path to vehicle"};
      }
      out_active->command_id = command_id;
      out_active->map_id = bot->GetMapId();
      out_active->instance_id = bot->GetInstanceId();
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
        if (bot->GetVehicle()) {
          return {.error = "could not leave vehicle"};
        }
        return {.done = true};
      }
      if (bot->IsInFlight()) {
        return {.error = "cannot dismount while on a flight path"};
      }
      if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED |
                            UNIT_STATE_FLEEING | UNIT_STATE_CHARMED |
                            UNIT_STATE_POSSESSED)) {
        return {.error = "cannot dismount right now"};
      }
      if (bot->IsMounted()) {
        bot->Dismount();
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        if (bot->IsMounted()) {
          return {.error = "could not dismount"};
        }
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
        ActionOutcome res =
            EquipBagItem(bot, entry.item, entry.bag, entry.slot, act.name);
        return res.error.empty()
                   ? ActStartResult{.done = true}
                   : ActStartResult{.error = std::move(res.error)};
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
        case pb::Command_Act::DISMISS: {
          // The client's Dismiss: a spell with the dismiss-pet effect when the
          // bot knows one (a hunter's Dismiss Pet: the pet bar's abandon
          // would delete a hunter's pet for good), else the pet bar's
          // dismiss (a summoned demon, a guardian).
          for (const auto& [id, player_spell] : bot->GetSpellMap()) {
            const SpellInfo* info = sSpellMgr->GetSpellInfo(id);
            if (!player_spell || player_spell->State == PLAYERSPELL_REMOVED ||
                !player_spell->Active || !info ||
                !info->HasEffect(SPELL_EFFECT_DISMISS_PET)) {
              continue;
            }
            pb::Command_Act use = act;
            use.name = std::string(SpellLocalizedName(info));
            return CastSpellNow(bot, command_id, use, info, pet, std::nullopt,
                                now, out_active);
          }
          if (pet->getPetType() == HUNTER_PET) {
            return {.error = "you can't dismiss your pet (no Dismiss Pet)"};
          }
          bot->GetSession()->HandlePetActionHelper(pet, pet->GetGUID(),
                                                   COMMAND_ABANDON, ACT_COMMAND,
                                                   ObjectGuid::Empty);
          return {.done = true};
        }
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
      std::vector<pb::Vec3> candidates = CandidateSpotsForKite(
          PosOf(bot), *anchor, kDefaultKiteYd, bot->GetCombatReach());
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, candidates, nullptr);
      if (!chosen) {
        return {.error = "no path to kite"};
      }
      CutCastsForMove(bot);
      out_active->command_id = command_id;
      out_active->map_id = bot->GetMapId();
      out_active->instance_id = bot->GetInstanceId();
      out_active->act = act;
      out_active->dest = *chosen;
      out_active->last_anchor = *anchor;
      out_active->last_repath_at = now;
      out_active->waiting_cast = false;
      return {.done = false};
    }

    case pb::Command_Act::TRADE_GOLD: {
      const ActionOutcome out = ExecuteTradeGold(bot, act.copper);
      if (!out.error.empty()) {
        return {.error = out.error};
      }
      return {.done = true};
    }
  }
  return {.error = "unsupported act"};
}

ActStartResult StartSpellUseOn(Player* bot,
                               uint32_t command_id,
                               const SpellInfo* spell,
                               Unit* target,
                               base::TimeTicks now,
                               ActiveAct* out_active) {
  pb::Command_Act act;
  act.verb = pb::Command_Act::USE;
  act.name = std::string(SpellLocalizedName(spell));
  return StartSpellUse(bot, command_id, act, spell, target, std::nullopt, now,
                       out_active);
}

ActStepResult StepActiveAct(Player* bot,
                            base::TimeTicks now,
                            ActiveAct* active) {
  if (!bot || !bot->IsInWorld() || GET_PLAYERBOT_AI(bot) == nullptr ||
      !bot->IsAlive()) {
    if (bot && GET_PLAYERBOT_AI(bot) != nullptr) {
      ClearActMovement(bot);
    }
    return {.error = "you died"};
  }
  if (active->map_id != bot->GetMapId() ||
      active->instance_id != bot->GetInstanceId()) {
    ClearActMovement(bot);
    ClearLastInterruptedForBot(bot->GetGUID().GetRawValue());
    return {.error = "map changed"};
  }
  if (active->waiting_cast) {
    const uint64_t bot_key = bot->GetGUID().GetRawValue();
    std::optional<BotSession::SpellMiss> new_miss;
    std::optional<BotSession::CastFailure> new_failure;
    BotSessionRegistry::Instance().InspectSession(
        bot_key, [&](const BotSession* s) {
          if (!s) {
            return;
          }
          if (s->spell_miss &&
              s->spell_miss->seq > active->cast_miss_seq_before &&
              s->spell_miss->spell_id == active->cast_spell_id) {
            new_miss = *s->spell_miss;
          }
          if (s->cast_failure &&
              s->cast_failure->seq > active->cast_failure_seq_before &&
              s->cast_failure->spell_id == active->cast_spell_id) {
            new_failure = *s->cast_failure;
          }
        });
    if (new_miss) {
      if (new_miss->target.empty() && !active->cast_target_name.empty()) {
        new_miss->target = active->cast_target_name;
        BotSessionRegistry::Instance().WithSession(bot_key, [&](BotSession& s) {
          if (s.spell_miss && s.spell_miss->seq == new_miss->seq &&
              s.spell_miss->target.empty()) {
            s.spell_miss->target = new_miss->target;
          }
        });
      }
      return {.error = FormatSpellMissLine(new_miss->spell, new_miss->target,
                                           new_miss->reason)};
    }
    if (IsCurrentlyCastingSpell(bot, active->cast_spell_id)) {
      return {};
    }
    const bool finished_normally =
        (!active->cast_cd_before &&
         bot->HasSpellCooldown(active->cast_spell_id)) ||
        bot->GetPower(POWER_MANA) < active->cast_mana_before ||
        active->cast_expected_end.is_null() ||
        now + base::Milliseconds(250) >= active->cast_expected_end;
    if (finished_normally) {
      return {.done = true};
    }
    if (new_failure) {
      if (const std::string_view why = CastFailureReason(new_failure->result);
          !why.empty()) {
        return {.error = base::StrCat({active->act.name, ": ", why})};
      }
    }
    return {.error = "cast interrupted"};
  }
  if (active->approach_cast) {
    return StepSpellApproach(bot, now, active);
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
      if (!bot->GetVehicle()) {
        return {.error = "could not board vehicle"};
      }
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
    const float bot_reach = bot->GetCombatReach();
    if (!bot->isMoving() ||
        Dist2D(bot_pos, active->dest) <= kArrivalToleranceYd + 2.0f ||
        EdgeDist2D(bot_pos, bot_reach, *cur_anchor) < kDefaultKiteYd * 0.6f) {
      std::vector<pb::Vec3> candidates = CandidateSpotsForKite(
          bot_pos, *cur_anchor, kDefaultKiteYd, bot_reach);
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

  if (act.verb != pb::Command_Act::TO && act.verb != pb::Command_Act::AWAY &&
      act.verb != pb::Command_Act::OUT_OF &&
      act.verb != pb::Command_Act::BACK) {
    return {.done = true};
  }

  const pb::Vec3 bot_pos = PosOf(bot);
  const float bot_reach = bot->GetCombatReach();
  if (act.verb == pb::Command_Act::BACK) {
    bool back_in_range =
        Dist2D(bot_pos, active->dest) <= kArrivalToleranceYd + 1.0f;
    if (!back_in_range && active->interrupted.target_guid != 0) {
      if (Unit* target = ObjectAccessor::GetUnit(
              *bot, ObjectGuid(active->interrupted.target_guid));
          target && target->IsAlive()) {
        const float max_range = BotMaxSpellRangeYd(bot);
        const float desired_dist =
            std::clamp(active->interrupted.prev_target_dist_yd, 2.0f,
                       std::max(5.0f, max_range - 3.0f));
        if (bot->GetDistance(target) <= desired_dist + 2.5f) {
          back_in_range = true;
        }
      }
    }
    if (back_in_range) {
      bot->StopMoving();
      ClearActMovement(bot);
      return {.done = true};
    }
    if (now - active->last_repath_at >= kRepathInterval && !bot->isMoving()) {
      active->last_repath_at = now;
      if (StartReturnToInterrupted(bot, active, now)) {
        return {.done = true};
      }
    }
    return {};
  }

  if (act.verb == pb::Command_Act::AWAY ||
      act.verb == pb::Command_Act::OUT_OF) {
    const bool cast_active =
        active->hazard_caster_guid != 0 && active->hazard_spell_id != 0 &&
        IsCasterStillCasting(bot, active->hazard_caster_guid,
                             active->hazard_spell_id);
    if (active->dodge_phase == ActiveAct::DodgePhase::kHolding) {
      if (cast_active && now < active->hold_deadline) {
        if (now - active->last_repath_at >= kRepathInterval) {
          active->last_repath_at = now;
          if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
            if (auto* last = ai->GetAiObjectContext()->GetValue<LastMovement&>(
                    "last movement")) {
              last->Get().Set(bot->GetMapId(), bot->GetPositionX(),
                              bot->GetPositionY(), bot->GetPositionZ(),
                              bot->GetOrientation(), 500.0f,
                              MovementPriority::MOVEMENT_FORCED);
            }
          }
        }
        return {};
      }
      FinishDodgeAct(bot, *active);
      return {.done = true};
    }

    // If we were dodging a cast and the cast already ended (landed or
    // interrupted), stop running further away and resume attacking.
    if (active->hazard_caster_guid != 0 && active->hazard_spell_id != 0 &&
        !cast_active) {
      FinishDodgeAct(bot, *active);
      return {.done = true};
    }

    const float target_yd = active->away_target_edge_yd > 0.0f
                                ? active->away_target_edge_yd
                                : (act.yd > 0.0f ? act.yd : kDefaultAwayYd);
    const float cur_metric =
        (act.verb == pb::Command_Act::OUT_OF)
            ? Dist2D(bot_pos, active->last_anchor.pos)
            : EdgeDist2D(bot_pos, bot_reach, active->last_anchor);
    if (cur_metric >= target_yd ||
        Dist2D(bot_pos, active->dest) <= kArrivalToleranceYd) {
      if (cast_active && now < active->hold_deadline) {
        bot->StopMoving();
        active->dodge_phase = ActiveAct::DodgePhase::kHolding;
        active->last_repath_at = now;
        return {};
      }
      FinishDodgeAct(bot, *active);
      return {.done = true};
    }
    // Keep the anchor fixed from StartAct so bare `away` (anchored on the
    // starting spot) and `away <moving mob>` finish cleanly instead of
    // chasing a shifting anchor forever.
    if (now - active->last_repath_at >= kRepathInterval && !bot->isMoving()) {
      active->last_repath_at = now;
      const float effective_radius =
          active->hazard_radius_yd > 0.0f
              ? (active->hazard_radius_yd + bot_reach)
              : 0.0f;
      std::vector<pb::Vec3> candidates =
          (act.verb == pb::Command_Act::OUT_OF)
              ? CandidateSpotsForOutOf(
                    bot_pos, active->last_anchor, active->hazard_radius_yd,
                    PreferredEscapePos(bot, active->last_anchor.pos,
                                       effective_radius),
                    kDefaultOutOfMarginYd, bot_reach)
              : CandidateSpotsForAway(bot_pos, active->last_anchor, target_yd,
                                      bot_reach);
      if (candidates.empty()) {
        if (cast_active && now < active->hold_deadline) {
          bot->StopMoving();
          active->dodge_phase = ActiveAct::DodgePhase::kHolding;
          return {};
        }
        FinishDodgeAct(bot, *active);
        return {.done = true};
      }
      std::optional<pb::Vec3> chosen = MoveToFirstValidSpot(
          bot,
          PreferSpotsClearOf(std::move(candidates), bot_pos,
                             NearbyUnpulledHostiles(bot)),
          nullptr);
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
    SetPingStay(bot, IsToPing(act));
    return {.done = true};
  }

  // Recompute relative spot if the anchor moved/turned (e.g. boss rotated)
  // or if the bot stopped moving before arriving.
  if (now - active->last_repath_at < kRepathInterval) {
    return {};
  }
  active->last_repath_at = now;

  if (active->fixed_spot) {
    if (!bot->isMoving()) {
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, {active->dest}, nullptr);
      if (!chosen) {
        ClearActMovement(bot);
        return {.error = "no path to destination"};
      }
      active->dest = *chosen;
    }
    return {};
  }

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
        act.place, bot_pos, *cur_anchor, act.yd, max_range, bot_reach);
    if (candidates.empty()) {
      ClearActMovement(bot);
      SetPingStay(bot, IsToPing(act));
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

bool ActMovesOut(const ActiveAct& active) {
  switch (active.act.verb) {
    case pb::Command_Act::OUT_OF:
    case pb::Command_Act::AWAY:
      return active.dodge_phase == ActiveAct::DodgePhase::kMovingOut;
    case pb::Command_Act::KITE:
      return true;
    default:
      return active.fixed_spot;
  }
}

ActStartResult StartSpreadAct(Player* bot,
                              const AnchorGeometry& center,
                              size_t slot_index,
                              size_t total_bots,
                              base::TimeTicks now,
                              ActiveAct* out_active) {
  if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !out_active) {
    return {.error = "bot cannot spread right now"};
  }
  const pb::Vec3 bot_pos = PosOf(bot);
  std::vector<pb::Vec3> candidates = CandidateSpotsForSpread(
      bot_pos, center, slot_index, total_bots, kDefaultSpreadRadiusYd);
  if (candidates.empty()) {
    return {.done = true};
  }
  candidates = PreferSpotsClearOf(std::move(candidates), bot_pos,
                                  NearbyUnpulledHostiles(bot));
  if (Dist2D(bot_pos, candidates.front()) <= kArrivalToleranceYd) {
    return {.done = true};
  }
  std::optional<pb::Vec3> chosen =
      MoveToFirstValidSpot(bot, candidates, nullptr);
  if (!chosen) {
    return {.error = "no path to spread"};
  }
  SetPingStay(bot, false);
  CutCastsForMove(bot);
  out_active->command_id = 0;
  out_active->map_id = bot->GetMapId();
  out_active->instance_id = bot->GetInstanceId();
  out_active->act = {};
  out_active->act.verb = pb::Command_Act::TO;
  out_active->dest = *chosen;
  out_active->last_anchor = center;
  out_active->last_repath_at = now;
  out_active->waiting_cast = false;
  out_active->fixed_spot = true;
  out_active->hold_on_arrival = true;
  return {.done = false};
}

bool ActHoldsStealth(const ActiveAct& active) {
  if (!active.approach_cast) {
    return false;
  }
  const SpellInfo* spell = sSpellMgr->GetSpellInfo(active.approach_spell_id);
  return spell && spell->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED);
}

void CancelActiveAct(Player* bot, ActiveAct* active) {
  if (!bot || !active || GET_PLAYERBOT_AI(bot) == nullptr) {
    return;
  }
  const pb::Command_Act::Verb verb = active->act.verb;
  if (verb == pb::Command_Act::TO || verb == pb::Command_Act::AWAY ||
      verb == pb::Command_Act::OUT_OF || verb == pb::Command_Act::BACK ||
      verb == pb::Command_Act::KITE || verb == pb::Command_Act::INTERACT ||
      verb == pb::Command_Act::BOARD) {
    if (bot->IsInWorld()) {
      if (CanClearBotMovement(bot)) {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
      }
      ClearActMovement(bot);
    }
  }
  *active = ActiveAct{};
}

void ClearAvoidedTargetsForBot(uint64_t bot_guid) {
  BotSessionRegistry::Instance().WithSession(
      bot_guid, [](BotSession& s) { s.avoided_targets.clear(); });
}

void ClearAvoidedTargetForBot(uint64_t bot_guid, uint64_t target_guid) {
  BotSessionRegistry::Instance().WithSession(
      bot_guid, [target_guid](BotSession& s) {
        auto& vec = s.avoided_targets;
        vec.erase(std::remove(vec.begin(), vec.end(), target_guid), vec.end());
      });
}

void ClearLastInterruptedForBot(uint64_t bot_guid) {
  BotSessionRegistry::Instance().WithSession(
      bot_guid, [](BotSession& s) { s.last_interrupted = {}; });
}

}  // namespace llm_brain
