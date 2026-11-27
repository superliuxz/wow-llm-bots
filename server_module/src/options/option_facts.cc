// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/option_facts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CreatureData.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourStatus.h"
#include "G3D/Vector3.h"
#include "GameGraveyard.h"
#include "GameObjectData.h"
#include "GameTime.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "InstanceScript.h"
#include "Map.h"
#include "MapDefines.h"
#include "MapMgr.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Position.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "Transport.h"
#include "TransportMgr.h"
#include "TravelMgr.h"
#include "UnitDefines.h"
#include "World.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "options/leader_travel.h"
#include "options/position_facts.h"
#include "snapshot/perception.h"
#include "snapshot/snapshot.h"
#include "snapshot/world_util.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr float kHubMergeRadiusYd = 250.0f;
constexpr float kGraveyardMergeYd = 120.0f;
constexpr size_t kMaxSpots = 3;
// Hubs this much beyond minimap range are kept, so a bot that moves before the
// next recompute (SnapshotBuilder: 20 yd) still sees them come into range.
constexpr float kMinimapMarginYd = 50.0f;

std::string PlaceId(uint32_t map_id, float x, float y) {
  return Acore::StringFormat("place:{}:{}:{}", map_id,
                             static_cast<int>(std::lround(x)),
                             static_cast<int>(std::lround(y)));
}

std::string SpotId(uint32_t map_id, float x, float y) {
  return Acore::StringFormat("spot:{}:{}:{}", map_id,
                             static_cast<int>(std::lround(x)),
                             static_cast<int>(std::lround(y)));
}

uint32_t DirBit(pb::Direction dir) {
  return 1u << static_cast<uint8_t>(dir);
}

std::string PluralizeNoun(std::string word) {
  if (word.empty()) {
    return word;
  }
  if (word == "wolf") {
    return "wolves";
  }
  if (word.ends_with("s") || word.ends_with("sh") || word.ends_with("ch") ||
      word.ends_with("x")) {
    return word;
  }
  return word + "s";
}

std::string MobNoun(const CreatureTemplate* tmpl) {
  if (tmpl->family != 0) {
    if (const CreatureFamilyEntry* fam =
            sCreatureFamilyStore.LookupEntry(tmpl->family)) {
      const char* raw = fam->Name[sWorld->GetDefaultDbcLocale()];
      if (raw && *raw) {
        return PluralizeNoun(base::ToLowerASCII(raw));
      }
    }
  }
  return base::ToLowerASCII(tmpl->Name);
}

uint64_t BucketKey(uint32_t map_id, float x, float y, float z) {
  const auto cell = [](float v) {
    return static_cast<uint64_t>(
        static_cast<uint16_t>(static_cast<int16_t>(std::lround(v / 50.0f))));
  };
  return (static_cast<uint64_t>(map_id & 0xFFFFu) << 48) | (cell(x) << 32) |
         (cell(y) << 16) | cell(z);
}

bool IsCapital(uint32_t zone_id) {
  const AreaTableEntry* zone = sAreaTableStore.LookupEntry(zone_id);
  return zone && (zone->flags & AREA_FLAG_CAPITAL) != 0;
}

