// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "nearby_builder.h"
#include <math.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Creature.h"
#include "CreatureGroups.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "DynamicObject.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectGuid.h"
#include "ObjectVisibilityContainer.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "World.h"
#include "base/time/time.h"
#include "perception.h"
#include "proto/snapshot.pb.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr float kLosNearYd = 40;
constexpr base::TimeDelta kLosNearTtl = base::Seconds(1);
constexpr base::TimeDelta kLosFarTtl = base::Seconds(5);
constexpr base::TimeDelta kLosForgetAfter = base::Seconds(30);
// Where a player looks at a game object: a little above its base.
constexpr float kObjectSightHeightYd = 1.0f;

std::string GuidString(ObjectGuid guid) {
  return Acore::StringFormat("0x{:016X}", guid.GetRawValue());
}

// A DBC string column in the server's locale (std::array or C array of 16).
template <typename Names>
const char* Localized(const Names& names) {
  const char* name = names[sWorld->GetDefaultDbcLocale()];
  return name ? name : "";
}

std::string SpellName(uint32_t spell_id) {
  const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
  return info ? Localized(info->SpellName) : "";
}

void FillRelative(Player* bot, WorldObject* object, pb::Relative* rel) {
  const float dx = object->GetPositionX() - bot->GetPositionX();
  const float dy = object->GetPositionY() - bot->GetPositionY();
  rel->set_dist_yd(bot->GetDistance(object));
  rel->set_dir(CompassDirection(dx, dy));
  rel->set_height(HeightOf(object->GetPositionZ() - bot->GetPositionZ()));
  pb::Vec3* position = rel->mutable_position();
  position->set_x(object->GetPositionX());
  position->set_y(object->GetPositionY());
  position->set_z(object->GetPositionZ());
}

pb::Unit::Kind UnitKind(Unit* unit) {
  if (unit->IsPlayer()) {
    return pb::Unit::PLAYER;
  }
  if (unit->IsTotem()) {
    return pb::Unit::TOTEM;
  }
  if (unit->IsPet()) {
    return pb::Unit::PET;
  }
  if (unit->IsVehicle()) {
    return pb::Unit::VEHICLE;
  }
  const Creature* creature = unit->ToCreature();
  if (creature &&
      creature->GetCreatureTemplate()->type == CREATURE_TYPE_CRITTER) {
    return pb::Unit::CRITTER;
  }
  return pb::Unit::NPC;
}

const char* RaceName(uint8_t race) {
  switch (race) {
    case RACE_HUMAN:
      return "human";
    case RACE_ORC:
      return "orc";
    case RACE_DWARF:
      return "dwarf";
    case RACE_NIGHTELF:
      return "night elf";
    case RACE_UNDEAD_PLAYER:
      return "undead";
    case RACE_TAUREN:
      return "tauren";
    case RACE_GNOME:
      return "gnome";
    case RACE_TROLL:
      return "troll";
    case RACE_BLOODELF:
      return "blood elf";
    case RACE_DRAENEI:
      return "draenei";
    default:
      return "person";
  }
}

const char* CreatureTypeName(uint32_t type) {
  switch (type) {
    case CREATURE_TYPE_BEAST:
      return "beast";
    case CREATURE_TYPE_DRAGONKIN:
      return "dragonkin";
    case CREATURE_TYPE_DEMON:
      return "demon";
    case CREATURE_TYPE_ELEMENTAL:
      return "elemental";
    case CREATURE_TYPE_GIANT:
      return "giant";
    case CREATURE_TYPE_UNDEAD:
      return "undead";
    case CREATURE_TYPE_HUMANOID:
      return "humanoid";
    case CREATURE_TYPE_CRITTER:
      return "critter";
    case CREATURE_TYPE_MECHANICAL:
      return "mechanical";
    case CREATURE_TYPE_TOTEM:
      return "totem";
    case CREATURE_TYPE_NON_COMBAT_PET:
      return "small pet";
    case CREATURE_TYPE_GAS_CLOUD:
      return "gas cloud";
    default:
      return "creature";
  }
}

