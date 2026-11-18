// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/nearby_builder.h"
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
#include "UpdateFields.h"
#include "World.h"
#include "base/time/time.h"
#include "proto/snapshot.h"
#include "snapshot/perception.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr float kLosNearYd = 40;
constexpr base::TimeDelta kLosNearTtl = base::Seconds(1);
constexpr base::TimeDelta kLosFarTtl = base::Seconds(5);
// Line-of-sight results are re-checked once the bot has moved this far.
constexpr float kLosMovedYd = 3;
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
  rel->dist_yd = bot->GetDistance(object);
  rel->dir = CompassDirection(dx, dy);
  rel->height = HeightOf(object->GetPositionZ() - bot->GetPositionZ());
  rel->position.x = object->GetPositionX();
  rel->position.y = object->GetPositionY();
  rel->position.z = object->GetPositionZ();
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

// Line of sight only when it was checked: unset means "not known yet" (the
// field has explicit presence), never a guess.
void SetInLos(std::optional<bool> in_los, pb::Relative* rel) {
  if (in_los) {
    rel->in_los = *in_los;
  }
}

// Whether the caster can't be interrupted right now: the client shows the
// shield on its cast bar.
bool ImmuneToInterrupt(Unit* unit) {
  return unit->m_spellImmune[IMMUNITY_MECHANIC].count(MECHANIC_INTERRUPT) > 0 ||
         unit->m_spellImmune[IMMUNITY_EFFECT].count(
             SPELL_EFFECT_INTERRUPT_CAST) > 0 ||
         unit->HasAuraTypeWithMiscvalue(SPELL_AURA_MECHANIC_IMMUNITY,
                                        MECHANIC_INTERRUPT) ||
         unit->HasAuraTypeWithMiscvalue(SPELL_AURA_EFFECT_IMMUNITY,
                                        SPELL_EFFECT_INTERRUPT_CAST);
}

