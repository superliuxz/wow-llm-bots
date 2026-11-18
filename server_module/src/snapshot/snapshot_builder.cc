// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/snapshot_builder.h"
#include <math.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Bag.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "GameGraveyard.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "IVMapMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "LootMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Position.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "ThreatManager.h"
#include "Trainer.h"
#include "TravelMgr.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "World.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "options/option_executor.h"
#include "options/options.h"
#include "proto/snapshot.h"
#include "snapshot/nearby_builder.h"
#include "snapshot/perception.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr uint32_t kSpellCategoryFood = 11;
constexpr uint32_t kSpellCategoryDrink = 59;
constexpr float kMinimapRangeYd = 150.0f;
constexpr float kMinTravelDistYd = 25.0f;
constexpr float kMaxTravelDistYd = 2500.0f;
constexpr float kHubMergeRadiusYd = 250.0f;

pb::Self::PowerType ToPowerType(Powers power) {
  switch (power) {
    case POWER_MANA:
      return pb::Self::MANA;
    case POWER_RAGE:
      return pb::Self::RAGE;
    case POWER_FOCUS:
      return pb::Self::FOCUS;
    case POWER_ENERGY:
      return pb::Self::ENERGY;
    case POWER_RUNIC_POWER:
      return pb::Self::RUNIC_POWER;
    default:
      return pb::Self::POWER_TYPE_UNSPECIFIED;
  }
}

// "0x00000000000001FC": the raw GUID, as the client's combat log prints it.
std::string GuidString(ObjectGuid guid) {
  return Acore::StringFormat("0x{:016X}", guid.GetRawValue());
}

float Percent(uint32_t value, uint32_t max) {
  return max == 0 ? 0.0f : 100.0f * static_cast<float>(value) / max;
}

// The area's name in the server's DBC locale (what the zone text shows).
std::string AreaName(uint32_t area_id) {
  const AreaTableEntry* area = sAreaTableStore.LookupEntry(area_id);
  if (!area) {
    return "";
  }
  const char* name = area->area_name[sWorld->GetDefaultDbcLocale()];
  return name ? name : "";
}

void FillPointRelative(Player* bot,
                       float x,
                       float y,
                       float z,
                       pb::Relative* rel) {
  const float dx = x - bot->GetPositionX();
  const float dy = y - bot->GetPositionY();
  const float dz = z - bot->GetPositionZ();
  rel->dist_yd = bot->GetExactDist(x, y, z);
  rel->dir = CompassDirection(dx, dy);
  rel->height = HeightOf(dz);
  rel->position = {x, y, z};
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

std::string SlugId(std::string_view prefix, std::string_view text) {
  std::string out(prefix);
  bool prev_underscore = false;
  for (unsigned char c : text) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      prev_underscore = false;
    } else if (!prev_underscore && !out.ends_with(':')) {
      out.push_back('_');
      prev_underscore = true;
    }
  }
  while (out.ends_with('_')) {
    out.pop_back();
  }
  return out;
}

// One-time index of grind spot labels (matching sTravelMgr.GetLocsPerLevelCache
// 50-yd buckets) and friendly service hubs by subzone, built once on the world
// thread from sObjectMgr->GetAllCreatureData().
struct WorldPlacesCache {
  struct SpotBucket {
    uint32_t map_id = 0;
    uint32_t zone_id = 0;
    uint32_t min_level = 0;
    uint32_t max_level = 0;
    std::vector<std::string> mobs;
    pb::Vec3 centroid;
    uint32_t count = 0;

    std::string FormatLabel() const {
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
  };

  struct Hub {
    std::string id;
    uint32_t map_id = 0;
    uint32_t zone_id = 0;
    uint32_t area_id = 0;
    std::string name;
    bool for_alliance = false;
    bool for_horde = false;
    bool has_innkeeper = false;
    bool has_vendor = false;
    bool has_repair = false;
    bool has_flight_master = false;
    uint32_t class_trainer_mask = 0;
    pb::Vec3 pos;
    bool pos_from_innkeeper = false;
  };

  static uint64_t BucketKey(uint32_t map_id, float x, float y, float z) {
    const uint64_t rx =
        static_cast<uint16_t>(static_cast<int16_t>(std::lround(x / 50.0f)));
    const uint64_t ry =
        static_cast<uint16_t>(static_cast<int16_t>(std::lround(y / 50.0f)));
    const uint64_t rz =
        static_cast<uint16_t>(static_cast<int16_t>(std::lround(z / 50.0f)));
    return (static_cast<uint64_t>(map_id & 0xFFFFu) << 48) | (rx << 32) |
           (ry << 16) | rz;
  }

  std::unordered_map<uint64_t, SpotBucket> spot_buckets;
  std::vector<Hub> hubs;
};

const WorldPlacesCache& GetWorldPlacesCache() {
  static const WorldPlacesCache* cache = []() {
    auto* c = new WorldPlacesCache();
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
        continue;
      }
      uint32_t zone_id = 0;
      uint32_t area_id = 0;
      sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, zone_id, area_id, map_id,
                                data.posX, data.posY, data.posZ);
      if (zone_id == 0) {
        continue;
      }