// What a player recognizes from the model alone: "boar", "troll", "undead".
std::string LooksLike(Unit* unit) {
  if (const Player* player = unit->ToPlayer()) {
    return RaceName(player->getRace());
  }
  const CreatureTemplate* tmpl = unit->ToCreature()->GetCreatureTemplate();
  if (tmpl->family) {
    if (const CreatureFamilyEntry* family =
            sCreatureFamilyStore.LookupEntry(tmpl->family)) {
      std::string name = Localized(family->Name);
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (!name.empty()) {
        return name;
      }
    }
  }
  return CreatureTypeName(tmpl->type);
}

pb::Unit::Classification Classification(const Creature* creature) {
  switch (creature->GetCreatureTemplate()->rank) {
    case CREATURE_ELITE_ELITE:
      return pb::Unit::ELITE;
    case CREATURE_ELITE_RAREELITE:
      return pb::Unit::RARE_ELITE;
    case CREATURE_ELITE_WORLDBOSS:
      return pb::Unit::WORLD_BOSS;
    case CREATURE_ELITE_RARE:
      return pb::Unit::RARE;
    default:
      return pb::Unit::NORMAL;
  }
}

pb::Unit::Reaction Reaction(Unit* unit, Player* bot) {
  const ReputationRank rank = unit->GetReactionTo(bot);
  if (rank <= REP_UNFRIENDLY) {
    return pb::Unit::HOSTILE;
  }
  if (rank == REP_NEUTRAL) {
    return pb::Unit::NEUTRAL;
  }
  return pb::Unit::FRIENDLY;
}

pb::Unit::QuestMarker QuestMarker(Player* bot, Creature* creature) {
  if (!creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER)) {
    return pb::Unit::QUEST_MARKER_UNSPECIFIED;
  }
  switch (bot->GetQuestDialogStatus(creature)) {
    case DIALOG_STATUS_AVAILABLE:
    case DIALOG_STATUS_AVAILABLE_REP:
      return pb::Unit::QUEST_AVAILABLE;
    case DIALOG_STATUS_LOW_LEVEL_AVAILABLE:
    case DIALOG_STATUS_LOW_LEVEL_AVAILABLE_REP:
      return pb::Unit::QUEST_AVAILABLE_LOW;
    case DIALOG_STATUS_REWARD:
    case DIALOG_STATUS_REWARD2:
    case DIALOG_STATUS_REWARD_REP:
    case DIALOG_STATUS_LOW_LEVEL_REWARD_REP:
      return pb::Unit::QUEST_TURN_IN;
    case DIALOG_STATUS_INCOMPLETE:
      return pb::Unit::QUEST_INCOMPLETE;
    default:
      return pb::Unit::QUEST_MARKER_UNSPECIFIED;
  }
}

void FillCast(Spell* spell, pb::Cast* cast) {
  const SpellInfo* info = spell->GetSpellInfo();
  cast->mutable_spell()->set_id(info->Id);
  cast->mutable_spell()->set_name(Localized(info->SpellName));
  cast->set_remaining_ms(
      static_cast<uint32_t>(std::max(0, spell->GetCastTimeRemaining())));
  cast->set_interruptible(info->InterruptFlags &
                          SPELL_INTERRUPT_FLAG_INTERRUPT);
  cast->set_channeled(info->IsChanneled());
}