void FillSpots(const WorldPlaces& world,
               const Viewpoint& view,
               const WorldObject* path_owner,
               const DungeonNavConfig& dungeon_nav,
               std::vector<pb::OptionFacts::Spot>* out) {
  const bool in_city = IsCapital(view.zone_id);
  Map* map = sMapMgr->CreateBaseMap(view.map_id);
  std::vector<pb::OptionFacts::Spot> candidates;
  const auto add_candidate = [&](float x, float y, float z, std::string label) {
    const float dist = Dist(view.pos, x, y, z);
    if (dist < kMinTravelDistYd || dist > kMaxTravelDistYd) {
      return;
    }
    const pb::Direction dir = DirFrom(view.pos, x, y);
    if (dir == pb::DIRECTION_UNSPECIFIED || dir == pb::HERE) {
      return;
    }
    candidates.push_back(
        {SpotId(view.map_id, x, y), dir, dist, std::move(label), {x, y, z}});
  };

  // Playerbots' per-level spot cache (what SelectRandomGrindPos picks from),
  // labeled with the 50 yd bucket's mobs. Distance first: the zone lookup is
  // the expensive part.
  for (const WorldLocation& loc : sTravelMgr.GetLocsPerLevelCache(view.level)) {
    if (loc.GetMapId() != view.map_id) {
      continue;
    }
    const float dist = Dist(view.pos, loc.GetPositionX(), loc.GetPositionY(),
                            loc.GetPositionZ());
    if (dist < kMinTravelDistYd || dist > kMaxTravelDistYd) {
      continue;
    }
    if (!in_city && map &&
        map->GetZoneId(view.phase_mask, loc.GetPositionX(), loc.GetPositionY(),
                       loc.GetPositionZ()) != view.zone_id) {
      continue;
    }
    const WorldPlaces::SpotBucket* bucket =
        world.FindBucket(view.map_id, loc.GetPositionX(), loc.GetPositionY(),
                         loc.GetPositionZ());
    add_candidate(loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ(),
                  bucket ? bucket->Label() : "");
  }

  // None in this zone at this level: try the zone's buckets within 3 levels,
  // then fall back to neighboring zones on the same continent (within
  // kMaxTravelDistYd) so an outleveled bot (e.g. level 14 in Elwynn Forest,
  // where max mob level is 10) gets concrete travel options into the next zone.
  if (candidates.empty()) {
    for (const auto& [key, b] : world.buckets()) {
      if (b.map_id != view.map_id || (!in_city && b.zone_id != view.zone_id) ||
          b.count < 2) {
        continue;
      }
      if (b.min_level > view.level + 3u ||
          (view.level > 3 && b.max_level + 3u < view.level)) {
        continue;
      }
      add_candidate(b.centroid.x, b.centroid.y, b.centroid.z, b.Label());
    }
  }
  if (candidates.empty() && !in_city) {
    for (const auto& [key, b] : world.buckets()) {
      if (b.map_id != view.map_id || b.zone_id == view.zone_id || b.count < 2) {
        continue;
      }
      if (b.min_level > view.level + 4u ||
          (view.level > 4 && b.max_level + 4u < view.level)) {
        continue;
      }
      std::string label = b.Label();
      if (const std::string zname = AreaName(b.zone_id); !zname.empty()) {
        label = label.empty() ? zname : base::StrCat({zname, ": ", label});
      }
      add_candidate(b.centroid.x, b.centroid.y, b.centroid.z, std::move(label));
    }
  }

  // Nearest first, one per compass direction: the same choice on every
  // snapshot (never a random pick), so the options don't flicker.
  std::sort(candidates.begin(), candidates.end(),
            [](const pb::OptionFacts::Spot& a, const pb::OptionFacts::Spot& b) {
              return a.dist_yd < b.dist_yd;
            });
  uint32_t seen_dirs = 0;
  for (pb::OptionFacts::Spot& spot : candidates) {
    if (out->size() >= kMaxSpots) {
      break;
    }
    if ((seen_dirs & DirBit(spot.dir)) != 0) {
      continue;
    }
    seen_dirs |= DirBit(spot.dir);
    out->push_back(std::move(spot));
  }

  // No grind spot at all (an instance, a city): in dungeon instances, first
  // expose navmesh-reachable clusters of living hostile packs/bosses so the
  // leader can navigate multi-room corridors and turns instead of blind rays.
  if (!out->empty() || !path_owner) {
    return;
  }
  Map* probe_map = path_owner->GetMap();
  if (!probe_map) {
    probe_map = map;
  }
  if (!probe_map) {
    return;
  }
  if (probe_map->IsDungeon()) {
    if (const Player* bot = path_owner->ToPlayer()) {
      std::vector<DungeonHostile> hostiles;
      const auto& spawned = probe_map->GetCreatureBySpawnIdStore();
      const FactionTemplateEntry* bot_ft = bot->GetFactionTemplateEntry();
      const InstanceScript* instance_script = nullptr;
      if (const InstanceMap* im = probe_map->ToInstanceMap()) {
        instance_script = im->GetInstanceScript();
      }
      if (const auto* db_spawns = world.DungeonSpawns(view.map_id)) {
        const uint8_t mode_bit =
            static_cast<uint8_t>(1u << probe_map->GetSpawnMode());
        for (const WorldPlaces::DungeonSpawn& sp : *db_spawns) {
          if (sp.spawn_mask != 0 && (sp.spawn_mask & mode_bit) == 0) {
            continue;
          }
          const auto [begin, end] = spawned.equal_range(sp.spawn_id);
          if (begin != end) {
            for (auto it = begin; it != end; ++it) {
              Creature* c = it->second;
              if (!c || !c->IsInWorld() || !c->IsAlive() || c->IsCritter() ||
                  c->IsTotem() || c->IsPet() ||
                  c->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE |
                                 UNIT_FLAG_NOT_SELECTABLE |
                                 UNIT_FLAG_IMMUNE_TO_PC) ||
                  !bot->IsHostileTo(c)) {
                continue;
              }
              hostiles.push_back(
                  {c->GetName(),
                   {c->GetPositionX(), c->GetPositionY(), c->GetPositionZ()},
                   sp.is_boss || c->IsDungeonBoss() || c->isWorldBoss()});
            }
          } else if (bot_ft && !probe_map->IsGridLoaded(sp.pos.x, sp.pos.y) &&
                     probe_map->GetCreatureRespawnTime(sp.spawn_id) <=
                         GameTime::GetGameTime().count() &&
                     !(instance_script &&
                       instance_script->IsBossSpawnDone(sp.spawn_id)) &&
                     (sp.phase_mask & view.phase_mask) != 0) {
            // Grid not yet loaded in this instance and spawn has no pending
            // respawn timer: check template faction hostility so long empty
            // entrance corridors still expose concrete rooms/bosses ahead.
            // Check mob->bot too: player factions have hostileMask for the
            // opposite player faction, while monster factions (e.g. 16) set
            // hostileMask for players.
            const FactionTemplateEntry* mob_ft =
                sFactionTemplateStore.LookupEntry(sp.faction);
            if (mob_ft && (mob_ft->IsHostileTo(*bot_ft) ||
                           bot_ft->IsHostileTo(*mob_ft))) {
              hostiles.push_back({sp.name, sp.pos, sp.is_boss});
            }
          }
        }
      }
      const std::vector<DungeonCluster> clusters =
          ClusterDungeonHostiles(view.pos, hostiles, dungeon_nav);
      const float unloaded_probe_yd =
          (!std::isfinite(dungeon_nav.unloaded_probe_yd) ||
           dungeon_nav.unloaded_probe_yd < kMinTravelDistYd)
              ? kMinTravelDistYd
              : dungeon_nav.unloaded_probe_yd;
      const float progress_slack_yd =
          (!std::isfinite(dungeon_nav.incomplete_progress_slack_yd) ||
           dungeon_nav.incomplete_progress_slack_yd < 0.0f)
              ? 0.0f
              : dungeon_nav.incomplete_progress_slack_yd;
      constexpr uint32_t kDungeonPathOk =
          PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
      for (const DungeonCluster& cl : clusters) {
        if (out->size() >= kMaxSpots) {
          break;
        }
        PathGenerator path(path_owner);
        path.CalculatePath(cl.pos.x, cl.pos.y, cl.pos.z);
        uint32_t pt = path.GetPathType();
        bool normal = (pt & PATHFIND_NORMAL) != 0;
        bool incomplete = (pt & PATHFIND_INCOMPLETE) != 0;
        if ((pt & ~kDungeonPathOk) != 0 || (!normal && !incomplete)) {
          // Far cluster in an unloaded grid: probe an intermediate step toward
          // it.
          if (cl.dist_yd > unloaded_probe_yd) {
            const float scale = unloaded_probe_yd / cl.dist_yd;
            const float mx = view.pos.x + (cl.pos.x - view.pos.x) * scale;
            const float my = view.pos.y + (cl.pos.y - view.pos.y) * scale;
            const float mz = probe_map->GetHeight(view.phase_mask, mx, my,
                                                  view.pos.z + 15.0f);
            if (mz != INVALID_HEIGHT && mz != VMAP_INVALID_HEIGHT_VALUE) {
              path.CalculatePath(mx, my, mz);
              pt = path.GetPathType();
              normal = (pt & PATHFIND_NORMAL) != 0;
              incomplete = (pt & PATHFIND_INCOMPLETE) != 0;
            }
          }
          if ((pt & ~kDungeonPathOk) != 0 || (!normal && !incomplete)) {
            continue;
          }
        }
        const G3D::Vector3& end = path.GetActualEndPosition();
        const float step_dist = Dist(view.pos, end.x, end.y, end.z);
        if (step_dist < kMinTravelDistYd) {
          continue;
        }
        if (incomplete &&
            Dist({end.x, end.y, end.z}, cl.pos.x, cl.pos.y, cl.pos.z) +
                    progress_slack_yd >=
                cl.dist_yd) {
          continue;
        }
        const pb::Direction dir = DirFrom(view.pos, end.x, end.y);
        if (dir == pb::DIRECTION_UNSPECIFIED || dir == pb::HERE ||
            (seen_dirs & DirBit(dir)) != 0) {
          continue;
        }
        seen_dirs |= DirBit(dir);
        out->push_back({SpotId(view.map_id, end.x, end.y),
                        dir,
                        step_dist,
                        cl.label,
                        {end.x, end.y, end.z}});
      }
      if (!out->empty()) {
        return;
      }
      // Disconnected entrance ledge/stair seam (e.g. Ahn'kahet portal ledge
      // where Detour stops 5 yd away at a small drop-off): if a validated
      // terrain + navmesh hop exists toward the nearest hostile cluster,
      // expose that cluster so TickLongWalk can step across the seam and
      // resume normal navmesh pathing.
      if (dungeon_nav.seam_bridge_enable) {
        for (const DungeonCluster& cl : clusters) {
          if (out->size() >= kMaxSpots) {
            break;
          }
          const float base_angle =
              std::atan2(cl.pos.y - view.pos.y, cl.pos.x - view.pos.x);
          pb::Vec3 hop;
          if (!FindDungeonSeamHop(probe_map, view.phase_mask, view.pos,
                                  base_angle, cl.dist_yd, dungeon_nav, &hop)) {
            continue;
          }
          const pb::Direction dir = DirFrom(view.pos, cl.pos.x, cl.pos.y);
          if (dir == pb::DIRECTION_UNSPECIFIED || dir == pb::HERE ||
              (seen_dirs & DirBit(dir)) != 0) {
            continue;
          }
          seen_dirs |= DirBit(dir);
          out->push_back({SpotId(view.map_id, cl.pos.x, cl.pos.y), dir,
                          cl.dist_yd, cl.label, cl.pos});
        }
        if (!out->empty()) {
          return;
        }
      }
    }
  }
  for (const float angle : ExploreRayAngles()) {
    if (out->size() >= kMaxSpots) {
      break;
    }
    for (const float ray_dist : ExploreRayDistancesYd()) {
      const float tx = view.pos.x + ray_dist * std::cos(angle);
      const float ty = view.pos.y + ray_dist * std::sin(angle);
      const float tz =
          probe_map->GetHeight(view.phase_mask, tx, ty, view.pos.z + 10.0f);
      if (tz == INVALID_HEIGHT || tz == VMAP_INVALID_HEIGHT_VALUE) {
        continue;
      }
      PathGenerator path(path_owner);
      path.CalculatePath(tx, ty, tz);
      constexpr uint32_t kOk =
          PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
      if ((path.GetPathType() & ~kOk) != 0) {
        continue;
      }
      const G3D::Vector3& end = path.GetActualEndPosition();
      const float dist = Dist(view.pos, end.x, end.y, end.z);
      const pb::Direction dir = DirFrom(view.pos, end.x, end.y);
      if (dist < kMinTravelDistYd || (seen_dirs & DirBit(dir)) != 0) {
        continue;
      }
      seen_dirs |= DirBit(dir);
      out->push_back({SpotId(view.map_id, end.x, end.y),
                      dir,
                      dist,
                      "",
                      {end.x, end.y, end.z}});
      break;
    }
  }
}