      // Grindable mob bucket (same filter as
      // TravelMgr::PrepareDestinationCache).
      if (tmpl->npcflag == 0 && tmpl->lootid != 0 &&
          tmpl->maxlevel - tmpl->minlevel < 3 && data.spawntimesecs < 1000 &&
          tmpl->rank == 0 && (tmpl->unit_flags & (256 | 4096)) == 0) {
        const uint64_t key = WorldPlacesCache::BucketKey(map_id, data.posX,
                                                         data.posY, data.posZ);
        WorldPlacesCache::SpotBucket& b = c->spot_buckets[key];
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
        if (tmpl->maxlevel > b.max_level) {
          b.max_level = tmpl->maxlevel;
        }
        if (b.mobs.size() < 3) {
          const std::string noun = MobNoun(tmpl);
          if (!noun.empty() &&
              std::find(b.mobs.begin(), b.mobs.end(), noun) == b.mobs.end()) {
            b.mobs.push_back(noun);
          }
        }
        continue;
      }

      // Service NPCs for Go to <place>.
      constexpr uint32_t kHubFlags =
          UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_VENDOR |
          UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_FLIGHTMASTER |
          UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS;
      if ((tmpl->npcflag & kHubFlags) == 0) {
        continue;
      }
      const FactionTemplateEntry* faction =
          sFactionTemplateStore.LookupEntry(tmpl->faction);
      if (!faction) {
        continue;
      }
      const bool for_alliance = (faction->hostileMask & 2) == 0;
      const bool for_horde = (faction->hostileMask & 4) == 0;
      if (!for_alliance && !for_horde) {
        continue;
      }

      uint32_t class_mask = 0;
      if ((tmpl->npcflag &
           (UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS)) != 0) {
        if (const Trainer::Trainer* trainer =
                sObjectMgr->GetTrainer(tmpl->Entry);
            trainer && trainer->GetTrainerType() == Trainer::Type::Class &&
            trainer->GetTrainerRequirement() < 32) {
          class_mask = 1u << trainer->GetTrainerRequirement();
        }
      }
      const bool has_inn = (tmpl->npcflag & UNIT_NPC_FLAG_INNKEEPER) != 0;
      const bool has_vendor = (tmpl->npcflag & UNIT_NPC_FLAG_VENDOR) != 0;
      const bool has_repair = (tmpl->npcflag & UNIT_NPC_FLAG_REPAIR) != 0;
      const bool has_fm = (tmpl->npcflag & UNIT_NPC_FLAG_FLIGHTMASTER) != 0;
      if (!has_inn && !has_vendor && !has_repair && !has_fm &&
          class_mask == 0) {
        continue;
      }

      const uint32_t eff_area = area_id != 0 ? area_id : zone_id;
      std::string place_name = eff_area == 363 ? "The Den" : AreaName(eff_area);
      if (place_name.empty()) {
        place_name = AreaName(zone_id);
      }
      if (place_name.empty()) {
        place_name = tmpl->Name;
      }

      WorldPlacesCache::Hub* hub = nullptr;
      for (WorldPlacesCache::Hub& existing : c->hubs) {
        if (existing.map_id == map_id && existing.area_id == eff_area &&
            existing.for_alliance == for_alliance &&
            existing.for_horde == for_horde &&
            std::hypot(existing.pos.x - data.posX,
                       existing.pos.y - data.posY) <= kHubMergeRadiusYd) {
          hub = &existing;
          break;
        }
      }
      if (!hub) {
        hub = &c->hubs.emplace_back();
        hub->map_id = map_id;
        hub->zone_id = zone_id;
        hub->area_id = eff_area;
        hub->name = place_name;
        hub->id = SlugId("place:", place_name);
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
    return c;
  }();
  return *cache;
}

// Free slots in the backpack and general-purpose bags, the number the client's
// bag bar shows (special bags, like a herb bag or a quiver, don't count).
uint32_t FreeBagSlots(Player* bot) {
  uint32_t free = 0;
  for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END;
       ++slot) {
    if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) {
      ++free;
    }
  }
  for (uint8_t i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i) {
    const Bag* bag = bot->GetBagByPos(i);
    if (bag && bag->GetTemplate()->Class == ITEM_CLASS_CONTAINER &&
        bag->GetTemplate()->SubClass == ITEM_SUBCLASS_CONTAINER) {
      free += bag->GetFreeSlots();
    }
  }
  return free;
}

void CheckBagFoodAndDrink(Player* bot, bool* has_food, bool* has_drink) {
  *has_food = false;
  *has_drink = false;
  const auto check_item = [&](Item* item) {
    if (!item) {
      return;
    }
    const ItemTemplate* proto = item->GetTemplate();
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE ||
        (proto->SubClass != ITEM_SUBCLASS_FOOD &&
         proto->SubClass != ITEM_SUBCLASS_CONSUMABLE)) {
      return;
    }
    if (bot->CanUseItem(proto) != EQUIP_ERR_OK) {
      return;
    }
    const uint32_t cat = proto->Spells[0].SpellCategory;
    if (cat == kSpellCategoryFood) {
      *has_food = true;
    } else if (cat == kSpellCategoryDrink) {
      *has_drink = true;
    }
  };
  for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END;
       ++slot) {
    check_item(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
  }
  for (uint8_t i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i) {
    if (const Bag* bag = bot->GetBagByPos(i)) {
      for (uint32_t slot = 0; slot < bag->GetBagSize(); ++slot) {
        check_item(bot->GetItemByPos(i, static_cast<uint8_t>(slot)));
      }
    }
  }
}

