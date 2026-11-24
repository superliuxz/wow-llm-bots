// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/snapshot_builder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Bag.h"
#include "Creature.h"
#include "CreatureData.h"
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
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "ThreatManager.h"
#include "Trainer.h"
#include "TravelMgr.h"
#include "Unit.h"
#include "UpdateFields.h"
#include "World.h"
#include "base/time/time.h"
#include "options/option_executor.h"
#include "options/option_facts.h"
#include "options/options.h"
#include "options/position_facts.h"
#include "snapshot/nearby_builder.h"
#include "snapshot/panels_builder.h"
#include "snapshot/perception.h"
#include "snapshot/snapshot.h"
#include "snapshot/ui_state.h"
#include "snapshot/world_util.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

// When a bot's Explore spots and Go to places are worked out again
// (docs/specs/options.md, "World-thread cost").
constexpr float kFactsRecomputeYd = 20.0f;
constexpr base::TimeDelta kFactsRecomputeAfter = base::Seconds(10);

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

float Percent(uint32_t value, uint32_t max) {
  return max == 0 ? 0.0f : 100.0f * static_cast<float>(value) / max;
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
    if (const Bag* bag = bot->GetBagByPos(i); IsGeneralBag(bag)) {
      free += bag->GetFreeSlots();
    }
  }
  return free;
}

