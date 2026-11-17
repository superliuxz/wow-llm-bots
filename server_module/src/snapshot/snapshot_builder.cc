// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/snapshot_builder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "Bag.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "ThreatManager.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "World.h"
#include "proto/snapshot.h"
#include "snapshot/nearby_builder.h"
#include "snapshot/perception.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

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
      added_objective = true;
    }
    if (!added_objective) {
      pb::Objective& obj = ext->quest_tracker.emplace_back();
      obj.title = q->GetTitle();
      obj.done = part.complete;
      obj.quest = part;
    }
  }
}

}  // namespace

SnapshotBuilder::SnapshotBuilder(NearbyBuilder::Options nearby_options)
    : nearby_(nearby_options) {}

SnapshotBuilder::~SnapshotBuilder() = default;

NearbyBuilder::Stats SnapshotBuilder::Build(Player* bot,
                                            pb::Snapshot* snapshot) {
  pb::Base& base = snapshot->base;
  FillSelf(bot, &base.self);
  FillWhere(bot, &base.where);
  const NearbyBuilder::Stats stats = nearby_.Build(bot, &base.nearby);
  FillTarget(bot, &nearby_, &snapshot->ext);
  FillCombat(bot, &nearby_, &snapshot->ext);
  FillParty(bot, &nearby_, &snapshot->ext);
  FillQuestTracker(bot, &snapshot->ext);
  return stats;
}

}  // namespace llm_brain