const char* EquipmentSlotName(uint8_t slot) {
  switch (slot) {
    case EQUIPMENT_SLOT_HEAD:
      return "head";
    case EQUIPMENT_SLOT_NECK:
      return "neck";
    case EQUIPMENT_SLOT_SHOULDERS:
      return "shoulders";
    case EQUIPMENT_SLOT_BODY:
      return "shirt";
    case EQUIPMENT_SLOT_CHEST:
      return "chest";
    case EQUIPMENT_SLOT_WAIST:
      return "waist";
    case EQUIPMENT_SLOT_LEGS:
      return "legs";
    case EQUIPMENT_SLOT_FEET:
      return "feet";
    case EQUIPMENT_SLOT_WRISTS:
      return "wrists";
    case EQUIPMENT_SLOT_HANDS:
      return "hands";
    case EQUIPMENT_SLOT_FINGER1:
      return "finger1";
    case EQUIPMENT_SLOT_FINGER2:
      return "finger2";
    case EQUIPMENT_SLOT_TRINKET1:
      return "trinket1";
    case EQUIPMENT_SLOT_TRINKET2:
      return "trinket2";
    case EQUIPMENT_SLOT_BACK:
      return "back";
    case EQUIPMENT_SLOT_MAINHAND:
      return "mainhand";
    case EQUIPMENT_SLOT_OFFHAND:
      return "offhand";
    case EQUIPMENT_SLOT_RANGED:
      return "ranged";
    case EQUIPMENT_SLOT_TABARD:
      return "tabard";
    default:
      return "slot";
  }
}

void FillIndicators(Player* bot, pb::Self_Indicators* indicators) {
  indicators->bag_free_slots = FreeBagSlots(bot);
  indicators->talent_points_unspent = bot->GetFreeTalentPoints() > 0;
  indicators->new_mail = bot->unReadMails > 0;
  for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot) {
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item) {
      continue;
    }
    const uint32_t max_dur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    if (max_dur == 0) {
      continue;
    }
    const uint32_t cur_dur = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
    if (cur_dur == 0) {
      indicators->durability_warning.push_back(
          {EquipmentSlotName(slot), pb::Self_Indicators::Durability::BROKEN});
    } else if (cur_dur * 100 <= max_dur * 25) {
      indicators->durability_warning.push_back(
          {EquipmentSlotName(slot), pb::Self_Indicators::Durability::DAMAGED});
    }
  }
}

float TopThreat(const ThreatManager& mgr, Unit** top_unit = nullptr) {
  float best = 0.0f;
  Unit* leader = nullptr;
  for (const ThreatReference* ref : mgr.GetUnsortedThreatList()) {
    if (!ref->IsAvailable()) {
      continue;
    }
    const float threat = ref->GetThreat();
    if (threat > best || leader == nullptr) {
      best = threat;
      leader = ref->GetVictim();
    }
  }
  if (top_unit != nullptr) {
    *top_unit = leader;
  }
  return best;
}

const char* LootMethodName(LootMethod method) {
  switch (method) {
    case FREE_FOR_ALL:
      return "FREE_FOR_ALL";
    case ROUND_ROBIN:
      return "ROUND_ROBIN";
    case MASTER_LOOT:
      return "MASTER_LOOT";
    case GROUP_LOOT:
      return "GROUP_LOOT";
    case NEED_BEFORE_GREED:
      return "NEED_BEFORE_GREED";
  }
  return "";
}

pb::Role PartyRole(uint8_t roles) {
  if (roles & lfg::PLAYER_ROLE_TANK) {
    return pb::Role::TANK;
  }
  if (roles & lfg::PLAYER_ROLE_HEALER) {
    return pb::Role::HEALER;
  }
  if (roles & lfg::PLAYER_ROLE_DAMAGE) {
    return pb::Role::DPS;
  }
  return pb::Role::ROLE_UNSPECIFIED;
}