const char* EquipmentSlotName(uint8_t slot) {
  static constexpr const char* kSlotNames[] = {
      "head",     "neck",    "shoulders", "shirt",    "chest",
      "waist",    "legs",    "feet",      "wrists",   "hands",
      "finger1",  "finger2", "trinket1",  "trinket2", "back",
      "mainhand", "offhand", "ranged",    "tabard"};
  return slot < std::size(kSlotNames) ? kSlotNames[slot] : "slot";
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

// The Dungeon Finder role when one was picked, else what the player's spec
// and stance say (what a player learns by inspecting: a prot warrior tanks, a
// holy priest heals), so roles show in every group, not only LFG (RH-M1).
pb::Role MemberRole(Player* member, uint8_t lfg_roles) {
  const pb::Role lfg = PartyRole(lfg_roles);
  if (lfg != pb::Role::ROLE_UNSPECIFIED || member == nullptr) {
    return lfg;
  }
  if (PlayerbotAI::IsTank(member)) {
    return pb::Role::TANK;
  }
  if (PlayerbotAI::IsHeal(member)) {
    return pb::Role::HEALER;
  }
  return pb::Role::DPS;
}

void FillSelf(Player* bot, pb::Self* self) {
  const Powers power = bot->getPowerType();
  pb::Unit& unit = self->unit;
  unit.guid = GuidString(bot->GetGUID().GetRawValue());
  unit.name = bot->GetName();
  unit.kind = pb::Unit::PLAYER;
  unit.level = bot->GetLevel();
  unit.health_pct = Percent(bot->GetHealth(), bot->GetMaxHealth());
  unit.power_pct = Percent(bot->GetPower(power), bot->GetMaxPower(power));
  unit.dead = bot->isDead();
  unit.player.emplace().class_ = ClassName(bot->getClass());
  if (Group* group = bot->GetGroup()) {
    for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
      if (slot.guid == bot->GetGUID()) {
        pb::GroupPart& gp = unit.group.emplace();
        gp.subgroup = static_cast<uint32_t>(slot.group) + 1;
        gp.leader = slot.guid == group->GetLeaderGUID();
        gp.assistant = (slot.flags & MEMBER_FLAG_ASSISTANT) != 0;
        gp.main_tank = (slot.flags & MEMBER_FLAG_MAINTANK) != 0;
        gp.main_assist = (slot.flags & MEMBER_FLAG_MAINASSIST) != 0;
        gp.role = MemberRole(bot, slot.roles);
        gp.online = true;
        gp.in_range = true;
        gp.aggro = !bot->getAttackers().empty();
        break;
      }
    }
  }
  // Populate Self::casting using the same visible cast-bar rules as
  // FillActivity (ignore flying projectiles and finished instants).
  if (Spell* sp = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      sp && sp->getState() == SPELL_STATE_PREPARING && sp->GetCastTime() > 0 &&
      sp->GetSpellInfo()) {
    pb::Cast& cast = self->casting.emplace();
    cast.spell.emplace().id = sp->GetSpellInfo()->Id;
    if (const char* sn =
            sp->GetSpellInfo()->SpellName[sWorld->GetDefaultDbcLocale()];
        sn && *sn) {
      cast.spell->name = sn;
    }
    cast.remaining_ms =
        static_cast<uint32_t>(std::max<int32_t>(0, sp->GetCastTimeRemaining()));
  } else if (Spell* ch = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
             ch && ch->getState() != SPELL_STATE_FINISHED &&
             ch->GetSpellInfo()) {
    pb::Cast& cast = self->casting.emplace();
    cast.spell.emplace().id = ch->GetSpellInfo()->Id;
    if (const char* sn =
            ch->GetSpellInfo()->SpellName[sWorld->GetDefaultDbcLocale()];
        sn && *sn) {
      cast.spell->name = sn;
    }
    cast.channeled = true;
    cast.remaining_ms =
        static_cast<uint32_t>(std::max<int32_t>(0, ch->GetCastTimeRemaining()));
  }
  // The bot's buff bar: the auras the client shows (those with a visible
  // slot), as a player sees their own ("Food" while eating).
  for (const auto& [spell_id, app] : bot->GetAppliedAuras()) {
    const Aura* base = app ? app->GetBase() : nullptr;
    if (!base || app->GetSlot() >= MAX_AURAS) {
      continue;
    }
    pb::Aura& aura = unit.auras.emplace_back();
    aura.id = spell_id;
    if (const char* name =
            base->GetSpellInfo()->SpellName[sWorld->GetDefaultDbcLocale()]) {
      aura.name = name;
    }
    aura.harmful = !app->IsPositive();
    if (base->GetDuration() > 0) {
      aura.remaining_s = static_cast<float>(base->GetDuration()) / 1000.0f;
    }
    aura.stacks = base->GetStackAmount() > 1 ? base->GetStackAmount() : 0;
  }

  unit.raid_marker = RaidMarkerOf(bot, bot->GetGUID().GetRawValue());
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
  if (Map* map = bot->GetMap()) {
    const LiquidData liquid = map->GetLiquidData(
        bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), bot->GetCollisionHeight(), MAP_ALL_LIQUIDS);
    if (liquid.Status != LIQUID_MAP_NO_WATER &&
        liquid.Status != LIQUID_MAP_ABOVE_WATER) {
      if ((liquid.Flags & MAP_LIQUID_TYPE_MAGMA) != 0) {
        self->liquid = "lava";
      } else if ((liquid.Flags & MAP_LIQUID_TYPE_SLIME) != 0) {
        self->liquid = "slime";
      } else if ((liquid.Flags &
                  (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) != 0) {
        self->liquid = "water";
      }
    }
  }
  self->in_flight = bot->IsInFlight();
  self->on_transport = bot->GetTransport() != nullptr;
  self->money = bot->GetMoney();
  FillIndicators(bot, &self->indicators);
  self->control = ReadControlState(bot);
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
    m.guid = GuidString(slot.guid.GetRawValue());
    m.name = slot.name;
    m.kind = pb::Unit::PLAYER;
    m.detail = pb::Unit::NAMEPLATE;
    m.raid_marker = RaidMarkerOf(bot, slot.guid.GetRawValue());
    pb::GroupPart& gp = m.group.emplace();
    gp.subgroup = static_cast<uint32_t>(slot.group) + 1;
    gp.leader = slot.guid == group->GetLeaderGUID();
    gp.assistant = (slot.flags & MEMBER_FLAG_ASSISTANT) != 0;
    gp.main_tank = (slot.flags & MEMBER_FLAG_MAINTANK) != 0;
    gp.main_assist = (slot.flags & MEMBER_FLAG_MAINASSIST) != 0;
    gp.role = PartyRole(slot.roles);
    if (Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
        member && member->IsInWorld()) {
      gp.role = MemberRole(member, slot.roles);
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

// The quest's map points for one objective (-1: the turn-in) on the bot's
// map: centroid, ground height and area name. Static data, but the height and
// area lookups cost, so each is worked out once per builder (QuestPois).
const SnapshotBuilder::QuestPoi& LookupQuestPoi(
    Player* bot,
    const Quest* q,
    int32_t objective_index,
    std::vector<SnapshotBuilder::QuestPoi>* cache) {
  for (const SnapshotBuilder::QuestPoi& entry : *cache) {
    if (entry.quest_id == q->GetQuestId() &&
        entry.objective == objective_index && entry.map_id == bot->GetMapId()) {
      return entry;
    }
  }
  SnapshotBuilder::QuestPoi& entry = cache->emplace_back();
  entry.quest_id = q->GetQuestId();
  entry.objective = objective_index;
  entry.map_id = bot->GetMapId();
  const QuestPOIVector* pois = sObjectMgr->GetQuestPOIVector(q->GetQuestId());
  Map* map = bot->GetMap();
  if (!pois || !map) {
    return entry;
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
    const float cz = std::max(map->GetHeight(cx, cy, MAX_HEIGHT),
                              map->GetWaterLevel(cx, cy));
    if (cz == INVALID_HEIGHT || cz == VMAP_INVALID_HEIGHT_VALUE) {
      continue;
    }
    uint32_t zone_id = 0;
    uint32_t area_id = 0;
    sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zone_id, area_id,
                              bot->GetMapId(), cx, cy, cz);
    std::string name = AreaName(area_id != 0 ? area_id : zone_id);
    if (name.empty()) {
      name = q->GetTitle();
    }
    entry.spots.push_back({std::move(name), {cx, cy, cz}});
  }
  return entry;
}

std::optional<pb::Place> FindQuestPoiPlace(
    Player* bot,
    const Quest* q,
    int32_t objective_index,
    std::vector<SnapshotBuilder::QuestPoi>* cache) {
  for (const auto& [name, pos] :
       LookupQuestPoi(bot, q, objective_index, cache).spots) {
    if (bot->GetExactDist2d(pos.x, pos.y) > kMaxTravelDistYd) {
      continue;
    }
    pb::Place place;
    place.name = name;
    FillPointRelative(bot, pos.x, pos.y, pos.z, &place.rel);
    place.kind = pb::Place::OBJECTIVE_AREA;
    place.known = true;
    place.quest = q->GetTitle();
    return place;
  }
  return std::nullopt;
}

void FillQuestTracker(Player* bot,
                      pb::Extensions* ext,
                      std::vector<SnapshotBuilder::QuestPoi>* pois) {
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
    if (Group* group = bot->GetGroup()) {
      for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
        if (slot.guid == bot->GetGUID() || slot.name.empty()) {
          continue;
        }
        Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
        if (!member || !member->IsInWorld()) {
          continue;
        }
        const QuestStatus ms = member->GetQuestStatus(quest_id);
        if (ms == QUEST_STATUS_INCOMPLETE || ms == QUEST_STATUS_COMPLETE) {
          part.party_members.push_back(slot.name);
        }
      }
      std::sort(part.party_members.begin(), part.party_members.end());
    }
    const std::optional<pb::Place> turnin_where =
        part.complete ? FindQuestPoiPlace(bot, q, -1, pois) : std::nullopt;

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
      obj.where =
          obj.done ? turnin_where
                   : FindQuestPoiPlace(bot, q, static_cast<int32_t>(i), pois);
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
                    bot, q, static_cast<int32_t>(QUEST_OBJECTIVES_COUNT + i),
                    pois);
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