void FillPlaces(const WorldPlaces& world,
                const Viewpoint& view,
                PositionFacts* out) {
  const uint32_t class_bit = 1u << view.class_id;
  const std::string trainer_label =
      base::StrCat({base::ToLowerASCII(ClassName(view.class_id)), " trainer"});

  struct Ranked {
    const WorldPlaces::Hub* hub;
    float dist_yd;
  };
  std::vector<Ranked> in_range;
  for (const WorldPlaces::Hub& hub : world.hubs()) {
    if (hub.map_id != view.map_id ||
        (view.alliance ? !hub.for_alliance : !hub.for_horde)) {
      continue;
    }
    const float dist = Dist(view.pos, hub.pos.x, hub.pos.y, hub.pos.z);
    if (dist <= kMinimapRangeYd + kMinimapMarginYd) {
      pb::Place& mp = out->minimap_hubs.emplace_back();
      mp.name = hub.name;
      mp.rel.position = hub.pos;
      mp.kind = hub.has_innkeeper ? pb::Place::TOWN : pb::Place::CAMP;
      mp.known = true;
    }
    if (dist >= kMinTravelDistYd && dist <= kMaxTravelDistYd) {
      in_range.push_back({&hub, dist});
    }
  }
  std::sort(
      in_range.begin(), in_range.end(),
      [](const Ranked& a, const Ranked& b) { return a.dist_yd < b.dist_yd; });

  // The nearest hub of each kind the bot could need.
  std::vector<const WorldPlaces::Hub*> chosen;
  const auto pick_first = [&](auto pred) {
    for (const Ranked& r : in_range) {
      if (pred(*r.hub)) {
        if (std::find(chosen.begin(), chosen.end(), r.hub) == chosen.end()) {
          chosen.push_back(r.hub);
        }
        return;
      }
    }
  };
  pick_first([](const WorldPlaces::Hub& h) { return h.has_innkeeper; });
  pick_first(
      [](const WorldPlaces::Hub& h) { return h.has_vendor || h.has_repair; });
  pick_first([&](const WorldPlaces::Hub& h) {
    return (h.class_trainer_mask & class_bit) != 0;
  });
  if (view.level >= 20) {
    pick_first([](const WorldPlaces::Hub& h) { return h.has_riding_trainer; });
  }
  pick_first([](const WorldPlaces::Hub& h) { return h.has_flight_master; });
  pick_first([](const WorldPlaces::Hub& h) { return h.has_quests; });
  pick_first([&](const WorldPlaces::Hub& h) {
    return h.has_quests && h.min_quest_level > 0 &&
           h.min_quest_level <= view.level + 3u &&
           h.max_quest_level + 3u >= view.level;
  });

  const GraveyardStruct* grave = sGraveyard->GetClosestGraveyard(
      view.map_id, view.pos.x, view.pos.y, view.pos.z,
      view.alliance ? TEAM_ALLIANCE : TEAM_HORDE, view.area_id, view.zone_id,
      view.class_id == CLASS_DEATH_KNIGHT);
  if (grave && grave->Map != view.map_id) {
    grave = nullptr;
  }
  bool grave_merged = false;

  for (const WorldPlaces::Hub* hub : chosen) {
    std::vector<std::string> services;
    if (hub->has_innkeeper) {
      services.push_back("innkeeper");
    }
    if (hub->has_vendor) {
      services.push_back("vendor");
    }
    if (hub->has_repair) {
      services.push_back("repair");
    }
    if ((hub->class_trainer_mask & class_bit) != 0) {
      services.push_back(trainer_label);
    }
    if (hub->has_riding_trainer) {
      services.push_back("riding trainer");
    }
    if (hub->has_flight_master) {
      services.push_back("flight master");
    }
    if (hub->has_quests) {
      if (hub->min_quest_level > 0 &&
          hub->max_quest_level >= hub->min_quest_level) {
        services.push_back(
            hub->min_quest_level == hub->max_quest_level
                ? Acore::StringFormat("quests lvl {}", hub->min_quest_level)
                : Acore::StringFormat("quests lvl {}-{}", hub->min_quest_level,
                                      hub->max_quest_level));
      } else {
        services.push_back("quests");
      }
    }
    if (grave && std::hypot(grave->x - hub->pos.x, grave->y - hub->pos.y) <=
                     kGraveyardMergeYd) {
      services.push_back("graveyard");
      grave_merged = true;
    }
    out->places.push_back({hub->id, hub->name, base::JoinString(services, ", "),
                           DirFrom(view.pos, hub->pos.x, hub->pos.y),
                           Dist(view.pos, hub->pos.x, hub->pos.y, hub->pos.z),
                           hub->pos});
  }

  if (grave && !grave_merged) {
    const float dist = Dist(view.pos, grave->x, grave->y, grave->z);
    if (dist >= kMinTravelDistYd && dist <= kMaxTravelDistYd) {
      uint32_t zone = 0;
      uint32_t area = 0;
      sMapMgr->GetZoneAndAreaId(view.phase_mask, zone, area, view.map_id,
                                grave->x, grave->y, grave->z);
      std::string name = AreaName(area != 0 ? area : zone);
      name = name.empty() ? "Graveyard" : name + " Graveyard";
      out->places.push_back({PlaceId(view.map_id, grave->x, grave->y),
                             std::move(name),
                             "graveyard",
                             DirFrom(view.pos, grave->x, grave->y),
                             dist,
                             {grave->x, grave->y, grave->z}});
    }
  }

  for (pb::OptionFacts::Place& travel_place :
       BuildOpenWorldTravelPlaces(view.map_id, view.pos, view.alliance,
                                  world.cross_map_edges(), kMaxTravelDistYd)) {
    out->places.push_back(std::move(travel_place));
  }
}

}  // namespace