// The rule of AzerothCore's own interrupt (Spell::EffectInterruptCast): only a
// spell silence can stop, by its cast or its channel interrupt flag.
bool Interruptible(Unit* unit, const SpellInfo* info) {
  if (info->PreventionType != SPELL_PREVENTION_TYPE_SILENCE) {
    return false;
  }
  const bool flagged =
      info->IsChanneled()
          ? (info->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT) !=
                0
          : (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0;
  return flagged && !ImmuneToInterrupt(unit);
}

void FillCast(Unit* unit, Spell* spell, pb::Cast* cast) {
  const SpellInfo* info = spell->GetSpellInfo();
  pb::Spell& sp = cast->spell.emplace();
  sp.id = info->Id;
  sp.name = Localized(info->SpellName);
  cast->remaining_ms =
      static_cast<uint32_t>(std::max(0, spell->GetCastTimeRemaining()));
  cast->interruptible = Interruptible(unit, info);
  cast->channeled = info->IsChanneled();
}

void FillActivity(Unit* unit, Player* bot, pb::Activity* activity) {
  if (!unit->IsAlive()) {
    activity->state = pb::Activity::DEAD;
    return;
  }
  // What shows a cast bar, as in Unit::IsNonMeleeSpellCast: a spell stays in
  // its slot after it finished, while its projectile flies (delayed) and for
  // an instant cast, none of which a player sees as casting.
  Spell* channel = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
  if (channel && channel->getState() != SPELL_STATE_FINISHED) {
    activity->state = pb::Activity::CHANNELING;
    FillCast(unit, channel, &activity->cast.emplace());
    return;
  }
  Spell* cast = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
  if (cast && cast->getState() == SPELL_STATE_PREPARING &&
      cast->GetCastTime() > 0) {
    activity->state = pb::Activity::CASTING;
    FillCast(unit, cast, &activity->cast.emplace());
    return;
  }
  Unit* victim = unit->GetVictim();
  if (victim && unit->IsInCombat()) {
    activity->state = victim == bot && unit->isMoving()
                          ? pb::Activity::APPROACHING_ME
                          : pb::Activity::FIGHTING;
    activity->with = victim->GetName();
    return;
  }
  switch (unit->GetMotionMaster()->GetCurrentMovementGeneratorType()) {
    case FLEEING_MOTION_TYPE:
    case TIMED_FLEEING_MOTION_TYPE:
      activity->state = pb::Activity::FLEEING;
      return;
    case WAYPOINT_MOTION_TYPE:
      activity->state = pb::Activity::PATROLLING;
      return;
    case FOLLOW_MOTION_TYPE:
      activity->state = pb::Activity::FOLLOWING;
      return;
    default:
      break;
  }
  if (unit->isMoving()) {
    activity->state = pb::Activity::MOVING;
  } else if (unit->getStandState() == UNIT_STAND_STATE_SIT) {
    activity->state = pb::Activity::SITTING;
  } else {
    activity->state = pb::Activity::IDLE;
  }
}

void FillVisualState(Unit* unit, pb::Unit* out) {
  if (unit->HasUnitState(UNIT_STATE_STUNNED)) {
    out->visual_state.push_back(pb::Unit::STUNNED);
  }
  if (unit->HasAuraType(SPELL_AURA_MOD_FEAR)) {
    out->visual_state.push_back(pb::Unit::FEARED);
  }
  if (unit->HasAuraWithMechanic(1 << MECHANIC_POLYMORPH)) {
    out->visual_state.push_back(pb::Unit::POLYMORPHED);
  }
  if (unit->HasAuraWithMechanic(1 << MECHANIC_SLEEP)) {
    out->visual_state.push_back(pb::Unit::ASLEEP);
  }
  if (unit->HasAuraWithMechanic(1 << MECHANIC_BANISH)) {
    out->visual_state.push_back(pb::Unit::BANISHED);
  }
  if (unit->HasAuraWithMechanic(1 << MECHANIC_SHACKLE)) {
    out->visual_state.push_back(pb::Unit::SHACKLED);
  }
  if (unit->HasUnitState(UNIT_STATE_ROOT)) {
    out->visual_state.push_back(pb::Unit::ROOTED);
  }
  if (unit->IsCharmed()) {
    out->visual_state.push_back(pb::Unit::MIND_CONTROLLED);
  }
  if (unit->HasStealthAura()) {
    out->visual_state.push_back(pb::Unit::STEALTH_SHIMMER);
  }
}

void FillPack(Creature* creature, pb::Unit* out) {
  CreatureGroup* group = creature->GetFormation();
  if (!group || !group->GetLeader() || group->GetMembers().size() < 2) {
    return;
  }
  pb::Unit::Pack& pack = out->pack.emplace();
  pack.id = group->GetLeader()->GetSpawnId();
  pack.size = static_cast<uint32_t>(group->GetMembers().size());
}

// Model-level fields: what anyone can see of the unit at any distance.
void FillModel(Unit* unit, Player* bot, pb::Unit* out) {
  out->guid = GuidString(unit->GetGUID());
  out->kind = UnitKind(unit);
  out->looks_like = LooksLike(unit);
  out->size = SizeFromBoundingRadius(unit->GetBoundaryRadius());
  out->dead = !unit->IsAlive();
  FillActivity(unit, bot, &out->activity);
  FillVisualState(unit, out);
  if (Creature* creature = unit->ToCreature()) {
    FillPack(creature, out);
    out->lootable = !unit->IsAlive() && bot->isAllowedToLoot(creature);
  }
}

// Nameplate-level fields (within name / nameplate range).
void FillNameplate(Unit* unit, Player* bot, pb::Unit* out) {
  out->name = unit->GetName();
  out->level = unit->GetLevel();
  out->level_color = LevelColor(bot->GetLevel(), unit->GetLevel());
  out->reaction = Reaction(unit, bot);
  out->health_pct = unit->GetHealthPct();
  if (unit->GetMaxPower(unit->getPowerType()) > 0) {
    out->power_pct = unit->GetPowerPct(unit->getPowerType());
  }
  out->targeting_me = unit->GetTarget() == bot->GetGUID();
  if (Unit* victim = unit->GetVictim()) {
    out->target = victim->GetName();
    if (victim == bot) {
      out->threat_status = pb::Unit::THREAT_AGGRO;
    }
  }
  out->skinnable = unit->HasUnitFlag(UNIT_FLAG_SKINNABLE);
  if (Player* player = unit->ToPlayer()) {
    out->pvp_flagged = player->IsPvP();
    out->player.emplace().class_ = ClassName(player->getClass());
  }
  if (Creature* creature = unit->ToCreature()) {
    out->classification = Classification(creature);
    out->subtitle = creature->GetCreatureTemplate()->SubName;
    out->tapped_by_other =
        creature->hasLootRecipient() && !creature->isTappedBy(bot);
    out->quest_marker = QuestMarker(bot, creature);
  }
}

// Herbs and ore veins are chests whose lock needs a gathering skill.
std::pair<pb::GameObject::Kind, uint32_t> ChestKindAndSkill(
    GameObject* object) {
  if (const LockEntry* lock =
          sLockStore.LookupEntry(object->GetGOInfo()->GetLockId())) {
    for (size_t i = 0; i < MAX_LOCK_CASE; ++i) {
      if (lock->Type[i] != LOCK_KEY_SKILL) {
        continue;
      }
      if (lock->Index[i] == LOCKTYPE_HERBALISM) {
        return {pb::GameObject::HERB, lock->Skill[i]};
      }
      if (lock->Index[i] == LOCKTYPE_MINING) {
        return {pb::GameObject::ORE, lock->Skill[i]};
      }
    }
  }
  return {pb::GameObject::CHEST, 0};
}

pb::GameObject::Kind ObjectKind(GameObject* object,
                                uint32_t* required_skill = nullptr) {
  if (required_skill) {
    *required_skill = 0;
  }
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
    case GAMEOBJECT_TYPE_CHEST: {
      const auto [kind, skill] = ChestKindAndSkill(object);
      if (required_skill) {
        *required_skill = skill;
      }
      return kind;
    }
    case GAMEOBJECT_TYPE_GOOBER:
    case GAMEOBJECT_TYPE_QUESTGIVER:
      return pb::GameObject::QUEST_OBJECT;
    default:
      return pb::GameObject::OTHER;
  }
}