void FillActivity(Unit* unit, Player* bot, pb::Activity* activity) {
  if (!unit->IsAlive()) {
    activity->set_state(pb::Activity::DEAD);
    return;
  }
  if (Spell* channel = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL)) {
    activity->set_state(pb::Activity::CHANNELING);
    FillCast(channel, activity->mutable_cast());
    return;
  }
  if (Spell* cast = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL)) {
    activity->set_state(pb::Activity::CASTING);
    FillCast(cast, activity->mutable_cast());
    return;
  }
  Unit* victim = unit->GetVictim();
  if (victim && unit->IsInCombat()) {
    activity->set_state(victim == bot && unit->isMoving()
                            ? pb::Activity::APPROACHING_ME
                            : pb::Activity::FIGHTING);
    activity->set_with(victim->GetName());
    return;
  }
  switch (unit->GetMotionMaster()->GetCurrentMovementGeneratorType()) {
    case FLEEING_MOTION_TYPE:
    case TIMED_FLEEING_MOTION_TYPE:
      activity->set_state(pb::Activity::FLEEING);
      return;
    case WAYPOINT_MOTION_TYPE:
      activity->set_state(pb::Activity::PATROLLING);
      return;
    case FOLLOW_MOTION_TYPE:
      activity->set_state(pb::Activity::FOLLOWING);
      return;
    default:
      break;
  }
  if (unit->isMoving()) {
    activity->set_state(pb::Activity::MOVING);
  } else if (unit->getStandState() == UNIT_STAND_STATE_SIT) {
    activity->set_state(pb::Activity::SITTING);
  } else {
    activity->set_state(pb::Activity::IDLE);
  }
}

void FillVisualState(Unit* unit, pb::Unit* out) {
  if (unit->HasUnitState(UNIT_STATE_STUNNED)) {
    out->add_visual_state(pb::Unit::STUNNED);
  }
  if (unit->HasAuraType(SPELL_AURA_MOD_FEAR)) {
    out->add_visual_state(pb::Unit::FEARED);
  }
  if (unit->HasAuraWithMechanic(1 << MECHANIC_POLYMORPH)) {
    out->add_visual_state(pb::Unit::POLYMORPHED);
  }
  if (unit->HasAuraWithMechanic(1 << MECHANIC_SLEEP)) {
    out->add_visual_state(pb::Unit::ASLEEP);
  }
  if (unit->HasAuraWithMechanic(1 << MECHANIC_BANISH)) {
    out->add_visual_state(pb::Unit::BANISHED);
  }
  if (unit->HasAuraWithMechanic(1 << MECHANIC_SHACKLE)) {
    out->add_visual_state(pb::Unit::SHACKLED);
  }
  if (unit->HasUnitState(UNIT_STATE_ROOT)) {
    out->add_visual_state(pb::Unit::ROOTED);
  }
  if (unit->IsCharmed()) {
    out->add_visual_state(pb::Unit::MIND_CONTROLLED);
  }
  if (unit->HasStealthAura()) {
    out->add_visual_state(pb::Unit::STEALTH_SHIMMER);
  }
}

void FillPack(Creature* creature, pb::Unit* out) {
  CreatureGroup* group = creature->GetFormation();
  if (!group || !group->GetLeader() || group->GetMembers().size() < 2) {
    return;
  }
  out->mutable_pack()->set_id(group->GetLeader()->GetSpawnId());
  out->mutable_pack()->set_size(group->GetMembers().size());
}

// Model-level fields: what anyone can see of the unit at any distance.
void FillModel(Unit* unit, Player* bot, pb::Unit* out) {
  out->set_guid(GuidString(unit->GetGUID()));
  out->set_kind(UnitKind(unit));
  out->set_looks_like(LooksLike(unit));
  out->set_size(SizeFromBoundingRadius(unit->GetBoundaryRadius()));
  out->set_dead(!unit->IsAlive());
  FillActivity(unit, bot, out->mutable_activity());
  FillVisualState(unit, out);
  if (Creature* creature = unit->ToCreature()) {
    FillPack(creature, out);
    out->set_lootable(!unit->IsAlive() && bot->isAllowedToLoot(creature));
  }
}

