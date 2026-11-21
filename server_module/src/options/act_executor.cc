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
#include "DBCStructure.h"
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
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "options/act_geometry.h"
#include "options/option_executor.h"
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
  // Prefer the exact GUID chosen by the harness when present so condition
  // evaluation and act execution always land on the same world entity.
  if (act.thing != pb::Command_Act::THING_UNSPECIFIED &&
      act.thing != pb::Command_Act::ME && act.thing != pb::Command_Act::FOCUS &&
      !act.it_guid.empty()) {
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
      return nullptr;
    case pb::Command_Act::ME:
      return bot;
    case pb::Command_Act::IT: {
      if (!act.it_guid.empty()) {
        ObjectGuid guid = ParseSnapshotGuid(act.it_guid);
        if (!guid.IsEmpty()) {
          if (WorldObject* obj = ObjectAccessor::GetWorldObject(*bot, guid);
              obj && IsVisibleToBot(bot, obj)) {
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

// Computes max DBC effect radius in yards for a spell.
float MaxSpellRadiusYd(const SpellInfo* info,
                       Unit* caster,
                       Spell* spell = nullptr) {
  if (!info) {
    return 0.0f;
  }
  float max_r = 0.0f;
  for (uint8_t eff = 0; eff < MAX_SPELL_EFFECTS; ++eff) {
    const float r = info->Effects[eff].CalcRadius(caster, spell);
    if (r > max_r) {
      max_r = r;
    }
  }
  return max_r;
}

// Per-bot last interrupted activity (world thread only) so explicit `then back`
// can return to the pre-dodge spot/range.
std::vector<std::pair<uint64_t, ActiveAct::InterruptedActivity>>&
LastInterruptedStore() {
  static std::vector<std::pair<uint64_t, ActiveAct::InterruptedActivity>> store;
  return store;
}

void SaveLastInterrupted(uint64_t bot_guid,
                         const ActiveAct::InterruptedActivity& info) {
  auto& store = LastInterruptedStore();
  for (auto& [guid, saved] : store) {
    if (guid == bot_guid) {
      saved = info;
      return;
    }
  }
  if (store.size() >= 256) {
    store.erase(store.begin());
  }
  store.emplace_back(bot_guid, info);
}

std::optional<ActiveAct::InterruptedActivity> LoadLastInterrupted(
    uint64_t bot_guid) {
  for (const auto& [guid, saved] : LastInterruptedStore()) {
    if (guid == bot_guid && saved.active) {
      return saved;
    }
  }
  return std::nullopt;
}

ActiveAct::InterruptedActivity CaptureInterruptedActivity(Player* bot) {
  ActiveAct::InterruptedActivity info;
  info.active = true;
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

bool SpotInsideHostileArea(Player* bot, const pb::Vec3& spot) {
  for (DynamicObject* dyn : NearbyAreas(bot, 40.0f)) {
    Unit* caster = dyn->GetCaster();
    if (!caster || !bot->IsHostileTo(caster)) {
      continue;
    }
    const float r = std::max(dyn->GetRadius(), 3.0f);
    if (Dist2D(spot, PosOf(dyn)) <= r + 1.0f) {
      return true;
    }
  }
  return false;
}

std::optional<pb::Vec3> PreferredEscapePos(Player* bot) {
  Group* grp = bot->GetGroup();
  if (!grp) {
    return std::nullopt;
  }
  // Prefer moving toward the tank (if bot is not the tank), else group
  // centroid.
  float sum_x = 0.0f;
  float sum_y = 0.0f;
  float sum_z = 0.0f;
  int count = 0;
  for (const Group::MemberSlot& slot : grp->GetMemberSlots()) {
    if (slot.guid == bot->GetGUID()) {
      continue;
    }
    Player* member = ObjectAccessor::FindPlayer(slot.guid);
    if (!member || !member->IsInWorld() || !member->IsAlive() ||
        member->GetMapId() != bot->GetMapId() ||
        bot->GetDistance(member) > 60.0f) {
      continue;
    }
    if ((slot.flags & MEMBER_FLAG_MAINTANK) != 0 ||
        (slot.roles & lfg::PLAYER_ROLE_TANK) != 0) {
      return PosOf(member);
    }
    sum_x += member->GetPositionX();
    sum_y += member->GetPositionY();
    sum_z += member->GetPositionZ();
    ++count;
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

// Resolves a synthetic `"trigger:<guid>"` id (from `nearby_builder.cc`) to the
// hidden trigger creature's ground position and active cast/aura radius.
std::optional<HazardInfo> ResolveTriggerHazard(Player* bot,
                                               std::string_view guid_str) {
  constexpr std::string_view kPrefix = "trigger:";
  if (!guid_str.starts_with(kPrefix)) {
    return std::nullopt;
  }
  ObjectGuid trigger_guid = ParseSnapshotGuid(guid_str.substr(kPrefix.size()));
  if (trigger_guid.IsEmpty()) {
    return std::nullopt;
  }
  Creature* trigger = ObjectAccessor::GetCreature(*bot, trigger_guid);
  if (!trigger || !trigger->IsAlive()) {
    return std::nullopt;
  }
  HazardInfo h;
  h.anchor = {PosOf(trigger), 0.0f, 0.0f};
  h.is_ground_area = true;
  if (Spell* cast = trigger->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      cast && cast->getState() == SPELL_STATE_PREPARING) {
    const SpellInfo* info = cast->GetSpellInfo();
    h.radius_yd = MaxSpellRadiusYd(info, trigger, cast);
    h.lands_in_s =
        static_cast<float>(std::max(0, cast->GetCastTimeRemaining())) / 1000.0f;
    h.caster_guid = trigger->GetGUID().GetRawValue();
    h.spell_id = info ? info->Id : 0;
  } else {
    for (const auto& [spell_id, aura_app] : trigger->GetAppliedAuras()) {
      if (!aura_app || !aura_app->GetBase()) {
        continue;
      }
      const SpellInfo* ainfo = aura_app->GetBase()->GetSpellInfo();
      if (!ainfo || ainfo->IsPassive()) {
        continue;
      }
      if (ainfo->SpellVisual[0] == 0 && ainfo->SpellVisual[1] == 0) {
        continue;
      }
      h.radius_yd = MaxSpellRadiusYd(ainfo, trigger);
      break;
    }
  }
  if (h.radius_yd <= 0.0f) {
    h.radius_yd = 8.0f;
  }
  return h;
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
    if (std::optional<HazardInfo> th = ResolveTriggerHazard(bot, act.it_guid)) {
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
    obj = ResolveThing(bot, act, thing_name);
  } else if (!act.it_guid.empty() || !act.it_name.empty()) {
    pb::Command_Act it_act = act;
    it_act.thing = pb::Command_Act::IT;
    obj = ResolveThing(bot, it_act, act.it_name);
  }
  if (obj) {
    if (DynamicObject* dyn = obj->ToDynObject()) {
      HazardInfo h;
      h.anchor = {PosOf(dyn), 0.0f, 0.0f};
      h.radius_yd = dyn->GetRadius() > 0.0f ? dyn->GetRadius() : 5.0f;
      h.is_ground_area = true;
      return h;
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

// Starts returning the bot to its interrupted engagement range/position after
// a dodge finishes. Returns true if already in range / done (no return walk
// needed), false if a return walk is now active (`active->dodge_phase =
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
  if (intr.target_guid != 0) {
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
    for (const pb::Vec3& c : candidates) {
      if (!SpotInsideHostileArea(bot, c)) {
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
  if (!act.it_guid.empty()) {
    if (std::optional<HazardInfo> th = ResolveTriggerHazard(bot, act.it_guid)) {
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
    WorldObject* obj = ResolveThing(bot, act, thing_name);
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
    if (WorldObject* obj = ResolveThing(bot, it_act, act.it_name)) {
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
        // Match WoW auto-self-cast: when no `on <target>` was given and a
        // helpful spell is cast while an enemy is selected, cast it on self.
        if (act.thing == pb::Command_Act::THING_UNSPECIFIED &&
            spell->IsPositive() && cast_target != bot &&
            !bot->IsFriendlyTo(cast_target)) {
          cast_target = bot;
        }
        const CastBeforeState before =
            CaptureCastBefore(bot, spell, cast_target);
        WorldPacket p(CMSG_CAST_SPELL);
        p << static_cast<uint8_t>(0) << spell->Id << static_cast<uint8_t>(0);
        SpellCastTargets targets;
        targets.SetUnitTarget(cast_target);
        targets.Write(p);
        p.rpos(0);
        bot->GetSession()->HandleCastSpellOpcode(p);
        if (!CastStartedAfter(bot, spell, cast_target, before)) {
          return {.error = "cannot cast " + act.name + " now"};
        }
        if (IsCurrentlyCastingSpell(bot, spell->Id)) {
          out_active->command_id = command_id;
          out_active->act = act;
          out_active->waiting_cast = true;
          out_active->cast_spell_id = spell->Id;
          out_active->cast_mana_before = before.mana;
          out_active->cast_cd_before = before.on_cd;
          int32_t cast_ms = 0;
          if (Spell* cur = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
              cur && cur->GetSpellInfo() &&
              cur->GetSpellInfo()->Id == spell->Id) {
            cast_ms = cur->GetCastTime();
          } else if (Spell* ch = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                     ch && ch->GetSpellInfo() &&
                     ch->GetSpellInfo()->Id == spell->Id) {
            cast_ms = ch->GetCastTime();
          }
          out_active->cast_expected_end =
              now + base::Milliseconds(std::max<int32_t>(0, cast_ms));
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
        if (item_target) {
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
      const float cur_dist = Dist2D(bot_pos, hazard->anchor.pos);
      if (hazard->radius_yd > 0.0f && cur_dist >= hazard->radius_yd + 0.5f) {
        // Already outside the hazard radius — skip dodge.
        return {.done = true};
      }
      if (hazard->lands_in_s > 0.0f &&
          !CanEscapeBeforeImpact(cur_dist, hazard->radius_yd,
                                 bot->GetSpeed(MOVE_RUN), hazard->lands_in_s)) {
        // Impact lands before the bot could exit — skip wasted movement.
        return {.done = true};
      }
      std::vector<pb::Vec3> candidates = CandidateSpotsForOutOf(
          bot_pos, hazard->anchor, hazard->radius_yd, PreferredEscapePos(bot));
      if (candidates.empty()) {
        return {.done = true};
      }
      if (Dist2D(bot_pos, candidates.front()) <= kArrivalToleranceYd) {
        return {.done = true};
      }
      std::optional<pb::Vec3> chosen =
          MoveToFirstValidSpot(bot, candidates, nullptr);
      if (!chosen) {
        return {.error = "no path out of hazard"};
      }
      const ActiveAct::InterruptedActivity intr =
          CaptureInterruptedActivity(bot);
      SaveLastInterrupted(bot->GetGUID().GetRawValue(), intr);
      out_active->command_id = command_id;
      out_active->act = act;
      out_active->dest = *chosen;
      out_active->last_anchor = hazard->anchor;
      out_active->away_target_edge_yd =
          hazard->radius_yd + kDefaultOutOfMarginYd;
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
      std::optional<ActiveAct::InterruptedActivity> intr =
          LoadLastInterrupted(bot->GetGUID().GetRawValue());
      if (!intr) {
        return {.done = true};
      }
      out_active->command_id = command_id;
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
        return {.error = "destination not found"};
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
      std::vector<pb::Vec3> candidates = CandidateSpotsForKite(
          PosOf(bot), *anchor, kDefaultKiteYd, bot->GetCombatReach());
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
    return {.error = "cast interrupted"};
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
  if (act.verb == pb::Command_Act::BACK ||
      ((act.verb == pb::Command_Act::AWAY ||
        act.verb == pb::Command_Act::OUT_OF) &&
       active->dodge_phase == ActiveAct::DodgePhase::kReturning)) {
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
      if (StartReturnToInterrupted(bot, active, now)) {
        return {.done = true};
      }
      return {};
    }

    // If we were dodging a cast and the cast already ended (landed or
    // interrupted), stop running further away and return immediately.
    if (active->hazard_caster_guid != 0 && active->hazard_spell_id != 0 &&
        !cast_active) {
      if (StartReturnToInterrupted(bot, active, now)) {
        return {.done = true};
      }
      return {};
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
      if (StartReturnToInterrupted(bot, active, now)) {
        return {.done = true};
      }
      return {};
    }
    // Keep the anchor fixed from StartAct so bare `away` (anchored on the
    // starting spot) and `away <moving mob>` finish cleanly instead of
    // chasing a shifting anchor forever.
    if (now - active->last_repath_at >= kRepathInterval && !bot->isMoving()) {
      active->last_repath_at = now;
      std::vector<pb::Vec3> candidates =
          (act.verb == pb::Command_Act::OUT_OF)
              ? CandidateSpotsForOutOf(bot_pos, active->last_anchor,
                                       active->hazard_radius_yd,
                                       PreferredEscapePos(bot))
              : CandidateSpotsForAway(bot_pos, active->last_anchor, target_yd,
                                      bot_reach);
      if (candidates.empty()) {
        if (cast_active && now < active->hold_deadline) {
          bot->StopMoving();
          active->dodge_phase = ActiveAct::DodgePhase::kHolding;
          return {};
        }
        if (StartReturnToInterrupted(bot, active, now)) {
          return {.done = true};
        }
        return {};
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
        act.place, bot_pos, *cur_anchor, act.yd, max_range, bot_reach);
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

void CancelActiveAct(Player* bot, ActiveAct* active) {
  if (!bot || !active) {
    return;
  }
  const pb::Command_Act::Verb verb = active->act.verb;
  if (verb == pb::Command_Act::TO || verb == pb::Command_Act::AWAY ||
      verb == pb::Command_Act::OUT_OF || verb == pb::Command_Act::BACK ||
      verb == pb::Command_Act::KITE || verb == pb::Command_Act::INTERACT ||
      verb == pb::Command_Act::BOARD) {
    if (bot->IsInWorld()) {
      bot->StopMoving();
      bot->GetMotionMaster()->Clear();
      ClearActMovement(bot);
    }
  }
  *active = ActiveAct{};
}

}  // namespace llm_brain
