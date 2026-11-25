// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_shared.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "AiObjectContext.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "Event.h"
#include "G3D/Vector3.h"
#include "GameObject.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LastMovementValue.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "base/strings/stringprintf.h"
#include "bot_session.h"
#include "options/position_facts.h"

namespace llm_brain {
namespace {

constexpr float kPathEndSlackYd = 3.0f;

bool NeedsGatheringSkill(GameObject* go) {
  const LockEntry* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
  if (!lock) {
    return false;
  }
  for (size_t i = 0; i < MAX_LOCK_CASE; ++i) {
    if (lock->Type[i] != LOCK_KEY_SKILL) {
      continue;
    }
    switch (SkillByLockType(static_cast<LockType>(lock->Index[i]))) {
      case SKILL_HERBALISM:
      case SKILL_MINING:
      case SKILL_SKINNING:
      case SKILL_ENGINEERING:
        return true;
      default:
        break;
    }
  }
  return false;
}

std::string WhyCantUse(Player* bot, GameObject* go) {
  const std::string name = go->GetName();
  const float limit = sPlayerbotAIConfig.LootDistance;
  if (limit > 0 && bot->GetDistance(go) > limit) {
    return base::StringPrintf("%s is %.0f yd away", name.c_str(),
                              bot->GetDistance(go));
  }
  if (go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE)) {
    return name + " can't be used now";
  }
  if (go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) &&
      !go->ActivateToQuest(bot)) {
    return name + " needs a quest you don't have";
  }
  if (bot->IsNonMeleeSpellCast(false)) {
    return "you are casting";
  }
  if (bot->GetLootGUID()) {
    return "you are already looting";
  }
  if (bot->IsInFlight() || bot->m_mover != bot) {
    return "you can't do that now";
  }
  if (IsLootableObject(go)) {
    const float dz = go->GetPositionZ() - bot->GetPositionZ();
    if (std::abs(dz) > INTERACTION_DISTANCE - 2.0f) {
      return base::StringPrintf("%s is %.0f yd %s you", name.c_str(),
                                std::abs(dz), dz > 0 ? "above" : "below");
    }
    LootObject loot(bot, go->GetGUID());
    if (!loot.IsLootPossible(bot)) {
      PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
      auto* bag_space =
          ai ? ai->GetAiObjectContext()->GetValue<uint8>("bag space") : nullptr;
      if (bag_space && bag_space->Get() > 80 && NeedsGatheringSkill(go)) {
        return base::StringPrintf("your bags are %d%% full",
                                  static_cast<int>(bag_space->Get()));
      }
      return "you lack the skill, tool or key for " + name;
    }
  }
  return "";
}

bool WalkNextToObject(Player* bot, WorldObject* target) {
  float x = target->GetPositionX();
  float y = target->GetPositionY();
  float z = target->GetPositionZ();
  target->GetClosePoint(x, y, z, bot->GetObjectSize(), CONTACT_DISTANCE,
                        target->GetAngle(bot) - target->GetOrientation(), bot);
  return WalkTo(bot, x, y, z);
}

}  // namespace

ObjectGuid ParseSnapshotGuid(std::string_view text) {
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
  }
  uint64_t raw = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), raw, 16);
  if (error != std::errc() || end != text.data() + text.size()) {
    return ObjectGuid::Empty;
  }
  return ObjectGuid(raw);
}

std::optional<PartyPingSpot> CurrentPartyPing(Player* bot) {
  if (!bot || !bot->IsInWorld()) {
    return std::nullopt;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  const std::optional<BotSession::PartyPing> ping =
      BotSessionRegistry::Instance().InspectSession(
          key,
          [](const BotSession* s) { return s ? s->party_ping : std::nullopt; });
  if (!ping) {
    return std::nullopt;
  }
  const uint32_t age_ms = GetMSTimeDiffToNow(ping->ms);
  const Group* group = bot->GetGroup();
  if (age_ms > kPartyPingShowMs || !group ||
      !group->IsMember(ObjectGuid(ping->from_guid)) ||
      ping->map_id != bot->GetMapId() ||
      ping->instance_id != bot->GetInstanceId()) {
    BotSessionRegistry::Instance().WithSession(key, [&ping](BotSession& s) {
      if (s.party_ping == ping) {
        s.party_ping.reset();
      }
    });
    return std::nullopt;
  }
  float z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), ping->x, ping->y,
                                     bot->GetPositionZ() + 5.0f);
  if (!std::isfinite(z) || z <= INVALID_HEIGHT) {
    z = bot->GetPositionZ();
  }
  return PartyPingSpot{
      .from = ping->from, .x = ping->x, .y = ping->y, .z = z, .age_ms = age_ms};
}