void FillSelf(Player* bot, pb::Self* self) {
  const Powers power = bot->getPowerType();
  pb::Unit& unit = self->unit;
  unit.guid = GuidString(bot->GetGUID());
  unit.name = bot->GetName();
  unit.kind = pb::Unit::PLAYER;
  unit.level = bot->GetLevel();
  unit.health_pct = Percent(bot->GetHealth(), bot->GetMaxHealth());
  unit.power_pct = Percent(bot->GetPower(power), bot->GetMaxPower(power));
  unit.dead = bot->isDead();
  unit.player.emplace().class_ = ClassName(bot->getClass());

  self->gender =
      bot->getGender() == GENDER_FEMALE ? pb::Self::FEMALE : pb::Self::MALE;
  self->xp = bot->GetUInt32Value(PLAYER_XP);
  self->xp_next = bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
  self->rested_xp = static_cast<uint32_t>(bot->GetRestBonus());
  self->health = bot->GetHealth();
  self->health_max = bot->GetMaxHealth();
  self->power_type = ToPowerType(power);
  self->power = bot->GetPower(power);
  self->power_max = bot->GetMaxPower(power);
  if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
    self->state = pb::Self::GHOST;
  } else if (bot->isDead()) {
    self->state = pb::Self::DEAD_BODY;
  } else {
    self->state = pb::Self::ALIVE;
  }
  self->in_combat = bot->IsInCombat();
  self->resting = bot->HasPlayerFlag(PLAYER_FLAGS_RESTING);
  self->mounted = bot->IsMounted();
  self->swimming = bot->IsInWater();
  self->in_flight = bot->IsInFlight();
  self->on_transport = bot->GetTransport() != nullptr;
  self->money = bot->GetMoney();
  FillIndicators(bot, &self->indicators);

  pb::Self::Control& control = self->control;
  control.stunned = bot->HasUnitState(UNIT_STATE_STUNNED) ||
                    bot->HasUnitFlag(UNIT_FLAG_STUNNED);
  control.feared =
      bot->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) ||
      bot->HasUnitFlag(UNIT_FLAG_FLEEING | UNIT_FLAG_CONFUSED);
  control.charmed =
      bot->HasUnitState(UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED) ||
      !bot->GetCharmerGUID().IsEmpty();
  control.silenced = bot->HasUnitFlag(UNIT_FLAG_SILENCED);
  control.rooted = bot->HasUnitState(UNIT_STATE_ROOT);
  control.pacified = bot->HasUnitFlag(UNIT_FLAG_PACIFIED);
  control.disarmed = bot->HasUnitFlag(UNIT_FLAG_DISARMED);
}

void FillWhere(Player* bot, pb::Where* where) {
  Map* map = bot->GetMap();
  where->map = map ? map->GetMapName() : "";
  const uint32_t zone_id = bot->GetZoneId();
  const uint32_t area_id = bot->GetAreaId();
  where->zone = AreaName(zone_id);
  if (area_id != zone_id) {
    where->subzone = AreaName(area_id);
  }
  where->position.x = bot->GetPositionX();
  where->position.y = bot->GetPositionY();
  where->position.z = bot->GetPositionZ();
  where->facing = bot->GetOrientation();
  where->indoors = !bot->IsOutdoors();
  where->in_instance = map && map->Instanceable();
}

void FillTarget(Player* bot, NearbyBuilder* nearby, pb::Extensions* ext) {
  Unit* target_unit = bot->GetSelectedUnit();
  if (!target_unit) {
    target_unit = bot->GetVictim();
  }
  if (!target_unit || target_unit == bot || !target_unit->IsInWorld()) {
    return;
  }
  pb::Target& target = ext->target.emplace();
  nearby->FillUnit(bot, target_unit, &target.target.emplace());
  Unit* tot = target_unit->GetVictim();
  if (!tot && !target_unit->GetTarget().IsEmpty()) {
    tot = ObjectAccessor::GetUnit(*target_unit, target_unit->GetTarget());
  }
  if (tot) {
    target.target_of_target = tot->GetName();
  }
  const ThreatManager& mgr = target_unit->GetThreatMgr();
  if (mgr.CanHaveThreatList() && !mgr.IsThreatListEmpty()) {
    const float top = TopThreat(mgr);
    if (top > 0.0f) {
      target.my_threat_pct = 100.0f * mgr.GetThreat(bot) / top;
    }
  }
}

void FillCombat(Player* bot, NearbyBuilder* nearby, pb::Extensions* ext) {
  if (!bot->IsInCombat()) {
    return;
  }
  pb::Combat& combat = ext->combat.emplace();
  for (Unit* attacker : bot->getAttackers()) {
    if (!attacker || !attacker->IsInWorld() || !attacker->IsAlive()) {
      continue;
    }
    nearby->FillUnit(bot, attacker, &combat.attackers.emplace_back());
    const ThreatManager& mgr = attacker->GetThreatMgr();
    if (mgr.CanHaveThreatList() && !mgr.IsThreatListEmpty()) {
      Unit* top_unit = nullptr;
      const float top = TopThreat(mgr, &top_unit);
      const float pct = top > 0.0f ? 100.0f * mgr.GetThreat(bot) / top : 0.0f;
      combat.threat.push_back(pb::Combat::Threat{
          attacker->GetName(), pct, top_unit ? top_unit->GetName() : ""});
    }
  }
  const uint32_t combo = bot->GetComboPoints();
  const uint32_t shards =
      bot->getClass() == CLASS_WARLOCK ? bot->GetItemCount(6265) : 0;
  if (combo > 0 || shards > 0) {
    pb::Combat::Resources& res = combat.resources.emplace();
    res.combo_points = combo;
    res.soul_shards = shards;
  }
}