// Talkable NPCs and non-attackable neutral/hostile units on screen, with their
// flags (what the client knows of a unit it can click: service icons, and
// whether the attack cursor is unavailable).
void FillNpcFacts(Player* bot, const pb::Snapshot& snapshot, OptionFacts* out) {
  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if ((u.kind != pb::Unit::NPC && u.kind != pb::Unit::CRITTER) || u.dead ||
        u.guid.empty()) {
      continue;
    }
    Creature* creature =
        ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(u.guid));
    if (!creature) {
      continue;
    }
    uint32_t flags = 0;
    if (u.kind == pb::Unit::NPC && u.reaction == pb::Unit::FRIENDLY) {
      flags |= creature->GetUInt32Value(UNIT_NPC_FLAGS) & kTalkNpcFlags;
    } else if ((u.reaction == pb::Unit::HOSTILE ||
                u.reaction == pb::Unit::NEUTRAL) &&
               !bot->IsValidAttackTarget(creature)) {
      flags |= kNpcFlagNotAttackable;
    }
    if (flags != 0) {
      out->npcs.push_back({u.guid, flags});
    }
  }
}

}  // namespace

SnapshotBuilder::SnapshotBuilder(NearbyBuilder::Options nearby_options,
                                 DungeonNavConfig dungeon_nav)
    : nearby_(nearby_options), dungeon_nav_(dungeon_nav) {}

SnapshotBuilder::~SnapshotBuilder() = default;

NearbyBuilder::Stats SnapshotBuilder::Build(Player* bot,
                                            pb::Snapshot* snapshot,
                                            OptionFacts* facts,
                                            const WorldPlaces* world,
                                            UiState* ui,
                                            const ItemSpellTexts* texts,
                                            base::TimeTicks now) {
  pb::Base& base = snapshot->base;
  FillSelf(bot, &base.self);
  FillWhere(bot, &base.where);
  const NearbyBuilder::Stats stats = nearby_.Build(bot, &base.nearby);
  FillTarget(bot, &nearby_, &snapshot->ext);
  FillCombat(bot, &nearby_, &snapshot->ext);
  FillParty(bot, &nearby_, &snapshot->ext);
  FillQuestTracker(bot, &snapshot->ext, &quest_pois_);
  FillOptionFacts(bot, snapshot, facts, world, now);
  // Open panels and the tooltip of what the bot points at (visibility OPEN and
  // HOVER, docs/specs/snapshot.md §3).
  const std::optional<pb::HoverRef> hover = ui->FillAndAge(&base.ui, now);
  if (ui->IsOpen(pb::BAGS)) {
    FillBags(bot, &snapshot->ext.bags.emplace());
  }
  if (ui->IsOpen(pb::CHARACTER)) {
    FillCharacter(bot, &snapshot->ext.character.emplace());
  }
  if (ui->IsOpen(pb::QUEST_LOG)) {
    FillQuestLog(bot, &snapshot->ext.quest_log.emplace());
  }
  if (hover) {
    if (std::optional<pb::Tooltipped> tip = BuildTooltip(bot, *hover, texts)) {
      base.tooltips.push_back(std::move(*tip));
    }
  }
  return stats;
}