bool CanClearBotMovement(const Player* bot) {
  if (!bot || !bot->IsInWorld()) {
    return false;
  }
  if (bot->IsInFlight() || bot->GetVehicle() != nullptr ||
      bot->HasUnitState(UNIT_STATE_IN_FLIGHT | UNIT_STATE_FLEEING |
                        UNIT_STATE_CONFUSED | UNIT_STATE_STUNNED |
                        UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED |
                        UNIT_STATE_DISTRACTED)) {
    return false;
  }
  if (const MotionMaster* mm = bot->GetMotionMaster();
      mm && mm->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE) {
    return false;
  }
  return true;
}

void RecordLastMovement(Player* bot, float x, float y, float z) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return;
  }
  auto* last =
      ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement");
  if (!last) {
    return;
  }
  const float speed = bot->GetSpeed(MOVE_RUN);
  const float ms =
      speed > 0 ? bot->GetExactDist(x, y, z) / speed * 1000.0f : 1000.0f;
  last->Get().Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), ms + 500.0f,
                  MovementPriority::MOVEMENT_NORMAL);
}

void ClearLastMovement(Player* bot) {
  if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
    if (auto* last = ai->GetAiObjectContext()->GetValue<LastMovement&>(
            "last movement")) {
      last->Get().clear();
    }
  }
}

bool WalkTo(Player* bot, float x, float y, float z) {
  PathGenerator path(bot);
  path.CalculatePath(x, y, z);
  const uint32_t type = path.GetPathType();
  const G3D::Vector3& end = path.GetActualEndPosition();
  if ((type & PATHFIND_NOT_USING_PATH) == 0 &&
      ((type & PATHFIND_NOPATH) != 0 ||
       std::hypot(end.x - x, end.y - y) > kPathEndSlackYd)) {
    return false;
  }
  bot->GetMotionMaster()->MovePoint(/*id=*/0, x, y, z, FORCED_MOVEMENT_NONE,
                                    /*speed=*/0.0f, /*orientation=*/0.0f,
                                    /*generatePath=*/true,
                                    /*forceDestination=*/false);
  RecordLastMovement(bot, x, y, z);
  return true;
}

bool WalkNextTo(Player* bot, Creature* target) {
  return WalkNextToObject(bot, target);
}

bool WalkNextTo(Player* bot, GameObject* target) {
  return WalkNextToObject(bot, target);
}

bool WalkNextTo(Player* bot, Unit* target) {
  return WalkNextToObject(bot, target);
}

bool WalkTowards(Player* bot, float x, float y, float z) {
  return WalkTo(bot, x, y, z);
}

void ClearStaleCombatTarget(Player* bot) {
  if (!bot) {
    return;
  }
  Unit* sel = bot->GetSelectedUnit();
  if (ShouldClearStaleCombatTarget(bot->IsInCombat(), bot->IsAlive(),
                                   sel != nullptr, sel && sel->IsAlive(),
                                   sel && bot->IsFriendlyTo(sel))) {
    bot->SetSelection(ObjectGuid::Empty);
  }
  if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
    if (AiObjectContext* ctx = ai->GetAiObjectContext()) {
      if (auto* cur = ctx->GetValue<Unit*>("current target")) {
        Unit* tgt = cur->Get();
        if (ShouldClearStaleCombatTarget(bot->IsInCombat(), bot->IsAlive(),
                                         tgt != nullptr, tgt && tgt->IsAlive(),
                                         tgt && bot->IsFriendlyTo(tgt))) {
          cur->Set(nullptr);
        }
      }
    }
  }
}

ActionOutcome MarkUnitWithIcon(Player* bot, Unit* unit, uint8_t icon) {
  Group* group = bot->GetGroup();
  if (!group) {
    return {.error = "not in a group"};
  }
  if (group->isRaidGroup() && !group->IsLeader(bot->GetGUID()) &&
      !group->IsAssistant(bot->GetGUID())) {
    return {.error = "must be raid leader or assistant to mark"};
  }
  if (icon >= TARGETICONCOUNT) {
    return {.error = "invalid raid mark"};
  }
  if (!unit || !unit->IsInWorld() || !unit->IsAlive()) {
    return {.error = "target is gone"};
  }
  group->SetTargetIcon(icon, bot->GetGUID(), unit->GetGUID());
  static constexpr const char* kIconNames[8] = {"Star",     "Circle", "Diamond",
                                                "Triangle", "Moon",   "Square",
                                                "Cross",    "Skull"};
  return {.used = true,
          .info = "marked " + unit->GetName() + " with " + kIconNames[icon]};
}

