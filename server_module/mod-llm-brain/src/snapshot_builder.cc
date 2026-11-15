// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot_builder.h"

#include <cstdint>
#include <string>

#include "DBCStores.h"
#include "DBCStructure.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "UpdateFields.h"
#include "World.h"
#include "proto/snapshot.pb.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

// The class names the client shows (ChrClasses.dbc's name column isn't loaded).
const char* ClassName(uint8_t class_id) {
  switch (class_id) {
    case CLASS_WARRIOR:
      return "Warrior";
    case CLASS_PALADIN:
      return "Paladin";
    case CLASS_HUNTER:
      return "Hunter";
    case CLASS_ROGUE:
      return "Rogue";
    case CLASS_PRIEST:
      return "Priest";
    case CLASS_DEATH_KNIGHT:
      return "Death Knight";
    case CLASS_SHAMAN:
      return "Shaman";
    case CLASS_MAGE:
      return "Mage";
    case CLASS_WARLOCK:
      return "Warlock";
    case CLASS_DRUID:
      return "Druid";
    default:
      return "";
  }
}

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

void FillSelf(Player* bot, pb::Self* self) {
  const Powers power = bot->getPowerType();
  pb::Unit* unit = self->mutable_unit();
  unit->set_guid(GuidString(bot->GetGUID()));
  unit->set_name(bot->GetName());
  unit->set_kind(pb::Unit::PLAYER);
  unit->set_level(bot->GetLevel());
  unit->set_health_pct(Percent(bot->GetHealth(), bot->GetMaxHealth()));
  unit->set_power_pct(Percent(bot->GetPower(power), bot->GetMaxPower(power)));
  unit->set_dead(bot->isDead());
  unit->mutable_player()->set_class_(ClassName(bot->getClass()));

  self->set_gender(bot->getGender() == GENDER_FEMALE ? pb::Self::FEMALE
                                                     : pb::Self::MALE);
  self->set_xp(bot->GetUInt32Value(PLAYER_XP));
  self->set_xp_next(bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP));
  self->set_rested_xp(static_cast<uint32_t>(bot->GetRestBonus()));
  self->set_health(bot->GetHealth());
  self->set_health_max(bot->GetMaxHealth());
  self->set_power_type(ToPowerType(power));
  self->set_power(bot->GetPower(power));
  self->set_power_max(bot->GetMaxPower(power));
  if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
    self->set_state(pb::Self::GHOST);
  } else if (bot->isDead()) {
    self->set_state(pb::Self::DEAD_BODY);
  } else {
    self->set_state(pb::Self::ALIVE);
  }
  self->set_in_combat(bot->IsInCombat());
  self->set_resting(bot->HasPlayerFlag(PLAYER_FLAGS_RESTING));
  self->set_mounted(bot->IsMounted());
  self->set_swimming(bot->IsInWater());
  self->set_in_flight(bot->IsInFlight());
  self->set_on_transport(bot->GetTransport() != nullptr);
  self->set_money(bot->GetMoney());
}

void FillWhere(Player* bot, pb::Where* where) {
  Map* map = bot->GetMap();
  where->set_map(map ? map->GetMapName() : "");
  const uint32_t zone_id = bot->GetZoneId();
  const uint32_t area_id = bot->GetAreaId();
  where->set_zone(AreaName(zone_id));
  if (area_id != zone_id) {
    where->set_subzone(AreaName(area_id));
  }
  pb::Vec3* position = where->mutable_position();
  position->set_x(bot->GetPositionX());
  position->set_y(bot->GetPositionY());
  position->set_z(bot->GetPositionZ());
  where->set_facing(bot->GetOrientation());
  where->set_indoors(!bot->IsOutdoors());
  where->set_in_instance(map && map->Instanceable());
}

}  // namespace

void BuildSnapshot(Player* bot, pb::Snapshot* snapshot) {
  pb::Base* base = snapshot->mutable_base();
  FillSelf(bot, base->mutable_self());
  FillWhere(bot, base->mutable_where());
}

}  // namespace llm_brain