// Nameplate-level fields (within name / nameplate range).
void FillNameplate(Unit* unit, Player* bot, pb::Unit* out) {
  out->set_name(unit->GetName());
  out->set_level(unit->GetLevel());
  out->set_level_color(LevelColor(bot->GetLevel(), unit->GetLevel()));
  out->set_reaction(Reaction(unit, bot));
  out->set_health_pct(unit->GetHealthPct());
  if (unit->GetMaxPower(unit->getPowerType()) > 0) {
    out->set_power_pct(unit->GetPowerPct(unit->getPowerType()));
  }
  out->set_targeting_me(unit->GetTarget() == bot->GetGUID());
  if (Unit* victim = unit->GetVictim()) {
    out->set_target(victim->GetName());
    if (victim == bot) {
      out->set_threat_status(pb::Unit::THREAT_AGGRO);
    }
  }
  out->set_skinnable(unit->HasUnitFlag(UNIT_FLAG_SKINNABLE));
  if (Player* player = unit->ToPlayer()) {
    out->set_pvp_flagged(player->IsPvP());
    out->mutable_player()->set_class_(ClassName(player->getClass()));
  }
  if (Creature* creature = unit->ToCreature()) {
    out->set_classification(Classification(creature));
    out->set_subtitle(creature->GetCreatureTemplate()->SubName);
    out->set_tapped_by_other(creature->hasLootRecipient() &&
                             !creature->isTappedBy(bot));
    out->set_quest_marker(QuestMarker(bot, creature));
  }
}

// Herbs and ore veins are chests whose lock needs a gathering skill.
pb::GameObject::Kind ChestKind(GameObject* object) {
  if (const LockEntry* lock =
          sLockStore.LookupEntry(object->GetGOInfo()->GetLockId())) {
    for (size_t i = 0; i < MAX_LOCK_CASE; ++i) {
      if (lock->Type[i] != LOCK_KEY_SKILL) {
        continue;
      }
      if (lock->Index[i] == LOCKTYPE_HERBALISM) {
        return pb::GameObject::HERB;
      }
      if (lock->Index[i] == LOCKTYPE_MINING) {
        return pb::GameObject::ORE;
      }
    }
  }
  return pb::GameObject::CHEST;
}

pb::GameObject::Kind ObjectKind(GameObject* object) {
  switch (object->GetGoType()) {
    case GAMEOBJECT_TYPE_DOOR:
    case GAMEOBJECT_TYPE_BUTTON:
      return pb::GameObject::DOOR;
    case GAMEOBJECT_TYPE_MAILBOX:
      return pb::GameObject::MAILBOX;
    case GAMEOBJECT_TYPE_FISHINGNODE:
      return pb::GameObject::BOBBER;
    case GAMEOBJECT_TYPE_FISHINGHOLE:
      return pb::GameObject::FISHING_POOL;
    case GAMEOBJECT_TYPE_MEETINGSTONE:
      return pb::GameObject::MEETING_STONE;
    case GAMEOBJECT_TYPE_SUMMONING_RITUAL:
      return pb::GameObject::RITUAL;
    case GAMEOBJECT_TYPE_BARBER_CHAIR:
      return pb::GameObject::BARBER_CHAIR;
    case GAMEOBJECT_TYPE_GUILD_BANK:
      return pb::GameObject::GUILD_VAULT;
    case GAMEOBJECT_TYPE_TRANSPORT:
    case GAMEOBJECT_TYPE_MO_TRANSPORT:
      return pb::GameObject::TRANSPORT;
    case GAMEOBJECT_TYPE_SPELLCASTER:
      return pb::GameObject::PORTAL;
    case GAMEOBJECT_TYPE_DESTRUCTIBLE_BUILDING:
      return pb::GameObject::DESTRUCTIBLE;
    case GAMEOBJECT_TYPE_CAPTURE_POINT:
      return pb::GameObject::CAPTURE_POINT;
    case GAMEOBJECT_TYPE_CHEST:
      return ChestKind(object);
    case GAMEOBJECT_TYPE_GOOBER:
    case GAMEOBJECT_TYPE_QUESTGIVER:
      return pb::GameObject::QUEST_OBJECT;
    default:
      return pb::GameObject::OTHER;
  }
}

}  // namespace

NearbyBuilder::NearbyBuilder(Options options)
    : options_(options), los_(kLosNearYd, kLosNearTtl, kLosFarTtl) {}

NearbyBuilder::~NearbyBuilder() = default;

