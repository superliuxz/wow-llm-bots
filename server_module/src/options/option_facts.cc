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
#include "G3D/Vector3.h"
#include "GameGraveyard.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Position.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "TravelMgr.h"
#include "UnitDefines.h"
#include "World.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
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

  // None in this zone at this level: the zone's buckets within 6 levels.
  if (candidates.empty()) {
    for (const auto& [key, b] : world.buckets()) {
      if (b.map_id != view.map_id || (!in_city && b.zone_id != view.zone_id) ||
          b.count < 2) {
        continue;
      }
      if (b.min_level > view.level + 6u ||
          (view.level > 6 && b.max_level + 6u < view.level)) {
        continue;
      }
      add_candidate(b.centroid.x, b.centroid.y, b.centroid.z, b.Label());
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

  // No grind spot at all (an instance, a city): walkable rays along 8 compass
  // directions at multiple distances on the bot's actual map (including
  // dungeon instances) so narrow/sloped corridors yield a valid Explore option.
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
  pick_first([](const WorldPlaces::Hub& h) { return h.has_flight_master; });

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
    if (hub->has_flight_master) {
      services.push_back("flight master");
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
      continue;  // Continents only.
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

    // A service NPC, for Go to <place>.
    constexpr uint32_t kHubFlags =
        UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR |
        UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_TRAINER |
        UNIT_NPC_FLAG_TRAINER_CLASS;
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
    if ((tmpl->npcflag &
         (UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS)) != 0) {
      if (const Trainer::Trainer* trainer = sObjectMgr->GetTrainer(tmpl->Entry);
          trainer && trainer->GetTrainerType() == Trainer::Type::Class &&
          trainer->GetTrainerRequirement() < 32) {
        class_mask = 1u << trainer->GetTrainerRequirement();
      }
    }
    const bool has_inn = (tmpl->npcflag & UNIT_NPC_FLAG_INNKEEPER) != 0;
    const bool has_vendor = (tmpl->npcflag & UNIT_NPC_FLAG_VENDOR) != 0;
    const bool has_repair = (tmpl->npcflag & UNIT_NPC_FLAG_REPAIR) != 0;
    const bool has_fm = (tmpl->npcflag & UNIT_NPC_FLAG_FLIGHTMASTER) != 0;
    if (!has_inn && !has_vendor && !has_repair && !has_fm && class_mask == 0) {
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
    Hub* hub = nullptr;
    for (Hub& existing : world->hubs_) {
      if (existing.map_id == map_id && existing.name == name &&
          existing.for_alliance == for_alliance &&
          existing.for_horde == for_horde &&
          std::hypot(existing.pos.x - data.posX, existing.pos.y - data.posY) <=
              kHubMergeRadiusYd) {
        hub = &existing;
        break;
      }
    }
    if (!hub) {
      hub = &world->hubs_.emplace_back();
      hub->map_id = map_id;
      hub->name = name;
      hub->for_alliance = for_alliance;
      hub->for_horde = for_horde;
      hub->pos = {data.posX, data.posY, data.posZ};
    }
    hub->has_innkeeper |= has_inn;
    hub->has_vendor |= has_vendor;
    hub->has_repair |= has_repair;
    hub->has_flight_master |= has_fm;
    hub->class_trainer_mask |= class_mask;
    if (has_inn && !hub->pos_from_innkeeper) {
      hub->pos = {data.posX, data.posY, data.posZ};
      hub->pos_from_innkeeper = true;
    }
  }
  // Ids from the final position (the innkeeper's, when the hub has one).
  for (Hub& hub : world->hubs_) {
    hub.id = PlaceId(hub.map_id, hub.pos.x, hub.pos.y);
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

PositionFacts ComputePositionFacts(const WorldPlaces& world,
                                   const Viewpoint& view,
                                   const WorldObject* path_owner) {
  PositionFacts facts;
  FillSpots(world, view, path_owner, &facts.spots);
  FillPlaces(world, view, &facts);
  RefreshFrom(view.pos, &facts);
  return facts;
}

}  // namespace llm_brain