void SnapshotBuilder::FillOptionFacts(Player* bot,
                                      pb::Snapshot* snapshot,
                                      OptionFacts* facts,
                                      const WorldPlaces* world,
                                      base::TimeTicks now) {
  OptionFacts local;
  OptionFacts* out = facts ? facts : &local;
  *out = OptionFacts{};
  FillNpcFacts(bot, *snapshot, out);
  CheckBagFoodAndDrink(bot, &out->has_food, &out->has_drink);
  CollectUsableBagItems(bot, &out->usable_items);
  CollectTalentTrees(bot, &out->talent_trees);
  CollectNpcActions(bot, *snapshot, out->has_food, out->has_drink,
                    &out->npc_actions);
  CollectGroupAndUpkeepActions(bot, *snapshot, &out->group_and_upkeep_actions);
  out->has_pending_resurrect = !bot->IsAlive() && bot->isResurrectRequested();
  if (Group* group = bot->GetGroup()) {
    for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
      if (slot.guid == bot->GetGUID()) {
        continue;
      }
      if (Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
          member && member->IsInWorld()) {
        if (Unit* sel = member->GetSelectedUnit()) {
          out->party_targets.push_back(
              {slot.name, GuidString(sel->GetGUID().GetRawValue())});
        }
      }
    }
  }
  out->herbalism_skill = bot->GetSkillValue(SKILL_HERBALISM);
  out->mining_skill = bot->GetSkillValue(SKILL_MINING);

  pb::Minimap& minimap = snapshot->ext.minimap.emplace();
  minimap.zone = snapshot->base.where.zone;
  minimap.subzone = snapshot->base.where.subzone;
  if (!world || !bot->IsAlive() || bot->IsInCombat()) {
    return;  // Explore and Go to are offered only alive and out of combat.
  }
  // The spots and places change only as the bot moves: recompute them after
  // 20 yd or 10 s, on a new map, zone or level, or when a cached spot/place
  // drops out of travel range; re-measure a copy in between so transient steps
  // across the 25 yd boundary never permanently erase cached entries.
  const Viewpoint view = ViewpointOf(bot);
  bool stale =
      !position_facts_ || position_view_.map_id != view.map_id ||
      position_view_.zone_id != view.zone_id ||
      position_view_.level != view.level ||
      std::hypot(position_view_.pos.x - view.pos.x,
                 position_view_.pos.y - view.pos.y) > kFactsRecomputeYd ||
      now - position_time_ > kFactsRecomputeAfter;
  PositionFacts current;
  if (!stale) {
    current = *position_facts_;
    RefreshFrom(view.pos, &current);
    if (current.spots.size() < position_facts_->spots.size() ||
        current.places.size() < position_facts_->places.size()) {
      stale = true;
    }
  }
  if (stale) {
    position_facts_ = ComputePositionFacts(*world, view, bot, dungeon_nav_);
    position_view_ = view;
    position_time_ = now;
    current = *position_facts_;
  }
  AddPositionFacts(current, view.pos, snapshot->ext.quest_tracker, out,
                   &minimap);
  // Expose a live per-snapshot `Go to <leader>` option for non-leader group
  // members when the living leader is on the same map/instance/phase at travel
  // distance (never cached in `position_facts_` so moving leaders and group
  // changes are reflected immediately).
  if (Group* group = bot->GetGroup();
      group && !group->IsLeader(bot->GetGUID())) {
    if (Player* leader =
            ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
        leader && leader->IsInWorld() && leader->IsAlive() &&
        leader->GetMapId() == view.map_id &&
        leader->GetInstanceId() == bot->GetInstanceId() &&
        (leader->GetPhaseMask() & view.phase_mask) != 0) {
      const pb::Vec3 leader_pos{leader->GetPositionX(), leader->GetPositionY(),
                                leader->GetPositionZ()};
      const float dist =
          Dist(view.pos, leader_pos.x, leader_pos.y, leader_pos.z);
      if (dist >= kMinTravelDistYd && dist <= kMaxTravelDistYd) {
        out->places.push_back(
            {"place:leader", leader->GetName(), "party leader",
             DirFrom(view.pos, leader_pos.x, leader_pos.y), dist, leader_pos});
      }
    }
  }
}

}  // namespace llm_brain