Viewpoint ViewpointOf(Player* bot) {
  Viewpoint view;
  view.map_id = bot->GetMapId();
  view.zone_id = bot->GetZoneId();
  view.area_id = bot->GetAreaId();
  view.phase_mask = bot->GetPhaseMask();
  view.pos = {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
  view.level = bot->GetLevel();
  view.class_id = bot->getClass();
  view.alliance = bot->GetTeamId() == TEAM_ALLIANCE;
  return view;
}

std::string WorldPlaces::SpotBucket::Label() const {
  std::string out = base::JoinString(mobs, ", ");
  if (min_level > 0) {
    if (!out.empty()) {
      out += ", ";
    }
    out += min_level == max_level
               ? Acore::StringFormat("level {}", min_level)
               : Acore::StringFormat("level {}-{}", min_level, max_level);
  }
  return out;
}

WorldPlaces::WorldPlaces() = default;

WorldPlaces::~WorldPlaces() = default;

std::unique_ptr<WorldPlaces> WorldPlaces::Build() {
  std::unique_ptr<WorldPlaces> world(new WorldPlaces());
  // Hash index `(map_id|name) -> indices in world->hubs_` avoids O(N*M) scans
  // while merging nearby service/quest-giver NPCs.
  std::unordered_map<std::string, std::vector<size_t>> hubs_by_key;
  std::vector<std::unordered_map<uint32_t, uint32_t>> hub_quests;
  for (const auto& [guid, data] : sObjectMgr->GetAllCreatureData()) {
    const CreatureTemplate* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
    if (!tmpl || tmpl->Name.starts_with("[DND]")) {
      continue;
    }
    const uint32_t display_id =
        tmpl->Models.empty() ? 0 : tmpl->Models[0].CreatureDisplayID;
    if (HiddenFromPlayers(
            (tmpl->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER) != 0,
            display_id)) {
      continue;
    }
    const uint32_t map_id = data.mapid;
    if (map_id != 0 && map_id != 1 && map_id != 530 && map_id != 571) {
      const MapEntry* me = sMapStore.LookupEntry(map_id);
      // Match ObjectMgr::ChooseCreatureFlags: non-zero per-spawn unit_flags
      // replaces creature_template.unit_flags.
      const uint32_t unit_flags =
          data.unit_flags ? data.unit_flags : tmpl->unit_flags;
      if (me && me->IsDungeon() && tmpl->type != CREATURE_TYPE_CRITTER &&
          (unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE |
                         UNIT_FLAG_IMMUNE_TO_PC)) == 0) {
        const bool is_boss =
            (tmpl->flags_extra & CREATURE_FLAG_EXTRA_INSTANCE_BIND) != 0 ||
            tmpl->rank == CREATURE_ELITE_WORLDBOSS;
        world->dungeon_spawns_[map_id].push_back(
            {guid,
             tmpl->Name,
             {data.posX, data.posY, data.posZ},
             tmpl->faction,
             data.phaseMask,
             data.spawnMask,
             is_boss});
      }
      continue;  // Continents only for open-world grind buckets/hubs.
    }
    uint32_t zone_id = 0;
    uint32_t area_id = 0;
    sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, zone_id, area_id, map_id,
                              data.posX, data.posY, data.posZ);
    if (zone_id == 0) {
      continue;
    }

    // A grindable mob (the filter of TravelMgr::PrepareDestinationCache).
    if (tmpl->npcflag == 0 && tmpl->lootid != 0 &&
        tmpl->maxlevel - tmpl->minlevel < 3 && data.spawntimesecs < 1000 &&
        tmpl->rank == 0 &&
        // The same raw bits as TravelMgr.cpp (256: immune to players).
        (tmpl->unit_flags & (256 | 4096)) == 0) {
      SpotBucket& b =
          world->buckets_[BucketKey(map_id, data.posX, data.posY, data.posZ)];
      b.map_id = map_id;
      b.zone_id = zone_id;
      const float n = static_cast<float>(b.count);
      b.centroid.x = (b.centroid.x * n + data.posX) / (n + 1.0f);
      b.centroid.y = (b.centroid.y * n + data.posY) / (n + 1.0f);
      b.centroid.z = (b.centroid.z * n + data.posZ) / (n + 1.0f);
      ++b.count;
      if (b.min_level == 0 || tmpl->minlevel < b.min_level) {
        b.min_level = tmpl->minlevel;
      }
      b.max_level = std::max<uint32_t>(b.max_level, tmpl->maxlevel);
      if (b.mobs.size() < 3) {
        std::string noun = MobNoun(tmpl);
        if (!noun.empty() &&
            std::find(b.mobs.begin(), b.mobs.end(), noun) == b.mobs.end()) {
          b.mobs.push_back(std::move(noun));
        }
      }
      continue;
    }

    // A service or quest-giver NPC, for Go to <place>.
    constexpr uint32_t kHubFlags =
        UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR |
        UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_TRAINER |
        UNIT_NPC_FLAG_TRAINER_CLASS | UNIT_NPC_FLAG_QUESTGIVER;
    if ((tmpl->npcflag & kHubFlags) == 0) {
      continue;
    }
    const FactionTemplateEntry* faction =
        sFactionTemplateStore.LookupEntry(tmpl->faction);
    if (!faction) {
      continue;
    }
    const bool for_alliance =
        (faction->hostileMask & FACTION_MASK_ALLIANCE) == 0;
    const bool for_horde = (faction->hostileMask & FACTION_MASK_HORDE) == 0;
    if (!for_alliance && !for_horde) {
      continue;
    }
    uint32_t class_mask = 0;
    bool has_riding_trainer = false;
    if ((tmpl->npcflag &
         (UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS)) != 0) {
      if (const Trainer::Trainer* trainer = sObjectMgr->GetTrainer(tmpl->Entry);
          trainer) {
        if (trainer->GetTrainerType() == Trainer::Type::Class &&
            trainer->GetTrainerRequirement() < 32) {
          class_mask = 1u << trainer->GetTrainerRequirement();
        } else if (trainer->GetTrainerType() == Trainer::Type::Mount) {
          has_riding_trainer = true;
        }
      }
    }
    const bool has_inn = (tmpl->npcflag & UNIT_NPC_FLAG_INNKEEPER) != 0;
    const bool has_vendor = (tmpl->npcflag & UNIT_NPC_FLAG_VENDOR) != 0;
    const bool has_repair = (tmpl->npcflag & UNIT_NPC_FLAG_REPAIR) != 0;
    const bool has_fm = (tmpl->npcflag & UNIT_NPC_FLAG_FLIGHTMASTER) != 0;
    const bool has_quests = (tmpl->npcflag & UNIT_NPC_FLAG_QUESTGIVER) != 0;
    if (!has_inn && !has_vendor && !has_repair && !has_fm && !has_quests &&
        !has_riding_trainer && class_mask == 0) {
      continue;
    }

    const uint32_t eff_area = area_id != 0 ? area_id : zone_id;
    std::string name = AreaName(eff_area);
    if (name.empty()) {
      name = AreaName(zone_id);
    }
    if (name.empty()) {
      name = tmpl->Name;
    }
    const std::string hub_key = Acore::StringFormat("{}|{}", map_id, name);
    size_t hub_idx = world->hubs_.size();
    auto& bucket_indices = hubs_by_key[hub_key];
    for (size_t idx : bucket_indices) {
      const Hub& existing = world->hubs_[idx];
      if (existing.for_alliance == for_alliance &&
          existing.for_horde == for_horde &&
          std::hypot(existing.pos.x - data.posX, existing.pos.y - data.posY) <=
              kHubMergeRadiusYd) {
        hub_idx = idx;
        break;
      }
    }
    if (hub_idx == world->hubs_.size()) {
      Hub& created = world->hubs_.emplace_back();
      created.map_id = map_id;
      created.name = name;
      created.for_alliance = for_alliance;
      created.for_horde = for_horde;
      created.pos = {data.posX, data.posY, data.posZ};
      bucket_indices.push_back(hub_idx);
      hub_quests.emplace_back();
    }
    Hub* hub = &world->hubs_[hub_idx];
    hub->has_innkeeper |= has_inn;
    hub->has_vendor |= has_vendor;
    hub->has_repair |= has_repair;
    hub->has_flight_master |= has_fm;
    hub->has_riding_trainer |= has_riding_trainer;
    hub->has_quests |= has_quests;
    if (has_quests) {
      const auto bounds =
          sObjectMgr->GetCreatureQuestRelationBounds(tmpl->Entry);
      for (auto it = bounds.first; it != bounds.second; ++it) {
        if (const Quest* q = sObjectMgr->GetQuestTemplate(it->second);
            q && q->GetQuestLevel() > 0 && !q->IsDaily() && !q->IsWeekly()) {
          hub_quests[hub_idx].emplace(
              q->GetQuestId(), static_cast<uint32_t>(q->GetQuestLevel()));
        }
      }
    }
    hub->class_trainer_mask |= class_mask;
    if (has_inn && !hub->pos_from_innkeeper) {
      hub->pos = {data.posX, data.posY, data.posZ};
      hub->pos_from_innkeeper = true;
    }
  }
  // Ids from the final position, and deduplicated quest-level ranges: full
  // [min, max] for small hubs (< 4 unique quests) and IQR (25th-75th
  // percentile) for larger hubs so one high-level seasonal/breadcrumb quest
  // does not inflate a starter town's level range.
  for (size_t i = 0; i < world->hubs_.size(); ++i) {
    Hub& hub = world->hubs_[i];
    hub.id = PlaceId(hub.map_id, hub.pos.x, hub.pos.y);
    std::vector<uint32_t> qlevels;
    qlevels.reserve(hub_quests[i].size());
    for (const auto& [qid, ql] : hub_quests[i]) {
      qlevels.push_back(ql);
    }
    if (!qlevels.empty()) {
      std::sort(qlevels.begin(), qlevels.end());
      const size_t n = qlevels.size();
      if (n < 4) {
        hub.min_quest_level = qlevels.front();
        hub.max_quest_level = qlevels.back();
      } else {
        hub.min_quest_level = qlevels[n / 4];
        hub.max_quest_level = qlevels[(3 * (n - 1)) / 4];
      }
    }
  }

  // World travel graph: legitimate AreaTriggerTeleports (dungeon/raid
  // entrances & exits, Dark Portal, Deeprun Tram, Darnassus <-> Rut'theran
  // Village), faction-compatible portal GameObjects (Shattrath/Dalaran city
  // portals, Orb of Translocation), and continent MotionTransports (boats &
  // zeppelins).
  const uint32_t locale = sWorld->GetDefaultDbcLocale();
  const auto location_area_name = [&](uint32_t map_id, float x, float y,
                                      float z, const MapEntry* map_entry) {
    uint32_t z_id = 0;
    uint32_t a_id = 0;
    sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, z_id, a_id, map_id, x, y, z);
    if (std::string n = AreaName(a_id); !n.empty()) {
      return n;
    }
    if (std::string n = AreaName(z_id); !n.empty()) {
      return n;
    }
    const char* mname = map_entry ? map_entry->name[locale] : nullptr;
    return (mname && *mname) ? std::string(mname) : std::string();
  };
  for (const auto& [trigger_id, at_tp] :
       sObjectMgr->GetAllAreaTriggerTeleports()) {
    const AreaTrigger* at = sObjectMgr->GetAreaTrigger(trigger_id);
    if (!at) {
      continue;
    }
    if (at->map == at_tp.target_mapId &&
        Dist({at->x, at->y, at->z}, at_tp.target_X, at_tp.target_Y,
             at_tp.target_Z) < kSameMapPortalMinDistYd) {
      continue;
    }
    const MapEntry* from_map = sMapStore.LookupEntry(at->map);
    const MapEntry* to_map = sMapStore.LookupEntry(at_tp.target_mapId);
    if (!from_map || !to_map || from_map->IsBattlegroundOrArena() ||
        to_map->IsBattlegroundOrArena()) {
      continue;
    }
    const char* to_name = to_map->name[locale];
    const std::string dest_name =
        location_area_name(at_tp.target_mapId, at_tp.target_X, at_tp.target_Y,
                           at_tp.target_Z, to_map);
    std::string label;
    if (to_map->IsDungeon()) {
      label = (to_name && *to_name) ? std::string(to_name) + " entrance"
                                    : "instance entrance";
    } else if (from_map->IsDungeon()) {
      label = "instance exit";
    } else if (!dest_name.empty()) {
      label = "portal to " + dest_name;
    } else {
      label = (to_name && *to_name) ? std::string("portal to ") + to_name
                                    : "portal";
    }
    world->cross_map_edges_.push_back(CrossMapEdge{
        .kind = TravelLegKind::kAreaTrigger,
        .from_map_id = at->map,
        .from_pos = {at->x, at->y, at->z},
        .to_map_id = at_tp.target_mapId,
        .to_pos = {at_tp.target_X, at_tp.target_Y, at_tp.target_Z},
        .trigger_id = trigger_id,
        .object_entry = 0,
        .label = std::move(label),
        .dest_name = dest_name,
        .dest_continent = (at->map != at_tp.target_mapId && to_name && *to_name)
                              ? std::string(to_name)
                              : "",
        .for_alliance = true,
        .for_horde = true,
        .involves_instance = from_map->IsDungeon() || to_map->IsDungeon(),
    });
  }
  for (const auto& [spawn_id, data] : sObjectMgr->GetAllGOData()) {
    if (data.phaseMask != PHASEMASK_NORMAL) {
      continue;
    }
    const GameObjectTemplate* go_tmpl =
        sObjectMgr->GetGameObjectTemplate(data.id);
    if (!go_tmpl || go_tmpl->type != GAMEOBJECT_TYPE_SPELLCASTER ||
        go_tmpl->spellcaster.spellId == 0 || go_tmpl->spellcaster.partyOnly ||
        go_tmpl->name.empty()) {
      continue;
    }
    const SpellInfo* spell =
        sSpellMgr->GetSpellInfo(go_tmpl->spellcaster.spellId);
    if (!spell) {
      continue;
    }
    const SpellTargetPosition* tp = nullptr;
    for (uint8_t i = 0; i < MAX_SPELL_EFFECTS; ++i) {
      if (spell->Effects[i].Effect == SPELL_EFFECT_TELEPORT_UNITS) {
        tp = sSpellMgr->GetSpellTargetPosition(spell->Id,
                                               static_cast<SpellEffIndex>(i));
        if (tp) {
          break;
        }
      }
    }
    if (!tp) {
      continue;
    }
    if (tp->target_mapId == data.mapid &&
        Dist({data.posX, data.posY, data.posZ}, tp->target_X, tp->target_Y,
             tp->target_Z) < kSameMapPortalMinDistYd) {
      continue;
    }
    const MapEntry* from_map = sMapStore.LookupEntry(data.mapid);
    const MapEntry* to_map = sMapStore.LookupEntry(tp->target_mapId);
    if (!from_map || !to_map || from_map->IsBattlegroundOrArena() ||
        to_map->IsBattlegroundOrArena()) {
      continue;
    }
    bool for_alliance = true;
    bool for_horde = true;
    if (const GameObjectTemplateAddon* addon =
            sObjectMgr->GetGameObjectTemplateAddon(data.id);
        addon && addon->faction != 0) {
      if (const FactionTemplateEntry* ft =
              sFactionTemplateStore.LookupEntry(addon->faction)) {
        for_alliance = (ft->hostileMask & FACTION_MASK_ALLIANCE) == 0;
        for_horde = (ft->hostileMask & FACTION_MASK_HORDE) == 0;
      }
    }
    if (!for_alliance && !for_horde) {
      continue;
    }
    const char* to_name = to_map->name[locale];
    const std::string dest_name = location_area_name(
        tp->target_mapId, tp->target_X, tp->target_Y, tp->target_Z, to_map);
    world->cross_map_edges_.push_back(CrossMapEdge{
        .kind = TravelLegKind::kPortalObject,
        .from_map_id = data.mapid,
        .from_pos = {data.posX, data.posY, data.posZ},
        .to_map_id = tp->target_mapId,
        .to_pos = {tp->target_X, tp->target_Y, tp->target_Z},
        .trigger_id = 0,
        .object_entry = data.id,
        .label = go_tmpl->name,
        .dest_name = dest_name.empty() ? go_tmpl->name : dest_name,
        .dest_continent =
            (data.mapid != tp->target_mapId && to_name && *to_name)
                ? std::string(to_name)
                : "",
        .for_alliance = for_alliance,
        .for_horde = for_horde,
        .involves_instance = from_map->IsDungeon() || to_map->IsDungeon(),
    });
  }

  // Continent boats and zeppelins (MotionTransports): extract stop-to-stop
  // legs from each active transport's keyFrames and infer faction safety from
  // nearby hubs (Alliance docks, Horde zeppelin towers, neutral ports).
  const auto infer_dock_factions = [&](uint32_t map_id, const pb::Vec3& pos,
                                       bool* alliance_ok, bool* horde_ok) {
    bool saw_ally = false;
    bool saw_horde = false;
    for (const Hub& hub : world->hubs_) {
      if (hub.map_id != map_id ||
          Dist(pos, hub.pos.x, hub.pos.y, hub.pos.z) > 180.0f) {
        continue;
      }
      saw_ally |= hub.for_alliance;
      saw_horde |= hub.for_horde;
    }
    if (saw_ally && !saw_horde) {
      *horde_ok = false;
    } else if (saw_horde && !saw_ally) {
      *alliance_ok = false;
    }
  };
  std::vector<uint32_t> seen_transport_entries;
  for (uint32_t continent_map : {0u, 1u, 530u, 571u}) {
    Map* cmap = sMapMgr->CreateBaseMap(continent_map);
    if (!cmap) {
      continue;
    }
    for (Transport* t : cmap->GetAllTransports()) {
      MotionTransport* mt = dynamic_cast<MotionTransport*>(t);
      if (!mt) {
        continue;
      }
      const uint32_t entry = mt->GetEntry();
      if (entry == 0 || std::find(seen_transport_entries.begin(),
                                  seen_transport_entries.end(),
                                  entry) != seen_transport_entries.end()) {
        continue;
      }
      seen_transport_entries.push_back(entry);
      const TransportTemplate* tmpl = mt->GetTransportTemplate();
      if (!tmpl || tmpl->inInstance || tmpl->keyFrames.empty()) {
        continue;
      }
      struct TransportStop {
        uint32_t map_id = 0;
        pb::Vec3 pos;
      };
      std::vector<TransportStop> stops;
      for (const KeyFrame& kf : tmpl->keyFrames) {
        if (!kf.IsStopFrame() || !kf.Node) {
          continue;
        }
        const pb::Vec3 p{kf.Node->x, kf.Node->y, kf.Node->z};
        if (!stops.empty() && stops.back().map_id == kf.Node->mapid &&
            Dist(stops.back().pos, p.x, p.y, p.z) < 50.0f) {
          continue;
        }
        stops.push_back({kf.Node->mapid, p});
      }
      if (stops.size() >= 2 && stops.front().map_id == stops.back().map_id &&
          Dist(stops.front().pos, stops.back().pos.x, stops.back().pos.y,
               stops.back().pos.z) < 50.0f) {
        stops.pop_back();
      }
      if (stops.size() < 2) {
        continue;
      }
      bool base_alliance = true;
      bool base_horde = true;
      if (const GameObjectTemplateAddon* addon =
              sObjectMgr->GetGameObjectTemplateAddon(entry);
          addon && addon->faction != 0) {
        if (const FactionTemplateEntry* ft =
                sFactionTemplateStore.LookupEntry(addon->faction)) {
          base_alliance = (ft->hostileMask & FACTION_MASK_ALLIANCE) == 0;
          base_horde = (ft->hostileMask & FACTION_MASK_HORDE) == 0;
        }
      }
      for (size_t i = 0; i < stops.size(); ++i) {
        const TransportStop& from = stops[i];
        const TransportStop& to = stops[(i + 1) % stops.size()];
        if (from.map_id == to.map_id &&
            Dist(from.pos, to.pos.x, to.pos.y, to.pos.z) <
                kSameMapPortalMinDistYd) {
          continue;
        }
        const MapEntry* from_map = sMapStore.LookupEntry(from.map_id);
        const MapEntry* to_map = sMapStore.LookupEntry(to.map_id);
        if (!from_map || !to_map || from_map->IsBattlegroundOrArena() ||
            to_map->IsBattlegroundOrArena() || from_map->IsDungeon() ||
            to_map->IsDungeon()) {
          continue;
        }
        bool for_alliance = base_alliance;
        bool for_horde = base_horde;
        infer_dock_factions(from.map_id, from.pos, &for_alliance, &for_horde);
        infer_dock_factions(to.map_id, to.pos, &for_alliance, &for_horde);
        if (!for_alliance && !for_horde) {
          continue;
        }
        const char* to_name = to_map->name[locale];
        const std::string dest_name =
            location_area_name(to.map_id, to.pos.x, to.pos.y, to.pos.z, to_map);
        std::string label =
            dest_name.empty() ? "transport" : "transport to " + dest_name;
        world->cross_map_edges_.push_back(CrossMapEdge{
            .kind = TravelLegKind::kTransport,
            .from_map_id = from.map_id,
            .from_pos = from.pos,
            .to_map_id = to.map_id,
            .to_pos = to.pos,
            .trigger_id = 0,
            .object_entry = entry,
            .label = std::move(label),
            .dest_name = dest_name,
            .dest_continent = (from.map_id != to.map_id && to_name && *to_name)
                                  ? std::string(to_name)
                                  : "",
            .for_alliance = for_alliance,
            .for_horde = for_horde,
            .involves_instance = false,
        });
      }
    }
  }
  return world;
}