// Whether the client gives `go` a click cursor for `bot`: selectable, any
// quest condition met (e.g. Cactus Apple requires its quest), and not an
// already-opened chest/herb/vein.
bool IsObjectUsable(Player* bot, GameObject* go) {
  if (go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE)) {
    return false;
  }
  if (go->HasGameObjectFlag(GO_FLAG_INTERACT_COND) &&
      !go->ActivateToQuest(bot)) {
    return false;
  }
  if (go->GetGoType() == GAMEOBJECT_TYPE_CHEST ||
      go->GetGOInfo()->GetLootId() != 0) {
    return go->getLootState() == GO_READY;
  }
  return true;
}

// Triggers and invisible-model creatures are on the visible list (the client
// is sent them) but draw nothing: a player sees nobody there.
bool HiddenFromPlayer(WorldObject* object) {
  Creature* creature = object->ToCreature();
  return creature &&
         HiddenFromPlayers(creature->IsTrigger(), creature->GetDisplayId());
}

}  // namespace

NearbyBuilder::NearbyBuilder(Options options)
    : options_(options), los_(kLosNearYd, kLosNearTtl, kLosFarTtl) {}

NearbyBuilder::~NearbyBuilder() = default;

void NearbyBuilder::FillUnit(Player* bot, Unit* unit, pb::Unit* out) {
  out->detail = pb::Unit::NAMEPLATE;
  FillModel(unit, bot, out);
  FillUnitRelative(bot, unit, &out->rel);
  out->rel.behind_it = unit->isInBack(bot);
  out->rel.facing_me = unit->isInFront(bot, M_PI / 2);
  FillNameplate(unit, bot, out);
}

void NearbyBuilder::FillUnitRelative(Player* bot,
                                     WorldObject* object,
                                     pb::Relative* rel) {
  FillRelative(bot, object, rel);
  SetInLos(los_.Get(object->GetGUID().GetRawValue()), rel);
}

NearbyBuilder::Stats NearbyBuilder::Build(Player* bot, pb::Nearby* nearby) {
  Stats stats;
  const VisibleWorldObjectsMap* visible =
      bot->GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
  if (!visible) {
    return stats;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  los_.SetViewer(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                 bot->GetPositionZ(), kLosMovedYd);

  // Line of sight first, on the budget, nearest unknown or stale first.
  std::vector<LosCache::Candidate> candidates;
  candidates.reserve(visible->size());
  for (const auto& [guid, object] : *visible) {
    if (HiddenFromPlayer(object)) {
      continue;  // Not worth a ray cast.
    }
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
    if (HiddenFromPlayer(object)) {
      ++stats.hidden;
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
      pb::Unit& out = nearby->units.emplace_back();
      FillModel(unit, bot, &out);
      FillRelative(bot, unit, &out.rel);
      SetInLos(in_los, &out.rel);
      out.rel.behind_it = unit->isInBack(bot);
      out.rel.facing_me = unit->isInFront(bot, M_PI / 2);
      if (near) {
        out.detail = pb::Unit::NAMEPLATE;
        FillNameplate(unit, bot, &out);
      } else {
        out.detail = pb::Unit::MODEL;
      }
      ++stats.units;
    } else if (GameObject* go = object->ToGameObject()) {
      pb::GameObject& out = nearby->objects.emplace_back();
      out.guid = GuidString(guid);
      out.name = go->GetName();
      out.kind = ObjectKind(go, &out.required_skill);
      out.usable = IsObjectUsable(bot, go);
      out.state = go->GetGoState() == GO_STATE_ACTIVE ? "active" : "ready";
      out.highlighted = go->ActivateToQuest(bot);
      FillRelative(bot, go, &out.rel);
      SetInLos(in_los, &out.rel);
      ++stats.objects;
    } else if (DynamicObject* area = object->ToDynObject()) {
      if (area->GetByteValue(DYNAMICOBJECT_BYTES, 0) !=
          DYNAMIC_OBJECT_AREA_SPELL) {
        continue;  // A far-sight viewpoint: nothing a player sees.
      }
      pb::Area& out = nearby->areas.emplace_back();
      out.guid = GuidString(guid);
      out.name = SpellName(area->GetSpellId());
      pb::Spell& sp = out.spell.emplace();
      sp.id = area->GetSpellId();
      sp.name = out.name;
      out.radius_yd = area->GetRadius();
      out.inside_me = bot->GetExactDist2d(area) <= area->GetRadius();
      Unit* caster = area->GetCaster();
      out.hostile = caster && caster->IsHostileTo(bot);
      if (area->GetDuration() > 0) {  // -1: lasts until removed.
        out.remaining_s = static_cast<float>(area->GetDuration()) / 1000.0f;
      }
      pb::Relative& rel = out.rel.emplace();
      FillRelative(bot, area, &rel);
      SetInLos(in_los, &rel);
      ++stats.areas;
    }
  }
  return stats;
}

}  // namespace llm_brain