void FillParty(Player* bot, NearbyBuilder* nearby, pb::Extensions* ext) {
  Group* group = bot->GetGroup();
  if (!group) {
    return;
  }
  pb::Party& party = ext->party.emplace();
  if (const char* leader = group->GetLeaderName()) {
    party.leader = leader;
  }
  party.loot_method = LootMethodName(group->GetLootMethod());
  for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
    if (slot.guid == bot->GetGUID()) {
      continue;
    }
    pb::Unit& m = party.members.emplace_back();
    m.guid = GuidString(slot.guid);
    m.name = slot.name;
    m.kind = pb::Unit::PLAYER;
    m.detail = pb::Unit::NAMEPLATE;
    pb::GroupPart& gp = m.group.emplace();
    gp.subgroup = static_cast<uint32_t>(slot.group) + 1;
    gp.leader = slot.guid == group->GetLeaderGUID();
    gp.assistant = (slot.flags & MEMBER_FLAG_ASSISTANT) != 0;
    gp.main_tank = (slot.flags & MEMBER_FLAG_MAINTANK) != 0;
    gp.role = PartyRole(slot.roles);
    if (Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
        member && member->IsInWorld()) {
      gp.online = true;
      m.level = member->GetLevel();
      m.dead = member->isDead();
      m.health_pct = Percent(member->GetHealth(), member->GetMaxHealth());
      const Powers p = member->getPowerType();
      if (member->GetMaxPower(p) > 0) {
        m.power_pct = Percent(member->GetPower(p), member->GetMaxPower(p));
      }
      m.player.emplace().class_ = ClassName(member->getClass());
      gp.aggro = !member->getAttackers().empty();
      gp.in_range = member->GetMapId() == bot->GetMapId() &&
                    bot->GetDistance(member) <= 100.0f;
      if (gp.in_range) {
        nearby->FillUnitRelative(bot, member, &m.rel);
      }
      if (Unit* sel = member->GetSelectedUnit()) {
        m.target = sel->GetName();
      }
    }
  }
}

std::optional<pb::Place> FindQuestPoiPlace(Player* bot,
                                           const Quest* q,
                                           int32_t objective_index) {
  const QuestPOIVector* pois = sObjectMgr->GetQuestPOIVector(q->GetQuestId());
  if (!pois) {
    return std::nullopt;
  }
  Map* map = bot->GetMap();
  if (!map) {
    return std::nullopt;
  }
  for (const QuestPOI& poi : *pois) {
    if (poi.MapId != bot->GetMapId() || poi.ObjectiveIndex != objective_index ||
        poi.points.empty()) {
      continue;
    }
    float cx = 0.0f;
    float cy = 0.0f;
    for (const QuestPOIPoint& pt : poi.points) {
      cx += static_cast<float>(pt.x);
      cy += static_cast<float>(pt.y);
    }
    cx /= static_cast<float>(poi.points.size());
    cy /= static_cast<float>(poi.points.size());
    if (bot->GetExactDist2d(cx, cy) > kMaxTravelDistYd) {
      continue;
    }
    const float cz = std::max(map->GetHeight(cx, cy, MAX_HEIGHT),
                              map->GetWaterLevel(cx, cy));
    if (cz == INVALID_HEIGHT || cz == VMAP_INVALID_HEIGHT_VALUE) {
      continue;
    }
    uint32_t zone_id = 0;
    uint32_t area_id = 0;
    sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zone_id, area_id,
                              bot->GetMapId(), cx, cy, cz);
    std::string area_name = AreaName(area_id != 0 ? area_id : zone_id);
    if (area_name.empty()) {
      area_name = q->GetTitle();
    }
    pb::Place place;
    place.name = std::move(area_name);
    FillPointRelative(bot, cx, cy, cz, &place.rel);
    place.kind = pb::Place::OBJECTIVE_AREA;
    place.known = true;
    place.quest = q->GetTitle();
    return place;
  }
  return std::nullopt;
}

