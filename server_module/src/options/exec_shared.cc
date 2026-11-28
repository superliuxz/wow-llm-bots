// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_shared.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "Bag.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "Event.h"
#include "G3D/Vector3.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LastMovementValue.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "ThreatManager.h"
#include "Timer.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/strings/stringprintf.h"
#include "bot_manager/pull_discipline.h"
#include "bot_session.h"
#include "options/npc_gossip_and_bags.h"
#include "options/position_facts.h"
#include "snapshot/perception.h"
#include "snapshot/world_util.h"

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
  if (!IsLootableObject(go)) {
    if (!go->IsAtInteractDistance(bot)) {
      return base::StringPrintf("%s is %.0f yd away", name.c_str(),
                                bot->GetDistance(go));
    }
  } else {
    const float limit = sPlayerbotAIConfig.LootDistance;
    if (limit > 0 && bot->GetDistance(go) > limit) {
      return base::StringPrintf("%s is %.0f yd away", name.c_str(),
                                bot->GetDistance(go));
    }
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

std::optional<std::pair<uint8_t, uint8_t>> FindFreeGeneralSlotOutsideBag(
    const Player* bot,
    uint8_t exclude_bag) {
  if (!bot) {
    return std::nullopt;
  }
  if (exclude_bag != INVENTORY_SLOT_BAG_0) {
    for (uint8_t s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END;
         ++s) {
      if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s) == nullptr) {
        return std::make_pair(static_cast<uint8_t>(INVENTORY_SLOT_BAG_0), s);
      }
    }
  }
  for (uint8_t b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b) {
    if (b == exclude_bag) {
      continue;
    }
    const Bag* bag = bot->GetBagByPos(b);
    if (!IsGeneralBag(bag)) {
      continue;
    }
    for (uint32_t s = 0; s < bag->GetBagSize(); ++s) {
      if (bag->GetItemByPos(static_cast<uint8_t>(s)) == nullptr) {
        return std::make_pair(b, static_cast<uint8_t>(s));
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::vector<EquippedBagSlotInfo> ReadEquippedBagSlots(const Player* bot) {
  std::vector<EquippedBagSlotInfo> out;
  if (!bot) {
    return out;
  }
  out.reserve(INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START);
  for (uint8_t slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END;
       ++slot) {
    EquippedBagSlotInfo info;
    info.slot = slot;
    const Bag* bag = bot->GetBagByPos(slot);
    if (bag) {
      info.occupied = true;
      info.is_general_bag = IsGeneralBag(bag);
      info.container_slots = bag->GetBagSize();
      uint32_t used = 0;
      for (uint32_t i = 0; i < bag->GetBagSize(); ++i) {
        if (bag->GetItemByPos(static_cast<uint8_t>(i)) != nullptr) {
          ++used;
        }
      }
      info.used_slots = used;
      if (const ItemTemplate* p = bag->GetTemplate()) {
        info.name = p->Name1;
      }
    }
    out.push_back(std::move(info));
  }
  return out;
}

bool HasFreeSlotOutsideBag(const Player* bot, uint8_t exclude_bag) {
  return FindFreeGeneralSlotOutsideBag(bot, exclude_bag).has_value();
}

std::vector<uint32_t> CollectActiveQuestItemIds(const Player* bot) {
  std::vector<uint32_t> ids;
  if (!bot) {
    return ids;
  }
  for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot) {
    const uint32_t qid = bot->GetQuestSlotQuestId(slot);
    if (qid == 0) {
      continue;
    }
    const Quest* q = sObjectMgr->GetQuestTemplate(qid);
    if (!q) {
      continue;
    }
    if (const uint32_t src = q->GetSrcItemId(); src != 0) {
      ids.push_back(src);
    }
    for (size_t i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i) {
      if (q->RequiredItemId[i] != 0) {
        ids.push_back(q->RequiredItemId[i]);
      }
    }
    for (size_t i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i) {
      if (q->ItemDrop[i] != 0) {
        ids.push_back(q->ItemDrop[i]);
      }
    }
  }
  return ids;
}

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

std::optional<RecentSpellMiss> CurrentSpellMiss(Player* bot) {
  if (!bot || !bot->IsInWorld()) {
    return std::nullopt;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  const std::optional<BotSession::SpellMiss> miss =
      BotSessionRegistry::Instance().InspectSession(
          key,
          [](const BotSession* s) { return s ? s->spell_miss : std::nullopt; });
  if (!miss) {
    return std::nullopt;
  }
  const uint32_t age_ms = GetMSTimeDiffToNow(miss->ms);
  if (age_ms > kSpellMissShowMs || miss->map_id != bot->GetMapId() ||
      miss->instance_id != bot->GetInstanceId()) {
    BotSessionRegistry::Instance().WithSession(key, [&miss](BotSession& s) {
      if (s.spell_miss == miss) {
        s.spell_miss.reset();
      }
    });
    return std::nullopt;
  }
  return RecentSpellMiss{
      .seq = miss->seq,
      .spell = miss->spell,
      .target = miss->target,
      .reason = miss->reason,
  };
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
  if (proto->InventoryType == INVTYPE_BAG) {
    if (proto->Class != ITEM_CLASS_CONTAINER ||
        proto->SubClass != ITEM_SUBCLASS_CONTAINER ||
        proto->ContainerSlots == 0) {
      return false;
    }
    if (bag_item && bag_item->IsNotEmptyBag()) {
      return false;
    }
    const uint8_t src_bag = bag_item ? bag_item->GetBagSlot() : NULL_BAG;
    const std::optional<BagEquipTarget> target = PickBagEquipSlot(
        proto->ContainerSlots, src_bag, HasFreeSlotOutsideBag(bot, src_bag),
        ReadEquippedBagSlots(bot));
    if (!target) {
      return false;
    }
    return bot->CanEquipItem(target->dest_slot, *out_dest, bag_item,
                             target->replaces_existing) == EQUIP_ERR_OK;
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
  uint8_t cur_bag = bag;
  uint8_t cur_slot = slot;
  if (proto->InventoryType == INVTYPE_BAG &&
      cur_bag == static_cast<uint8_t>(dest & 0xFF)) {
    const std::optional<std::pair<uint8_t, uint8_t>> staging =
        FindFreeGeneralSlotOutsideBag(bot, cur_bag);
    if (!staging) {
      return {.error = "cannot equip " + std::string(label)};
    }
    const uint16_t old_src =
        static_cast<uint16_t>((static_cast<uint16_t>(cur_bag) << 8) | cur_slot);
    const uint16_t stage_dst = static_cast<uint16_t>(
        (static_cast<uint16_t>(staging->first) << 8) | staging->second);
    bot->SwapItem(old_src, stage_dst);
    Item* moved = bot->GetItemByPos(staging->first, staging->second);
    if (!moved || moved->GetGUID() != src_guid) {
      return {.error = "couldn't equip " + std::string(label)};
    }
    cur_bag = staging->first;
    cur_slot = staging->second;
  }
  const uint16_t src =
      static_cast<uint16_t>((static_cast<uint16_t>(cur_bag) << 8) | cur_slot);
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
  if (go->GetGoType() == GAMEOBJECT_TYPE_SUMMONING_RITUAL) {
    const GameObjectTemplate* info = go->GetGOInfo();
    Unit* owner = go->GetOwner();
    Player* owner_player = owner ? owner->ToPlayer() : nullptr;
    if (!IsSummoningRitualUsable({
            .loot_state_ready = go->getLootState() == GO_READY,
            .unique_use_count = go->GetUniqueUseCount(),
            .req_participants =
                info ? info->summoningRitual.reqParticipants : 0u,
            .has_owner = owner != nullptr,
            .owner_is_player = owner_player != nullptr,
            .bot_is_owner = owner_player == bot,
            .owner_channeling =
                owner != nullptr &&
                owner->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr,
            .casters_grouped =
                !info || info->summoningRitual.castersGrouped != 0,
            .in_same_raid_with_owner =
                owner_player != nullptr && bot->IsInSameRaidWith(owner_player),
            .bot_channeling =
                bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr,
        })) {
      return go->GetName() + " is no longer active";
    }
    WorldSession* session = bot->GetSession();
    if (!session) {
      return "no session";
    }
    if (bot->isMoving()) {
      bot->StopMoving();
    }
    WorldPacket pkt(CMSG_GAMEOBJ_USE, 8);
    pkt << go->GetGUID();
    pkt.rpos(0);
    session->HandleGameObjectUseOpcode(pkt);
    ai->ResetActionDuration();
    return "";
  }
  if (go->GetGoType() == GAMEOBJECT_TYPE_MEETINGSTONE) {
    const GameObjectTemplate* info = go->GetGOInfo();
    const uint8_t min_level =
        info ? static_cast<uint8_t>(info->meetingstone.minLevel) : 0;
    Group* group = bot->GetGroup();
    if (!group) {
      return "you are not in a group";
    }
    if (bot->GetLevel() < min_level) {
      return "your level is too low for " + go->GetName();
    }
    Player* target = ObjectAccessor::FindConnectedPlayer(bot->GetTarget());
    if (!target || target == bot || !target->IsInWorld() ||
        !target->IsInSameRaidWith(bot) || target->GetLevel() < min_level) {
      target = nullptr;
      float best_dist = -1.0f;
      for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
        if (slot.guid == bot->GetGUID()) {
          continue;
        }
        Player* m = ObjectAccessor::FindConnectedPlayer(slot.guid);
        if (!m || !m->IsInWorld() || m->GetLevel() < min_level) {
          continue;
        }
        const float dist = (m->GetMapId() == bot->GetMapId() &&
                            m->GetInstanceId() == bot->GetInstanceId())
                               ? bot->GetDistance(m)
                               : std::numeric_limits<float>::infinity();
        if (!target || dist > best_dist) {
          target = m;
          best_dist = dist;
        }
      }
    }
    if (!target) {
      return "no eligible group member to summon at " + go->GetName();
    }
    WorldSession* session = bot->GetSession();
    if (!session) {
      return "no session";
    }
    if (bot->isMoving()) {
      bot->StopMoving();
    }
    bot->SetSelection(target->GetGUID());
    WorldPacket pkt(CMSG_GAMEOBJ_USE, 8);
    pkt << go->GetGUID();
    pkt.rpos(0);
    session->HandleGameObjectUseOpcode(pkt);
    ai->ResetActionDuration();
    return "";
  }
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

void EvaluateMobCombatWithGroup(Player* bot,
                                Group* group,
                                const std::vector<Player*>& same_map_members,
                                Unit* unit,
                                MobPullInput* out) {
  if (!bot || !group || !unit || !out) {
    return;
  }
  const auto classify_target = [&](Unit* target) {
    if (!target || !target->IsInWorld() ||
        target->GetMapId() != bot->GetMapId() ||
        target->GetInstanceId() != bot->GetInstanceId()) {
      return;
    }
    if (Player* p = target->ToPlayer()) {
      if (group->IsMember(p->GetGUID())) {
        out->fights_party_member = true;
      }
      return;
    }
    if (Player* owner = target->GetCharmerOrOwnerPlayerOrPlayerItself()) {
      if (group->IsMember(owner->GetGUID())) {
        out->fights_party_pet_or_totem = true;
      }
      return;
    }
    if (Creature* c = target->ToCreature()) {
      if (c->IsCritter() || c->IsTrigger() || !bot->IsFriendlyTo(c)) {
        return;
      }
      float min_dist = std::numeric_limits<float>::infinity();
      for (Player* m : same_map_members) {
        if (m && m->IsInWorld()) {
          min_dist = std::min(min_dist, c->GetDistance(m));
        }
      }
      if (!std::isfinite(min_dist)) {
        min_dist = c->GetDistance(bot);
      }
      const bool is_escort =
          c->AI() && (c->AI()->IsEscorted() || c->AI()->IsEscortNPC(true));
      if (is_escort) {
        if (!out->fights_escort_npc ||
            min_dist < out->escort_npc_dist_to_party_yd) {
          out->fights_escort_npc = true;
          out->escort_npc_dist_to_party_yd = min_dist;
        }
      } else {
        if (!out->fights_friendly_npc ||
            min_dist < out->friendly_npc_dist_to_party_yd) {
          out->fights_friendly_npc = true;
          out->friendly_npc_dist_to_party_yd = min_dist;
        }
      }
    }
  };

  if (Unit* victim = unit->GetVictim()) {
    classify_target(victim);
  }
  if (!unit->IsInCombat()) {
    return;
  }
  const ThreatManager& mgr = unit->GetThreatMgr();
  if (mgr.CanHaveThreatList() && !mgr.IsThreatListEmpty()) {
    for (const ThreatReference* ref : mgr.GetUnsortedThreatList()) {
      if (ref && ref->GetThreat() > 0.0f) {
        classify_target(ref->GetVictim());
      }
    }
  }
}

bool IsUnitInCombatWithGroup(Player* bot, Group* group, Unit* unit) {
  if (!bot || !group || !unit || !unit->IsInCombat()) {
    return false;
  }
  std::vector<Player*> same_map_members;
  for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
       itr = itr->next()) {
    Player* member = itr->GetSource();
    if (member && member->IsInWorld() &&
        member->GetMapId() == bot->GetMapId() &&
        member->GetInstanceId() == bot->GetInstanceId()) {
      same_map_members.push_back(member);
    }
  }
  MobPullInput in;
  in.in_group = true;
  EvaluateMobCombatWithGroup(bot, group, same_map_members, unit, &in);
  return IsMobPulledForGroup(in);
}

bool IsCreatureActiveQuestMobForPlayer(const Creature* creature,
                                       Player* player) {
  if (!creature || !player) {
    return false;
  }
  const CreatureTemplate* tmpl = creature->GetCreatureTemplate();
  if (!tmpl) {
    return false;
  }
  const uint32_t entry = creature->GetEntry();
  const uint32_t credit0 = tmpl->KillCredit[0];
  const uint32_t credit1 = tmpl->KillCredit[1];
  const QuestStatusMap& status_map = player->getQuestStatusMap();
  for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot) {
    const uint32_t quest_id = player->GetQuestSlotQuestId(slot);
    if (!quest_id) {
      continue;
    }
    const auto it = status_map.find(quest_id);
    if (it == status_map.end() ||
        it->second.Status != QUEST_STATUS_INCOMPLETE) {
      continue;
    }
    const Quest* q = sObjectMgr->GetQuestTemplate(quest_id);
    if (!q) {
      continue;
    }
    for (size_t i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
      const int32_t req = q->RequiredNpcOrGo[i];
      const uint32_t need = q->RequiredNpcOrGoCount[i];
      if (req <= 0 || need == 0 || it->second.CreatureOrGOCount[i] >= need) {
        continue;
      }
      const uint32_t req_id = static_cast<uint32_t>(req);
      if (req_id == entry || (credit0 != 0 && req_id == credit0) ||
          (credit1 != 0 && req_id == credit1)) {
        return true;
      }
    }
  }
  if (tmpl->lootid != 0 &&
      LootTemplates_Creature.HaveQuestLootForPlayer(tmpl->lootid, player)) {
    return true;
  }
  return false;
}

}  // namespace llm_brain