NearbyBuilder::Stats NearbyBuilder::Build(Player* bot, pb::Nearby* nearby) {
  Stats stats;
  const VisibleWorldObjectsMap* visible =
      bot->GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
  if (!visible) {
    return stats;
  }
  const base::TimeTicks now = base::TimeTicks::Now();

  // Line of sight first, on the budget, nearest unknown or stale first.
  std::vector<LosCache::Candidate> candidates;
  candidates.reserve(visible->size());
  for (const auto& [guid, object] : *visible) {
    candidates.push_back({guid.GetRawValue(), bot->GetDistance(object)});
  }
  for (uint64_t raw : los_.PickChecks(std::move(candidates), now,
                                      options_.raycasts_per_snapshot)) {
    const auto it = visible->find(ObjectGuid(raw));
    if (it == visible->end()) {
      continue;
    }
    WorldObject* object = it->second;
    // Game objects have no collision height, so the usual check aims at their
    // base on the ground (grazing terrain and steps) and through their own
    // model. A player looks at the object: aim a little above its base and
    // check the map geometry only. Units are also blocked by doors and other
    // game objects.
    const bool in_los =
        object->ToGameObject()
            ? bot->IsWithinLOS(object->GetPositionX(), object->GetPositionY(),
                               object->GetPositionZ() + kObjectSightHeightYd,
                               VMAP::ModelIgnoreFlags::Nothing,
                               LINEOFSIGHT_CHECK_VMAP)
            : bot->IsWithinLOSInMap(object);
    los_.Put(raw, in_los, bot->GetDistance(object), now);
    ++stats.raycasts;
  }
  los_.Prune(now, kLosForgetAfter);

  for (const auto& [guid, object] : *visible) {
    if (object == bot) {
      continue;
    }
    const float dist = bot->GetDistance(object);
    const bool near = dist <= options_.nameplate_range_yd;
    const std::optional<bool> in_los = los_.Get(guid.GetRawValue());
    // Far and not (known to be) in line of sight: a player can't see it.
    if (!near && !in_los.value_or(false)) {
      ++stats.hidden;
      continue;
    }
    if (Unit* unit = object->ToUnit()) {
      pb::Unit* out = nearby->add_units();
      FillModel(unit, bot, out);
      FillRelative(bot, unit, out->mutable_rel());
      out->mutable_rel()->set_in_los(in_los.value_or(true));
      out->mutable_rel()->set_behind_it(unit->isInBack(bot));
      out->mutable_rel()->set_facing_me(unit->isInFront(bot, M_PI / 2));
      if (near) {
        out->set_detail(pb::Unit::NAMEPLATE);
        FillNameplate(unit, bot, out);
      } else {
        out->set_detail(pb::Unit::MODEL);
      }
      ++stats.units;
    } else if (GameObject* go = object->ToGameObject()) {
      pb::GameObject* out = nearby->add_objects();
      out->set_guid(GuidString(guid));
      out->set_name(go->GetName());
      out->set_kind(ObjectKind(go));
      out->set_usable(!go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE));
      out->set_state(go->GetGoState() == GO_STATE_ACTIVE ? "active" : "ready");
      out->set_highlighted(go->ActivateToQuest(bot));
      FillRelative(bot, go, out->mutable_rel());
      out->mutable_rel()->set_in_los(in_los.value_or(true));
      ++stats.objects;
    } else if (DynamicObject* area = object->ToDynObject()) {
      pb::Area* out = nearby->add_areas();
      out->set_guid(GuidString(guid));
      out->set_name(SpellName(area->GetSpellId()));
      out->mutable_spell()->set_id(area->GetSpellId());
      out->mutable_spell()->set_name(out->name());
      out->set_radius_yd(area->GetRadius());
      out->set_inside_me(bot->GetExactDist2d(area) <= area->GetRadius());
      Unit* caster = area->GetCaster();
      out->set_hostile(caster && caster->IsHostileTo(bot));
      out->set_remaining_s(area->GetDuration() / 1000.0f);
      FillRelative(bot, area, out->mutable_rel());
      out->mutable_rel()->set_in_los(in_los.value_or(true));
      ++stats.areas;
    }
  }
  return stats;
}

}  // namespace llm_brain