bool PickEquipSlot(Player* bot,
                   Item* bag_item,
                   const ItemTemplate* proto,
                   uint16_t* out_dest) {
  auto try_dual = [&](uint8_t s1, uint8_t s2) -> bool {
    const Item* i1 = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s1);
    const Item* i2 = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s2);
    const uint32_t l1 =
        i1 && i1->GetTemplate() ? i1->GetTemplate()->ItemLevel : 0;
    const uint32_t l2 =
        i2 && i2->GetTemplate() ? i2->GetTemplate()->ItemLevel : 0;
    const uint8_t preferred = (l2 < l1) ? s2 : s1;
    const uint8_t alternate = (preferred == s1) ? s2 : s1;
    if (bot->CanEquipItem(preferred, *out_dest, bag_item, true) ==
        EQUIP_ERR_OK) {
      return true;
    }
    return bot->CanEquipItem(alternate, *out_dest, bag_item, true) ==
           EQUIP_ERR_OK;
  };
  if (proto->InventoryType == INVTYPE_FINGER) {
    return try_dual(EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2);
  }
  if (proto->InventoryType == INVTYPE_TRINKET) {
    return try_dual(EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2);
  }
  if ((proto->InventoryType == INVTYPE_WEAPON && bot->CanDualWield()) ||
      (proto->InventoryType == INVTYPE_2HWEAPON && bot->CanDualWield() &&
       bot->CanTitanGrip())) {
    return try_dual(EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND);
  }
  return bot->CanEquipItem(NULL_SLOT, *out_dest, bag_item, true) ==
         EQUIP_ERR_OK;
}

ActionOutcome EquipBagItem(Player* bot,
                           Item* bag_item,
                           uint8_t bag,
                           uint8_t slot,
                           std::string_view label) {
  const ItemTemplate* proto = bag_item ? bag_item->GetTemplate() : nullptr;
  uint16_t dest = 0;
  if (!proto || !PickEquipSlot(bot, bag_item, proto, &dest)) {
    return {.error = "cannot equip " + std::string(label)};
  }
  const ObjectGuid src_guid = bag_item->GetGUID();
  const uint16_t src =
      static_cast<uint16_t>((static_cast<uint16_t>(bag) << 8) | slot);
  bot->SwapItem(src, dest);
  Item* now_equipped = bot->GetItemByPos(dest);
  if (!now_equipped || now_equipped->GetGUID() != src_guid) {
    return {.error = "couldn't equip " + std::string(label)};
  }
  return {.used = true, .info = "equipped " + std::string(label)};
}

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

bool IsPreparingOrChannelingSpell(const Player* bot, uint32_t spell_id) {
  if (!bot || spell_id == 0) {
    return false;
  }
  if (const Spell* g = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
      g && g->getState() == SPELL_STATE_PREPARING && g->GetSpellInfo() &&
      g->GetSpellInfo()->Id == spell_id) {
    return true;
  }
  if (const Spell* c = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
      c && c->getState() != SPELL_STATE_FINISHED && c->GetSpellInfo() &&
      c->GetSpellInfo()->Id == spell_id) {
    return true;
  }
  return false;
}

bool IsSpellCastPending(const Player* bot, uint32_t spell_id) {
  return QueuedSpellCount(bot, spell_id) > 0 ||
         IsPreparingOrChannelingSpell(bot, spell_id);
}

bool IsLootableObject(GameObject* go) {
  return go->GetGoType() == GAMEOBJECT_TYPE_CHEST ||
         go->GetGOInfo()->GetLootId() != 0;
}

bool IsInUseRange(Player* bot, GameObject* go) {
  if (!IsLootableObject(go)) {
    return go->IsAtInteractDistance(bot);
  }
  const float limit = sPlayerbotAIConfig.LootDistance;
  return limit <= 0 || bot->GetDistance(go) <= std::max(limit - 1.0f, 1.0f);
}

std::string UseObjectNow(Player* bot, GameObject* go) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return "not a bot";
  }
  if (std::string why = WhyCantUse(bot, go); !why.empty()) {
    return why;
  }
  ClearLastMovement(bot);
  const std::string link =
      "|cffffff00|Hfound:" + std::to_string(go->GetGUID().GetRawValue()) + ":" +
      std::to_string(go->GetEntry()) + ":|h[" + go->GetName() + "]|h|r";
  if (!ai->DoSpecificAction("use", Event("llm-brain", link), /*silent=*/true)) {
    return "can't use " + go->GetName() + " now";
  }
  ai->ResetActionDuration();
  return "";
}

bool IsInTalkRange(Player* bot, Creature* npc) {
  return npc->IsWithinDistInMap(bot, INTERACTION_DISTANCE);
}

bool IsInLootRange(Player* bot, Creature* creature) {
  return creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE - 0.5f);
}

}  // namespace llm_brain