void FillQuestTracker(Player* bot, pb::Extensions* ext) {
  // 3.3.5a auto-watches accepted quests; show the whole quest log here.
  const auto& status_map = bot->getQuestStatusMap();
  for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot) {
    const uint32_t quest_id = bot->GetQuestSlotQuestId(slot);
    if (quest_id == 0) {
      continue;
    }
    const Quest* q = sObjectMgr->GetQuestTemplate(quest_id);
    if (!q) {
      continue;
    }
    const QuestStatus status = bot->GetQuestStatus(quest_id);
    pb::QuestPart part;
    part.quest_id = quest_id;
    part.quest = q->GetTitle();
    part.complete = status == QUEST_STATUS_COMPLETE;
    part.failed = status == QUEST_STATUS_FAILED;
    part.daily = q->IsDaily();
    if (q->GetQuestLevel() > 0) {
      part.level = static_cast<uint32_t>(q->GetQuestLevel());
    }
    const auto status_it = status_map.find(quest_id);
    if (status_it != status_map.end() && status_it->second.Timer > 0) {
      part.timer_s = static_cast<float>(status_it->second.Timer) / 1000.0f;
    }
    const std::optional<pb::Place> turnin_where =
        part.complete ? FindQuestPoiPlace(bot, q, -1) : std::nullopt;

    bool added_objective = false;
    for (size_t i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
      const int32_t npc_or_go = q->RequiredNpcOrGo[i];
      const uint32_t need = q->RequiredNpcOrGoCount[i];
      if (npc_or_go == 0 || need == 0) {
        continue;
      }
      const uint32_t have =
          status_it != status_map.end()
              ? status_it->second.CreatureOrGOCount[i]
              : bot->GetQuestSlotCounter(slot, static_cast<uint8_t>(i));
      std::string title = q->ObjectiveText[i];
      if (title.empty()) {
        if (npc_or_go > 0) {
          const CreatureTemplate* tmpl =
              sObjectMgr->GetCreatureTemplate(static_cast<uint32_t>(npc_or_go));
          title = tmpl ? tmpl->Name + " slain" : "objective";
        } else {
          const GameObjectTemplate* tmpl = sObjectMgr->GetGameObjectTemplate(
              static_cast<uint32_t>(-npc_or_go));
          title = tmpl ? tmpl->name : "objective";
        }
      }
      pb::Objective& obj = ext->quest_tracker.emplace_back();
      obj.title = std::move(title);
      obj.progress = pb::Objective::Progress{have, need};
      obj.done = have >= need;
      obj.quest = part;
      obj.where = obj.done ? turnin_where
                           : FindQuestPoiPlace(bot, q, static_cast<int32_t>(i));
      added_objective = true;
    }
    for (size_t i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i) {
      const uint32_t item_id = q->RequiredItemId[i];
      const uint32_t need = q->RequiredItemCount[i];
      if (item_id == 0 || need == 0) {
        continue;
      }
      const uint32_t have = status_it != status_map.end()
                                ? status_it->second.ItemCount[i]
                                : bot->GetItemCount(item_id);
      const ItemTemplate* tmpl = sObjectMgr->GetItemTemplate(item_id);
      pb::Objective& obj = ext->quest_tracker.emplace_back();
      obj.title = tmpl && !tmpl->Name1.empty() ? tmpl->Name1 : "item";
      obj.progress = pb::Objective::Progress{have, need};
      obj.done = have >= need;
      obj.quest = part;
      obj.where =
          obj.done
              ? turnin_where
              : FindQuestPoiPlace(
                    bot, q, static_cast<int32_t>(QUEST_OBJECTIVES_COUNT + i));
      added_objective = true;
    }
    if (!added_objective) {
      pb::Objective& obj = ext->quest_tracker.emplace_back();
      obj.title = q->GetTitle();
      obj.done = part.complete;
      obj.quest = part;
      obj.where = turnin_where;
    }
  }
}

void FillExploreSpots(Player* bot,
                      const WorldPlacesCache& cache,
                      std::vector<OptionFacts::Spot>* out) {
  const uint32_t map_id = bot->GetMapId();
  const uint32_t zone_id = bot->GetZoneId();
  bool in_city = false;
  if (const AreaTableEntry* zone = sAreaTableStore.LookupEntry(zone_id)) {
    in_city = (zone->flags & AREA_FLAG_CAPITAL) != 0;
  }

  std::vector<OptionFacts::Spot> candidates;
  const auto add_candidate = [&](float x, float y, float z,
                                 const std::string& label) {
    const float dist = bot->GetExactDist(x, y, z);
    if (dist < kMinTravelDistYd || dist > kMaxTravelDistYd) {
      return;
    }
    const pb::Direction dir =
        CompassDirection(x - bot->GetPositionX(), y - bot->GetPositionY());
    if (dir == pb::DIRECTION_UNSPECIFIED || dir == pb::HERE) {
      return;
    }
    const std::string id = Acore::StringFormat(
        "spot:{}:{}:{}", map_id, static_cast<int>(std::lround(x)),
        static_cast<int>(std::lround(y)));
    candidates.push_back({id, dir, dist, label, {x, y, z}});
  };

  // Primary source: playerbots' per-level spot cache, annotated with the
  // 50-yd bucket's mob names and level range from WorldPlacesCache.
  const std::vector<WorldLocation>& level_locs =
      sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
  Map* map = bot->GetMap();
  for (const WorldLocation& loc : level_locs) {
    if (loc.GetMapId() != map_id) {
      continue;
    }
    if (!in_city && map &&
        map->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(),
                       loc.GetPositionY(), loc.GetPositionZ()) != zone_id) {
      continue;
    }
    const uint64_t key = WorldPlacesCache::BucketKey(
        map_id, loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ());
    std::string label;
    if (const auto it = cache.spot_buckets.find(key);
        it != cache.spot_buckets.end()) {
      label = it->second.FormatLabel();
    }
    add_candidate(loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ(),
                  label);
  }

  // If GetLocsPerLevelCache had no spots in this zone (e.g. a zone whose mobs
  // are outside the level bracket), fall back to the zone's buckets in
  // WorldPlacesCache within +-6 levels of the bot.
  if (candidates.empty()) {
    const uint32_t bot_level = bot->GetLevel();
    for (const auto& [key, b] : cache.spot_buckets) {
      if (b.map_id != map_id || (!in_city && b.zone_id != zone_id) ||
          b.count < 2) {
        continue;
      }
      if (b.min_level > bot_level + 6 ||
          (bot_level > 6 && b.max_level + 6 < bot_level)) {
        continue;
      }
      add_candidate(b.centroid.x, b.centroid.y, b.centroid.z, b.FormatLabel());
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const OptionFacts::Spot& a, const OptionFacts::Spot& b) {
              return a.dist_yd < b.dist_yd;
            });

  uint32_t seen_dirs = 0;
  for (OptionFacts::Spot& spot : candidates) {
    if (out->size() >= 3) {
      break;
    }
    const uint32_t bit = 1u << static_cast<uint8_t>(spot.dir);
    if ((seen_dirs & bit) != 0) {
      continue;
    }
    seen_dirs |= bit;
    out->push_back(std::move(spot));
  }

  // Fallback when a zone/instance has no cached grind spots: up to 3 walkable
  // mmap rays ~80 yd out.
  if (out->empty() && map) {
    static constexpr float kAngles[] = {0.0f, static_cast<float>(M_PI_2),
                                        static_cast<float>(M_PI),
                                        static_cast<float>(-M_PI_2)};
    for (float angle : kAngles) {
      if (out->size() >= 3) {
        break;
      }
      const float tx = bot->GetPositionX() + 80.0f * std::cos(angle);
      const float ty = bot->GetPositionY() + 80.0f * std::sin(angle);
      const float tz = map->GetHeight(bot->GetPhaseMask(), tx, ty,
                                      bot->GetPositionZ() + 5.0f);
      if (tz == INVALID_HEIGHT || tz == VMAP_INVALID_HEIGHT_VALUE) {
        continue;
      }
      PathGenerator path(bot);
      path.CalculatePath(tx, ty, tz);
      const uint32_t ok_mask =
          PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
      if ((path.GetPathType() & ~ok_mask) != 0) {
        continue;
      }
      const G3D::Vector3& end = path.GetActualEndPosition();
      const float dist = bot->GetExactDist(end.x, end.y, end.z);
      if (dist < kMinTravelDistYd) {
        continue;
      }
      const pb::Direction dir = CompassDirection(end.x - bot->GetPositionX(),
                                                 end.y - bot->GetPositionY());
      const uint32_t bit = 1u << static_cast<uint8_t>(dir);
      if ((seen_dirs & bit) != 0) {
        continue;
      }
      seen_dirs |= bit;
      out->push_back({Acore::StringFormat("spot:{}:{}:{}", map_id,
                                          static_cast<int>(std::lround(end.x)),
                                          static_cast<int>(std::lround(end.y))),
                      dir,
                      dist,
                      "",
                      {end.x, end.y, end.z}});
    }
  }
}