const WorldPlaces::SpotBucket* WorldPlaces::FindBucket(uint32_t map_id,
                                                       float x,
                                                       float y,
                                                       float z) const {
  const auto it = buckets_.find(BucketKey(map_id, x, y, z));
  return it == buckets_.end() ? nullptr : &it->second;
}

const std::vector<WorldPlaces::DungeonSpawn>* WorldPlaces::DungeonSpawns(
    uint32_t map_id) const {
  const auto it = dungeon_spawns_.find(map_id);
  return it == dungeon_spawns_.end() ? nullptr : &it->second;
}

bool FindDungeonSeamHop(Map* map,
                        uint32_t phase_mask,
                        const wlb::snapshot::Vec3& from,
                        float base_angle,
                        float max_dist_yd,
                        const DungeonNavConfig& dungeon_nav,
                        wlb::snapshot::Vec3* out_hop,
                        std::vector<wlb::snapshot::Vec3>* out_path) {
  if (!dungeon_nav.seam_bridge_enable || !map || !map->IsDungeon() ||
      !out_hop) {
    return false;
  }
  const dtNavMeshQuery* nav_query =
      map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
  if (!nav_query) {
    return false;
  }
  const float max_drop = (!std::isfinite(dungeon_nav.seam_bridge_max_drop_yd) ||
                          dungeon_nav.seam_bridge_max_drop_yd < 0.0f)
                             ? 0.0f
                             : dungeon_nav.seam_bridge_max_drop_yd;
  const float max_climb =
      (!std::isfinite(dungeon_nav.seam_bridge_max_climb_yd) ||
       dungeon_nav.seam_bridge_max_climb_yd < 0.0f)
          ? 0.0f
          : dungeon_nav.seam_bridge_max_climb_yd;
  const float height_probe_z = std::max(4.0f, max_climb + 1.5f);

  dtQueryFilter filter;
  filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
  filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);

  static constexpr float kSeamAngles[] = {0.0f, 0.35f, -0.35f, 0.7f, -0.7f};
  static constexpr float kSeamDists[] = {6.0f, 8.0f, 10.0f, 14.0f, 18.0f};
  for (float d : kSeamDists) {
    const float step_d = std::min(d, max_dist_yd);
    if (step_d < 3.0f) {
      if (d >= max_dist_yd) {
        break;
      }
      continue;
    }
    const int samples = std::max(2, static_cast<int>(std::ceil(step_d / 2.0f)));
    for (float off : kSeamAngles) {
      const float a = base_angle + off;
      const float hx = from.x + step_d * std::cos(a);
      const float hy = from.y + step_d * std::sin(a);
      // Track continuous walkable terrain and dual-height LOS (+0.6 yd low
      // railing, +1.5 yd torso/wall) piecewise from `from` so overhangs above
      // (hx, hy) do not falsely reject descending stairs and convex stair
      // crests do not clip a single chord while walls/railings/pits still fail.
      std::vector<pb::Vec3> polyline;
      if (out_path) {
        polyline.reserve(static_cast<size_t>(samples) + 2);
        polyline.push_back(from);
      }
      float prev_x = from.x;
      float prev_y = from.y;
      float prev_z = from.z;
      bool terrain_ok = true;
      for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const float sx = from.x + (hx - from.x) * t;
        const float sy = from.y + (hy - from.y) * t;
        const float sz =
            map->GetHeight(phase_mask, sx, sy, prev_z + height_probe_z);
        if (sz == INVALID_HEIGHT || sz == VMAP_INVALID_HEIGHT_VALUE ||
            sz - prev_z > max_climb || prev_z - sz > max_drop ||
            sz - from.z > max_climb || from.z - sz > max_drop) {
          terrain_ok = false;
          break;
        }
        if (!map->isInLineOfSight(prev_x, prev_y, prev_z + 0.6f, sx, sy,
                                  sz + 0.6f, phase_mask, LINEOFSIGHT_ALL_CHECKS,
                                  VMAP::ModelIgnoreFlags::Nothing) ||
            !map->isInLineOfSight(prev_x, prev_y, prev_z + 1.5f, sx, sy,
                                  sz + 1.5f, phase_mask, LINEOFSIGHT_ALL_CHECKS,
                                  VMAP::ModelIgnoreFlags::Nothing)) {
          terrain_ok = false;
          break;
        }
        if (out_path && i < samples) {
          polyline.push_back({sx, sy, sz});
        }
        prev_x = sx;
        prev_y = sy;
        prev_z = sz;
      }
      if (!terrain_ok) {
        continue;
      }
      // Confirm the landing point lies on a walkable navmesh polygon so the
      // bot never steps off-mesh or into geometry.
      const float point[3] = {hy, prev_z, hx};  // Detour uses (y, z, x).
      const float extents[3] = {1.5f, 2.5f, 1.5f};
      dtPolyRef poly_ref = INVALID_POLYREF;
      float closest[3] = {0.0f, 0.0f, 0.0f};
      if (!dtStatusSucceed(nav_query->findNearestPoly(point, extents, &filter,
                                                      &poly_ref, closest)) ||
          poly_ref == INVALID_POLYREF) {
        continue;
      }
      if (std::hypot(closest[2] - hx, closest[0] - hy) > 1.5f ||
          std::abs(closest[1] - prev_z) > 2.0f) {
        continue;
      }
      // Re-validate drop/climb bounds and dual-height LOS to the snapped
      // polygon point so snapping cannot exceed bounds or cross a thin wall.
      const float snap_x = closest[2];
      const float snap_y = closest[0];
      const float snap_z = closest[1];
      const float snap_dz = snap_z - from.z;
      if (snap_dz < -max_drop || snap_dz > max_climb) {
        continue;
      }
      if (!map->isInLineOfSight(
              hx, hy, prev_z + 0.6f, snap_x, snap_y, snap_z + 0.6f, phase_mask,
              LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing) ||
          !map->isInLineOfSight(
              hx, hy, prev_z + 1.5f, snap_x, snap_y, snap_z + 1.5f, phase_mask,
              LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing)) {
        continue;
      }
      *out_hop = {snap_x, snap_y, snap_z};
      if (out_path) {
        // Include the final terrain sample `(hx, hy, prev_z)` when `snap`
        // shifted materially so every spline segment follows a LOS-checked
        // edge (`s_{samples-1} -> (hx, hy, prev_z) -> snap`).
        if (Dist({hx, hy, prev_z}, snap_x, snap_y, snap_z) > 0.1f) {
          polyline.push_back({hx, hy, prev_z});
        }
        polyline.push_back(*out_hop);
        *out_path = std::move(polyline);
      }
      return true;
    }
    if (d >= max_dist_yd) {
      break;
    }
  }
  return false;
}

PositionFacts ComputePositionFacts(const WorldPlaces& world,
                                   const Viewpoint& view,
                                   const WorldObject* path_owner,
                                   const DungeonNavConfig& dungeon_nav) {
  PositionFacts facts;
  FillSpots(world, view, path_owner, dungeon_nav, &facts.spots);
  FillPlaces(world, view, &facts);
  RefreshFrom(view.pos, &facts);
  return facts;
}

}  // namespace llm_brain