void FillGoToPlacesAndMinimap(Player* bot,
                              const WorldPlacesCache& cache,
                              pb::Snapshot* snapshot,
                              std::vector<OptionFacts::Place>* out_places) {
  const uint32_t map_id = bot->GetMapId();
  const bool is_alliance = bot->GetTeamId() == TEAM_ALLIANCE;
  const uint32_t class_bit = 1u << bot->getClass();
  const std::string trainer_label = base::StrCat(
      {base::ToLowerASCII(ClassName(bot->getClass())), " trainer"});

  // Also populates ext.minimap for places within minimap range (glance).
  pb::Minimap& minimap = snapshot->ext.minimap.emplace();
  minimap.zone = snapshot->base.where.zone;
  minimap.subzone = snapshot->base.where.subzone;

  struct RankedHub {
    const WorldPlacesCache::Hub* hub = nullptr;
    float dist_yd = 0.0f;
  };
  std::vector<RankedHub> valid_hubs;
  for (const WorldPlacesCache::Hub& hub : cache.hubs) {
    if (hub.map_id != map_id) {
      continue;
    }
    if (is_alliance ? !hub.for_alliance : !hub.for_horde) {
      continue;
    }
    const float dist = bot->GetExactDist(hub.pos.x, hub.pos.y, hub.pos.z);
    if (dist <= kMinimapRangeYd) {
      pb::Place& mp = minimap.places.emplace_back();
      mp.name = hub.name;
      FillPointRelative(bot, hub.pos.x, hub.pos.y, hub.pos.z, &mp.rel);
      mp.kind = hub.has_innkeeper ? pb::Place::TOWN : pb::Place::CAMP;
      mp.known = true;
    }
    if (dist >= kMinTravelDistYd && dist <= kMaxTravelDistYd) {
      valid_hubs.push_back({&hub, dist});
    }
  }
  std::sort(valid_hubs.begin(), valid_hubs.end(),
            [](const RankedHub& a, const RankedHub& b) {
              return a.dist_yd < b.dist_yd;
            });

  // Pick the nearest hub for each service kind the bot could need:
  // innkeeper, vendor/repair, class trainer, flight master.
  std::vector<const WorldPlacesCache::Hub*> chosen;
  const auto pick_first = [&](auto pred) {
    for (const RankedHub& rh : valid_hubs) {
      if (pred(*rh.hub)) {
        if (std::find(chosen.begin(), chosen.end(), rh.hub) == chosen.end()) {
          chosen.push_back(rh.hub);
        }
        return;
      }
    }
  };
  pick_first([](const WorldPlacesCache::Hub& h) { return h.has_innkeeper; });
  pick_first([](const WorldPlacesCache::Hub& h) {
    return h.has_vendor || h.has_repair;
  });
  pick_first([&](const WorldPlacesCache::Hub& h) {
    return (h.class_trainer_mask & class_bit) != 0;
  });
  pick_first(
      [](const WorldPlacesCache::Hub& h) { return h.has_flight_master; });

  const GraveyardStruct* grave =
      sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
  bool grave_merged = false;

  for (const WorldPlacesCache::Hub* hub : chosen) {
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
    if (grave && grave->Map == map_id &&
        std::hypot(grave->x - hub->pos.x, grave->y - hub->pos.y) <= 120.0f) {
      services.push_back("graveyard");
      grave_merged = true;
    }
    const float dist = bot->GetExactDist(hub->pos.x, hub->pos.y, hub->pos.z);
    const pb::Direction dir = CompassDirection(
        hub->pos.x - bot->GetPositionX(), hub->pos.y - bot->GetPositionY());
    out_places->push_back({hub->id, hub->name, base::JoinString(services, ", "),
                           dir, dist, hub->pos});
  }

  if (grave && !grave_merged && grave->Map == map_id) {
    const float dist = bot->GetExactDist(grave->x, grave->y, grave->z);
    if (dist >= kMinTravelDistYd && dist <= kMaxTravelDistYd) {
      uint32_t gz = 0;
      uint32_t ga = 0;
      sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), gz, ga, map_id, grave->x,
                                grave->y, grave->z);
      std::string gname = AreaName(ga != 0 ? ga : gz);
      gname = gname.empty() ? "Graveyard" : gname + " Graveyard";
      const pb::Direction dir = CompassDirection(
          grave->x - bot->GetPositionX(), grave->y - bot->GetPositionY());
      out_places->push_back({SlugId("place:", gname),
                             gname,
                             "graveyard",
                             dir,
                             dist,
                             {grave->x, grave->y, grave->z}});
    }
  }

  // Nearest completed quest turn-in and nearest incomplete quest objective
  // from quest_tracker ("the nearest place of each kind").
  const pb::Objective* best_turnin = nullptr;
  const pb::Objective* best_objective = nullptr;
  for (const pb::Objective& obj : snapshot->ext.quest_tracker) {
    if (!obj.where || !obj.quest || obj.where->rel.dist_yd < kMinTravelDistYd) {
      continue;
    }
    if (obj.where->rel.dist_yd <= kMinimapRangeYd) {
      minimap.objective_areas.push_back(*obj.where);
    }
    if (obj.quest->complete) {
      if (!best_turnin ||
          obj.where->rel.dist_yd < best_turnin->where->rel.dist_yd) {
        best_turnin = &obj;
      }
    } else if (!obj.done &&
               (!best_objective ||
                obj.where->rel.dist_yd < best_objective->where->rel.dist_yd)) {
      best_objective = &obj;
    }
  }
  for (const pb::Objective* obj : {best_turnin, best_objective}) {
    if (!obj) {
      continue;
    }
    const std::string service =
        (obj->quest->complete ? "turn in: " : "quest: ") + obj->quest->quest;
    const std::string id =
        Acore::StringFormat("place:quest_{}_{}", obj->quest->quest_id,
                            SlugId("", obj->where->name));
    out_places->push_back({id, obj->where->name, service, obj->where->rel.dir,
                           obj->where->rel.dist_yd, obj->where->rel.position});
  }
}

void FillOptionFactsAndPlaces(Player* bot,
                              pb::Snapshot* snapshot,
                              OptionFacts* facts) {
  const WorldPlacesCache& cache = GetWorldPlacesCache();
  OptionFacts local_facts;
  OptionFacts* out = facts ? facts : &local_facts;
  *out = OptionFacts{};

  for (const pb::Unit& u : snapshot->base.nearby.units) {
    if (u.kind != pb::Unit::NPC || u.dead || u.reaction != pb::Unit::FRIENDLY ||
        u.guid.empty()) {
      continue;
    }
    if (Creature* creature =
            ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(u.guid))) {
      const uint32_t flags = creature->GetUInt32Value(UNIT_NPC_FLAGS);
      if ((flags & kTalkNpcFlags) != 0) {
        out->npcs.push_back({u.guid, flags});
      }
    }
  }

  CheckBagFoodAndDrink(bot, &out->has_food, &out->has_drink);
  out->herbalism_skill = bot->GetSkillValue(SKILL_HERBALISM);
  out->mining_skill = bot->GetSkillValue(SKILL_MINING);

  if (bot->IsAlive() && !bot->IsInCombat()) {
    FillExploreSpots(bot, cache, &out->spots);
    FillGoToPlacesAndMinimap(bot, cache, snapshot, &out->places);
  }
}

}  // namespace

SnapshotBuilder::SnapshotBuilder(NearbyBuilder::Options nearby_options)
    : nearby_(nearby_options) {}

SnapshotBuilder::~SnapshotBuilder() = default;

NearbyBuilder::Stats SnapshotBuilder::Build(Player* bot,
                                            pb::Snapshot* snapshot,
                                            OptionFacts* facts) {
  pb::Base& base = snapshot->base;
  FillSelf(bot, &base.self);
  FillWhere(bot, &base.where);
  const NearbyBuilder::Stats stats = nearby_.Build(bot, &base.nearby);
  FillTarget(bot, &nearby_, &snapshot->ext);
  FillCombat(bot, &nearby_, &snapshot->ext);
  FillParty(bot, &nearby_, &snapshot->ext);
  FillQuestTracker(bot, &snapshot->ext);
  FillOptionFactsAndPlaces(bot, snapshot, facts);
  return stats;
}

}  // namespace llm_brain
