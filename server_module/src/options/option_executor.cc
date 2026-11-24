// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/option_executor.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "ByteBuffer.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "Event.h"
#include "G3D/Vector3.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "IVMapMgr.h"
#include "Item.h"
#include "ItemPackets.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "LastMovementValue.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "MoveSplineInitArgs.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "bot_manager/bot_manager.h"
#include "options/act_executor.h"
#include "options/option_facts.h"
#include "options/options.h"
#include "position_facts.h"
#include "snapshot/snapshot.h"
#include "snapshot/world_util.h"

namespace llm_brain {

namespace pb = wlb::snapshot;

namespace {

constexpr uint32_t kSpellCategoryFood = 11;
constexpr uint32_t kSpellCategoryDrink = 59;
constexpr uint32_t kSpellGraccusFruitcake = 25990;
// How long a Rest without food holds the body's AI, renewed every second.
constexpr uint32_t kSitHoldMs = 1500;

constexpr float kWalkStepYd = 70.0f;
constexpr float kWalkStepAdvanceYd = 10.0f;
constexpr float kWalkNearEnoughArrivalYd = 12.0f;
constexpr base::TimeDelta kWalkRepathEvery = base::Seconds(1);

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

bool IsLootableObject(GameObject* go) {
  return go->GetGoType() == GAMEOBJECT_TYPE_CHEST ||
         go->GetGOInfo()->GetLootId() != 0;
}

bool IsInUseRange(Player* bot, GameObject* go) {
  if (!IsLootableObject(go)) {
    return go->IsAtInteractDistance(bot);
  }
  // Playerbots' "use" refuses a lootable object past its LootDistance
  // (UseItemAction::UseGameObject); 1 yd of margin for the walk's last step.
  const float limit = sPlayerbotAIConfig.LootDistance;
  return limit <= 0 || bot->GetDistance(go) <= std::max(limit - 1.0f, 1.0f);
}

bool IsInTalkRange(Player* bot, Creature* npc) {
  return npc->IsWithinDistInMap(bot, INTERACTION_DISTANCE);
}

bool IsInLootRange(Player* bot, Creature* creature) {
  return creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE - 0.5f);
}

namespace {

// How far a path may end from where the bot was sent and still count as
// getting there (a path ends on the navmesh, a little off an object's spot).
constexpr float kPathEndSlackYd = 3.0f;

// Whether `go`'s lock needs a gathering skill (herbalism, mining, skinning,
// engineering), as playerbots' 80% bag rule reads it.
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

// Why playerbots' "use" refuses `go`, checked in the order its
// UseItemAction::UseGameObject checks (it only tells a master, so we ask the
// same questions). Empty when none of them applies.
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
      // Playerbots won't gather (herb, ore, skin, engineering) with bags more
      // than 80% full: that reason only for a gathering lock, and only once
      // the object was refused (a chest or quest object never: review of
      // f40b0b6, finding 12).
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

// Walks to (x, y, z) and tells playerbots it is the bot's current movement,
// so its own idle movement (wander, travel) doesn't replace the walk before it
// arrives; combat movement still does. Only along a path, as a player walks:
// false (and no move) when none gets there; never a straight line through
// walls, which MovePoint's forced destination drew (review of f40b0b6,
// finding 11).
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

}  // namespace

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

namespace {

// Walks to the bot's side of `target`, within contact distance; false when
// no path gets there.
bool WalkNextTo(Player* bot, WorldObject* target) {
  float x = target->GetPositionX();
  float y = target->GetPositionY();
  float z = target->GetPositionZ();
  // The angle is relative to the target's facing.
  target->GetClosePoint(x, y, z, bot->GetObjectSize(), CONTACT_DISTANCE,
                        target->GetAngle(bot) - target->GetOrientation(), bot);
  return WalkTo(bot, x, y, z);
}

void ClearLastMovement(Player* bot) {
  if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
    if (auto* last = ai->GetAiObjectContext()->GetValue<LastMovement&>(
            "last movement")) {
      last->Get().clear();
    }
  }
}

uint32_t SellGreyItemsTo(Player* bot, ObjectGuid vendor_guid) {
  uint32_t sold = 0;
  for (const BagSlotItem& entry : BagItems(bot)) {
    const ItemTemplate* proto = entry.item->GetTemplate();
    if (!proto || proto->Quality != ITEM_QUALITY_POOR ||
        proto->SellPrice == 0) {
      continue;
    }
    WorldPacket p(CMSG_SELL_ITEM);
    p << vendor_guid << entry.item->GetGUID() << entry.item->GetCount();
    WorldPackets::Item::SellItem packet(std::move(p));
    packet.Read();
    bot->GetSession()->HandleSellItemOpcode(packet);
    ++sold;
  }
  return sold;
}

// Picks the point ~70 yd along `points` from the start (or the end of `points`
// if the path is shorter).
G3D::Vector3 PointAlongPath(const Movement::PointsArray& points,
                            float step_yd) {
  if (points.empty()) {
    return G3D::Vector3();
  }
  float acc = 0.0f;
  for (size_t i = 1; i < points.size(); ++i) {
    const G3D::Vector3 seg = points[i] - points[i - 1];
    const float len = seg.length();
    if (acc + len >= step_yd && len > 0.0f) {
      const float t = (step_yd - acc) / len;
      return points[i - 1] + seg * t;
    }
    acc += len;
  }
  return points.back();
}

}  // namespace

std::string UseObjectNow(Player* bot, GameObject* go) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return "not a bot";
  }
  if (std::string why = WhyCantUse(bot, go); !why.empty()) {
    return why;
  }
  // If WalkNextTo brought the bot into LootDistance, clear its movement timer
  // so UseGameObject's "move to loot" step is not refused by
  // IsWaitingForLastMove.
  ClearLastMovement(bot);
  // Playerbots' "use" action takes the object as a chat link (the form
  // ChatHelper::FormatGameobject writes) and does what a click does: a chest,
  // herb or vein goes through its loot strategy (walk there, open, loot), any
  // other object gets CMSG_GAMEOBJ_USE when in reach.
  const std::string link =
      "|cffffff00|Hfound:" + std::to_string(go->GetGUID().GetRawValue()) + ":" +
      std::to_string(go->GetEntry()) + ":|h[" + go->GetName() + "]|h|r";
  if (!ai->DoSpecificAction("use", Event("llm-brain", link), /*silent=*/true)) {
    return "can't use " + go->GetName() + " now";
  }
  ai->ResetActionDuration();
  return "";
}

TalkResult TalkToNpcNow(Player* bot, Creature* npc) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {"not a bot", ""};
  }
  if (!npc || !npc->IsInWorld() || !npc->IsAlive()) {
    return {"that NPC is gone", ""};
  }
  if (!IsInTalkRange(bot, npc)) {
    return {base::StringPrintf("%s is %.0f yd away", npc->GetName().c_str(),
                               bot->GetDistance(npc)),
            ""};
  }
  ClearLastMovement(bot);
  if (bot->isMoving()) {
    bot->StopMoving();
  }
  bot->SetFacingToObject(npc);
  bot->SetSelection(npc->GetGUID());
  ai->ResetActionDuration();
  // Pure harness: approaching an NPC never makes quest, reward, trainer, or
  // vendor choices in C++; once in talk range, `CollectNpcActions` exposes the
  // concrete per-quest, per-reward, train, vendor, and flight options so the
  // brain decides.
  return {"", "approached " + npc->GetName()};
}

namespace {

// Whether the bot has an eating (kSpellCategoryFood) or drinking
// (kSpellCategoryDrink) aura.
bool HasCategoryAura(Player* bot, uint32_t category) {
  for (const auto& [spell_id, app] : bot->GetAppliedAuras()) {
    (void)spell_id;
    if (!app || !app->GetBase()) {
      continue;
    }
    if (const SpellInfo* info = app->GetBase()->GetSpellInfo();
        info && info->GetCategory() == category) {
      return true;
    }
  }
  return false;
}

// Picks the best valid equipment slot for `bag_item` (shared by both option
// discovery and execution). For dual-slot items (rings, trinkets, 1H weapons
// when dual-wielding, and 2H weapons with Titan's Grip), prefers the empty or
// lower-ilvl slot and falls back to the alternate slot if `CanEquipItem`
// rejects the first (e.g., 2H weapon equipped in main hand, offhand staff/
// polearm restriction, or Unique-Equipped replacement).
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

}  // namespace

void CheckBagFoodAndDrink(Player* bot, bool* has_food, bool* has_drink) {
  *has_food = false;
  *has_drink = false;
  for (const BagSlotItem& entry : BagItems(bot)) {
    const ItemTemplate* proto = entry.item->GetTemplate();
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE ||
        (proto->SubClass != ITEM_SUBCLASS_FOOD &&
         proto->SubClass != ITEM_SUBCLASS_CONSUMABLE) ||
        bot->CanUseItem(proto) != EQUIP_ERR_OK) {
      continue;
    }
    const uint32_t cat = proto->Spells[0].SpellCategory;
    if (cat == kSpellCategoryFood) {
      *has_food = true;
    } else if (cat == kSpellCategoryDrink) {
      *has_drink = true;
    }
  }
}

const char* TalentTabName(uint32_t tab_id) {
  switch (tab_id) {
    case TALENT_TREE_WARRIOR_ARMS:
      return "Arms";
    case TALENT_TREE_WARRIOR_FURY:
      return "Fury";
    case TALENT_TREE_WARRIOR_PROTECTION:
      return "Protection";
    case TALENT_TREE_PALADIN_HOLY:
      return "Holy";
    case TALENT_TREE_PALADIN_PROTECTION:
      return "Protection";
    case TALENT_TREE_PALADIN_RETRIBUTION:
      return "Retribution";
    case TALENT_TREE_HUNTER_BEAST_MASTERY:
      return "Beast Mastery";
    case TALENT_TREE_HUNTER_MARKSMANSHIP:
      return "Marksmanship";
    case TALENT_TREE_HUNTER_SURVIVAL:
      return "Survival";
    case TALENT_TREE_ROGUE_ASSASSINATION:
      return "Assassination";
    case TALENT_TREE_ROGUE_COMBAT:
      return "Combat";
    case TALENT_TREE_ROGUE_SUBTLETY:
      return "Subtlety";
    case TALENT_TREE_PRIEST_DISCIPLINE:
      return "Discipline";
    case TALENT_TREE_PRIEST_HOLY:
      return "Holy";
    case TALENT_TREE_PRIEST_SHADOW:
      return "Shadow";
    case TALENT_TREE_DEATH_KNIGHT_BLOOD:
      return "Blood";
    case TALENT_TREE_DEATH_KNIGHT_FROST:
      return "Frost";
    case TALENT_TREE_DEATH_KNIGHT_UNHOLY:
      return "Unholy";
    case TALENT_TREE_SHAMAN_ELEMENTAL:
      return "Elemental";
    case TALENT_TREE_SHAMAN_ENHANCEMENT:
      return "Enhancement";
    case TALENT_TREE_SHAMAN_RESTORATION:
      return "Restoration";
    case TALENT_TREE_MAGE_ARCANE:
      return "Arcane";
    case TALENT_TREE_MAGE_FIRE:
      return "Fire";
    case TALENT_TREE_MAGE_FROST:
      return "Frost";
    case TALENT_TREE_WARLOCK_AFFLICTION:
      return "Affliction";
    case TALENT_TREE_WARLOCK_DEMONOLOGY:
      return "Demonology";
    case TALENT_TREE_WARLOCK_DESTRUCTION:
      return "Destruction";
    case TALENT_TREE_DRUID_BALANCE:
      return "Balance";
    case TALENT_TREE_DRUID_FERAL_COMBAT:
      return "Feral Combat";
    case TALENT_TREE_DRUID_RESTORATION:
      return "Restoration";
    default:
      return nullptr;
  }
}

void CollectTalentTrees(Player* bot,
                        std::vector<pb::OptionFacts_TalentTree>* out) {
  out->clear();
  if (!bot || !bot->IsAlive() || bot->IsInCombat()) {
    return;
  }
  const uint32_t free_points = bot->GetFreeTalentPoints();
  if (free_points == 0) {
    return;
  }
  const uint32_t class_mask = bot->getClassMask();
  struct TabInfo {
    uint32_t tab_id = 0;
    uint32_t tab_page = 0;
    std::string name;
    uint32_t spent = 0;
  };
  std::vector<TabInfo> tabs;
  for (uint32_t i = 0; i < sTalentTabStore.GetNumRows(); ++i) {
    const TalentTabEntry* entry = sTalentTabStore.LookupEntry(i);
    if (!entry || (entry->ClassMask & class_mask) == 0) {
      continue;
    }
    const char* name = TalentTabName(entry->TalentTabID);
    if (!name || !*name) {
      continue;
    }
    tabs.push_back({entry->TalentTabID, entry->tabpage, name, 0});
  }
  std::sort(tabs.begin(), tabs.end(), [](const TabInfo& a, const TabInfo& b) {
    return a.tab_page < b.tab_page;
  });
  const uint8_t active_spec = bot->GetActiveSpec();
  const PlayerTalentMap& talent_map = bot->GetTalentMap();
  for (uint32_t i = 0; i < sTalentStore.GetNumRows(); ++i) {
    const TalentEntry* talent = sTalentStore.LookupEntry(i);
    if (!talent) {
      continue;
    }
    TabInfo* owner = nullptr;
    for (TabInfo& t : tabs) {
      if (t.tab_id == talent->TalentTab) {
        owner = &t;
        break;
      }
    }
    if (!owner) {
      continue;
    }
    for (int rank = MAX_TALENT_RANK - 1; rank >= 0; --rank) {
      const uint32_t spell_id = talent->RankID[rank];
      if (!spell_id) {
        continue;
      }
      auto it = talent_map.find(spell_id);
      if (it != talent_map.end() && it->second &&
          it->second->State != PLAYERSPELL_REMOVED &&
          it->second->IsInSpec(active_spec)) {
        owner->spent += static_cast<uint32_t>(rank + 1);
        break;
      }
    }
  }
  for (const TabInfo& t : tabs) {
    out->push_back({t.tab_page, t.name, t.spent, free_points});
  }
}

namespace {

bool HasGreySellableItem(Player* bot) {
  for (const BagSlotItem& entry : BagItems(bot)) {
    const ItemTemplate* proto =
        entry.item ? entry.item->GetTemplate() : nullptr;
    if (proto && proto->Quality == ITEM_QUALITY_POOR && proto->SellPrice > 0) {
      return true;
    }
  }
  return false;
}

bool HasDamagedEquipment(Player* bot) {
  for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot) {
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item) {
      continue;
    }
    const uint32_t max_dur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    if (max_dur > 0 && item->GetUInt32Value(ITEM_FIELD_DURABILITY) < max_dur) {
      return true;
    }
  }
  return false;
}

uint32_t CountItemInBags(Player* bot, uint32_t item_entry) {
  return bot->GetItemCount(item_entry, false);
}

bool IsAmmoForHunter(Player* bot, const ItemTemplate* proto) {
  if (!bot || bot->getClass() != CLASS_HUNTER || !proto ||
      proto->Class != ITEM_CLASS_PROJECTILE) {
    return false;
  }
  Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
  if (!ranged || !ranged->GetTemplate()) {
    return false;
  }
  const uint32_t rsub = ranged->GetTemplate()->SubClass;
  if (rsub == ITEM_SUBCLASS_WEAPON_BOW ||
      rsub == ITEM_SUBCLASS_WEAPON_CROSSBOW) {
    return proto->SubClass == ITEM_SUBCLASS_ARROW;
  }
  if (rsub == ITEM_SUBCLASS_WEAPON_GUN) {
    return proto->SubClass == ITEM_SUBCLASS_BULLET;
  }
  return false;
}

struct LfgRoleChoice {
  uint8_t mask = lfg::PLAYER_ROLE_DAMAGE;
  const char* name = "DPS";
};

// Mechanically valid WotLK 3.3.5a LFG roles for `cls` (the brain chooses which
// role to queue or confirm, rather than C++ guessing by class).
std::vector<LfgRoleChoice> ClassLfgRoles(uint8_t cls) {
  switch (cls) {
    case CLASS_WARRIOR:
    case CLASS_DEATH_KNIGHT:
      return {{lfg::PLAYER_ROLE_TANK, "Tank"},
              {lfg::PLAYER_ROLE_DAMAGE, "DPS"}};
    case CLASS_PRIEST:
    case CLASS_SHAMAN:
      return {{lfg::PLAYER_ROLE_HEALER, "Healer"},
              {lfg::PLAYER_ROLE_DAMAGE, "DPS"}};
    case CLASS_PALADIN:
    case CLASS_DRUID:
      return {{lfg::PLAYER_ROLE_TANK, "Tank"},
              {lfg::PLAYER_ROLE_HEALER, "Healer"},
              {lfg::PLAYER_ROLE_DAMAGE, "DPS"}};
    default:
      return {{lfg::PLAYER_ROLE_DAMAGE, "DPS"}};
  }
}

uint32_t FindRandomLfgDungeonId(Player* bot) {
  if (!bot) {
    return 0;
  }
  const uint8_t lvl = bot->GetLevel();
  uint32_t best_id = 0;
  uint32_t best_max_lvl = 0;
  for (uint32_t i = 0; i < sLFGDungeonStore.GetNumRows(); ++i) {
    const LFGDungeonEntry* d = sLFGDungeonStore.LookupEntry(i);
    if (!d || d->TypeID != lfg::LFG_TYPE_RANDOM) {
      continue;
    }
    if (lvl < d->MinLevel || lvl > d->MaxLevel) {
      continue;
    }
    if (d->MaxLevel >= best_max_lvl) {
      best_max_lvl = d->MaxLevel;
      best_id = d->ID;
    }
  }
  return best_id;
}

bool ParseUint32(std::string_view s, uint32_t* out) {
  if (s.empty()) {
    return false;
  }
  uint32_t val = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
  if (ec != std::errc{} || ptr != s.data() + s.size()) {
    return false;
  }
  *out = val;
  return true;
}

}  // namespace

void CollectNpcActions(Player* bot,
                       const pb::Snapshot& snapshot,
                       bool has_food,
                       bool has_drink,
                       std::vector<pb::OptionFacts_NpcAction>* out) {
  out->clear();
  if (!bot || !bot->IsAlive() || bot->IsInCombat()) {
    return;
  }
  static constexpr size_t kMaxNpcActions = 20;
  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if (out->size() >= kMaxNpcActions) {
      break;
    }
    if (u.kind != pb::Unit::NPC || u.reaction != pb::Unit::FRIENDLY || u.dead ||
        u.guid.empty()) {
      continue;
    }
    Creature* npc =
        ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(u.guid));
    if (!npc || !npc->IsInWorld() || !npc->IsAlive() ||
        !IsInTalkRange(bot, npc)) {
      continue;
    }
    const std::string& npc_name = npc->GetName();

    // 1. Quest turn-ins (with explicit per-reward options when > 1 choice)
    //    and quest accepts (one option per available quest).
    if (npc->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) &&
        bot->CanInteractWithQuestGiver(npc)) {
      bot->PrepareQuestMenu(npc->GetGUID());
      const QuestMenu& menu = bot->PlayerTalkClass->GetQuestMenu();
      for (uint8_t idx = 0;
           idx < menu.GetMenuItemCount() && out->size() < kMaxNpcActions;
           ++idx) {
        const QuestMenuItem& item = menu.GetItem(idx);
        const Quest* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest) {
          continue;
        }
        const QuestStatus status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_COMPLETE &&
            !bot->GetQuestRewardStatus(item.QuestId) &&
            bot->CanRewardQuest(quest, false)) {
          const uint32_t choices = quest->GetRewChoiceItemsCount();
          if (choices > 1) {
            for (uint32_t r = 0; r < choices && out->size() < kMaxNpcActions;
                 ++r) {
              const ItemTemplate* rproto =
                  sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[r]);
              const std::string rdesc =
                  rproto ? (rproto->Name1 + ", ilvl " +
                            std::to_string(rproto->ItemLevel))
                         : ("reward #" + std::to_string(r + 1));
              out->push_back({"npc_qturnin:" + u.guid + ":" +
                                  std::to_string(quest->GetQuestId()) + ":" +
                                  std::to_string(r),
                              u.guid,
                              "Turn in \"" + quest->GetTitle() + "\" to " +
                                  npc_name + " (pick " + rdesc + ")"});
            }
          } else {
            out->push_back(
                {"npc_qturnin:" + u.guid + ":" +
                     std::to_string(quest->GetQuestId()) + ":0",
                 u.guid,
                 "Turn in \"" + quest->GetTitle() + "\" to " + npc_name});
          }
        } else if (status == QUEST_STATUS_NONE &&
                   bot->CanTakeQuest(quest, false) &&
                   bot->CanAddQuest(quest, false)) {
          out->push_back(
              {"npc_qaccept:" + u.guid + ":" +
                   std::to_string(quest->GetQuestId()),
               u.guid,
               "Accept quest \"" + quest->GetTitle() + "\" from " + npc_name});
        }
      }
    }

    // 2. Class trainer: count affordable learnable class spells.
    if (out->size() < kMaxNpcActions &&
        npc->HasNpcFlag(UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS)) {
      if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
          trainer && trainer->GetTrainerType() == Trainer::Type::Class &&
          trainer->IsTrainerValidForPlayer(bot)) {
        const float discount = bot->GetReputationPriceDiscount(npc);
        uint32_t affordable = 0;
        for (const Trainer::Spell& sp : trainer->GetSpells()) {
          if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer,
                                                             &sp) ||
              !trainer->CanTeachSpell(bot, &sp)) {
            continue;
          }
          const int32_t cost = static_cast<int32_t>(sp.MoneyCost * discount);
          if (bot->HasEnoughMoney(cost)) {
            ++affordable;
          }
        }
        if (affordable > 0) {
          out->push_back({"npc_train:" + u.guid, u.guid,
                          "Train " + std::to_string(affordable) +
                              " class spell" + (affordable == 1 ? "" : "s") +
                              " at " + npc_name});
        }
      }
    }

    // 3. Vendor sell grey items & repair.
    if (out->size() < kMaxNpcActions &&
        (npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) ||
         npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))) {
      const bool can_sell_grey =
          npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && HasGreySellableItem(bot);
      const bool can_repair =
          npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && HasDamagedEquipment(bot);
      if (can_sell_grey || can_repair) {
        std::string label = can_sell_grey && can_repair
                                ? "Sell grey items & repair at "
                            : can_sell_grey ? "Sell grey items at "
                                            : "Repair gear at ";
        out->push_back({"npc_vendor:" + u.guid, u.guid, label + npc_name});
      }
    }

    // 4. Vendor upkeep buy: food, water, ammo, or class reagents when low/out.
    if (out->size() < kMaxNpcActions && npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR)) {
      if (const VendorItemData* vitems = npc->GetVendorItems()) {
        const float discount = bot->GetReputationPriceDiscount(npc);
        const bool need_drink = !has_drink && bot->getPowerType() == POWER_MANA;
        const bool need_ammo =
            bot->getClass() == CLASS_HUNTER &&
            (bot->GetUInt32Value(PLAYER_AMMO_ID) == 0 ||
             CountItemInBags(bot, bot->GetUInt32Value(PLAYER_AMMO_ID)) < 200);
        const ItemTemplate* best_food = nullptr;
        const ItemTemplate* best_drink = nullptr;
        const ItemTemplate* best_ammo = nullptr;
        for (uint32_t slot = 0; slot < vitems->GetItemCount(); ++slot) {
          const VendorItem* vi = vitems->GetItem(slot);
          if (!vi || vi->ExtendedCost != 0) {
            continue;
          }
          const ItemTemplate* proto = sObjectMgr->GetItemTemplate(vi->item);
          if (!proto || proto->Name1.empty() ||
              proto->RequiredLevel > bot->GetLevel()) {
            continue;
          }
          const uint32_t price =
              static_cast<uint32_t>(proto->BuyPrice * discount);
          if (!bot->HasEnoughMoney(price)) {
            continue;
          }
          if (!has_food && proto->Class == ITEM_CLASS_CONSUMABLE &&
              proto->SubClass == ITEM_SUBCLASS_FOOD &&
              (!best_food || proto->ItemLevel > best_food->ItemLevel)) {
            // Distinguish food from drink via first spell category.
            const SpellInfo* sp =
                proto->Spells[0].SpellId
                    ? sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId)
                    : nullptr;
            if (sp && sp->GetCategory() == 11) {
              best_food = proto;
            }
          }
          if (need_drink && proto->Class == ITEM_CLASS_CONSUMABLE &&
              proto->SubClass == ITEM_SUBCLASS_FOOD &&
              (!best_drink || proto->ItemLevel > best_drink->ItemLevel)) {
            const SpellInfo* sp =
                proto->Spells[0].SpellId
                    ? sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId)
                    : nullptr;
            if (sp && sp->GetCategory() == 59) {
              best_drink = proto;
            }
          }
          if (need_ammo && IsAmmoForHunter(bot, proto) &&
              (!best_ammo || proto->ItemLevel > best_ammo->ItemLevel)) {
            best_ammo = proto;
          }
        }
        for (const ItemTemplate* pick : {best_food, best_drink, best_ammo}) {
          if (pick && out->size() < kMaxNpcActions) {
            out->push_back(
                {"npc_buy:" + u.guid + ":" + std::to_string(pick->ItemId),
                 u.guid, "Buy " + pick->Name1 + " from " + npc_name});
          }
        }
      }
    }

    // 5. Flight master travel: discover unknown node or fly to known
    //    connected destinations.
    if (out->size() < kMaxNpcActions &&
        npc->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER)) {
      const uint32_t cur_node =
          sObjectMgr->GetNearestTaxiNode(*npc, bot->GetTeamId(true));
      if (cur_node != 0 && !bot->m_taxi.IsTaximaskNodeKnown(cur_node)) {
        out->push_back({"npc_fly_discover:" + u.guid, u.guid,
                        "Discover flight path at " + npc_name});
      } else if (cur_node != 0) {
        size_t added_flights = 0;
        const uint32_t locale = sWorld->GetDefaultDbcLocale();
        for (uint32_t i = 0; i < sTaxiPathStore.GetNumRows() &&
                             added_flights < 4 && out->size() < kMaxNpcActions;
             ++i) {
          const TaxiPathEntry* path = sTaxiPathStore.LookupEntry(i);
          if (!path || path->from != cur_node ||
              !bot->m_taxi.IsTaximaskNodeKnown(path->to) ||
              !bot->HasEnoughMoney(path->price)) {
            continue;
          }
          const TaxiNodesEntry* dest = sTaxiNodesStore.LookupEntry(path->to);
          if (!dest || !dest->name[locale] || !*dest->name[locale]) {
            continue;
          }
          out->push_back({"npc_fly:" + u.guid + ":" + std::to_string(path->to),
                          u.guid,
                          std::string("Fly to ") + dest->name[locale] +
                              " via " + npc_name});
          ++added_flights;
        }
      }
    }
  }
}

void CollectGroupAndUpkeepActions(Player* bot,
                                  const pb::Snapshot& snapshot,
                                  std::vector<pb::OptionFacts_Action>* out) {
  out->clear();
  if (!bot || !bot->IsInWorld()) {
    return;
  }

  // 1. Need / Greed / Pass loot rolls (allowed even in combat or dead).
  if (Group* group = bot->GetGroup()) {
    size_t rolls_added = 0;
    for (const Roll* roll : group->GetRolls()) {
      if (!roll || rolls_added >= 2) {
        break;
      }
      auto it = roll->playerVote.find(bot->GetGUID());
      if (it == roll->playerVote.end() || it->second != NOT_EMITED_YET) {
        continue;
      }
      const ItemTemplate* proto = sObjectMgr->GetItemTemplate(roll->itemid);
      const std::string item_name =
          proto ? proto->Name1 : ("item #" + std::to_string(roll->itemid));
      const std::string detail =
          proto ? (" (ilvl " + std::to_string(proto->ItemLevel) + ")") : "";
      const std::string prefix = base::StringPrintf(
          "roll:0x%016llx:%u:",
          static_cast<unsigned long long>(roll->itemGUID.GetRawValue()),
          static_cast<unsigned>(roll->itemSlot));
      if (roll->rollVoteMask & ROLL_FLAG_TYPE_NEED) {
        out->push_back({prefix + "1", "Need " + item_name + detail});
      }
      if (roll->rollVoteMask & ROLL_FLAG_TYPE_GREED) {
        out->push_back({prefix + "2", "Greed " + item_name + detail});
      }
      out->push_back({prefix + "0", "Pass " + item_name});
      ++rolls_added;
    }
  }

  // 2. Ready check (allowed in combat or dead).
  if (BotManager::HasPendingReadyCheck(bot)) {
    out->push_back({"ready:yes", "Ready (confirm ready check)"});
    out->push_back({"ready:no", "Not ready (decline ready check)"});
  }

  // 3. Shared quest accept / decline (alive).
  if (bot->IsAlive()) {
    if (const uint32_t qid = BotManager::GetPendingSharedQuest(bot); qid != 0) {
      if (const Quest* q = sObjectMgr->GetQuestTemplate(qid)) {
        out->push_back({"qshare:accept:" + std::to_string(qid),
                        "Accept shared quest \"" + q->GetTitle() + "\""});
        out->push_back({"qshare:decline:" + std::to_string(qid),
                        "Decline shared quest \"" + q->GetTitle() + "\""});
      }
    }
  }

  // 4. Dungeon Finder (LFG / RDF).
  if (bot->IsAlive()) {
    const lfg::LfgState st = sLFGMgr->GetState(bot->GetGUID());
    if (st == lfg::LFG_STATE_ROLECHECK) {
      for (const LfgRoleChoice& rc : ClassLfgRoles(bot->getClass())) {
        out->push_back(
            {"lfg:role:" + std::to_string(rc.mask),
             std::string("Confirm Dungeon Finder role (") + rc.name + ")"});
      }
    } else if (st == lfg::LFG_STATE_PROPOSAL) {
      if (const uint32_t pid = BotManager::GetPendingLfgProposal(bot);
          pid != 0) {
        out->push_back({"lfg:accept:" + std::to_string(pid),
                        "Accept Dungeon Finder proposal"});
        out->push_back({"lfg:decline:" + std::to_string(pid),
                        "Decline Dungeon Finder proposal"});
      }
    } else if (st == lfg::LFG_STATE_QUEUED) {
      out->push_back({"lfg:leave", "Leave Dungeon Finder queue"});
    } else if (st == lfg::LFG_STATE_DUNGEON && !bot->IsInCombat()) {
      if (bot->GetMap() && !bot->GetMap()->IsDungeon()) {
        out->push_back({"lfg:tele_in", "Teleport to LFG dungeon"});
      } else {
        out->push_back({"lfg:tele_out", "Teleport out of LFG dungeon"});
      }
    } else if (st == lfg::LFG_STATE_NONE && !bot->IsInCombat() &&
               bot->GetLevel() >= 15 && bot->GetMap() &&
               !bot->GetMap()->IsDungeon() &&
               (!bot->GetGroup() ||
                bot->GetGroup()->IsLeader(bot->GetGUID())) &&
               FindRandomLfgDungeonId(bot) != 0) {
      for (const LfgRoleChoice& rc : ClassLfgRoles(bot->getClass())) {
        out->push_back(
            {"lfg:queue:" + std::to_string(rc.mask),
             std::string("Queue Random Dungeon (LFG: ") + rc.name + ")"});
      }
    }
  }

  // 5. Close-range Follow / Regroup with party leader (8..25 yd out of combat;
  //    >= 25 yd already has `Go to <leader> (party leader)`).
  if (bot->IsAlive() && !bot->IsInCombat()) {
    if (Group* group = bot->GetGroup()) {
      const ObjectGuid leader_guid = group->GetLeaderGUID();
      if (leader_guid && leader_guid != bot->GetGUID()) {
        if (Player* leader = ObjectAccessor::FindConnectedPlayer(leader_guid);
            leader && leader->IsInWorld() && leader->IsAlive() &&
            leader->GetMapId() == bot->GetMapId() &&
            leader->GetInstanceId() == bot->GetInstanceId()) {
          const float dist = bot->GetDistance(leader);
          if (dist >= 8.0f && dist < 25.0f) {
            out->push_back({base::StringPrintf("follow:0x%016llx",
                                               static_cast<unsigned long long>(
                                                   leader_guid.GetRawValue())),
                            "Follow " + leader->GetName() + " (party leader)"});
          }
        }
      }
    }
  }

  // 6. Leader raid target mark: mark nearest living hostile mob with Skull.
  if (bot->IsAlive()) {
    if (Group* group = bot->GetGroup(); group &&
                                        (group->IsLeader(bot->GetGUID()) ||
                                         group->IsAssistant(bot->GetGUID())) &&
                                        group->GetTargetIcon(7).IsEmpty()) {
      const pb::Unit* best = nullptr;
      for (const pb::Unit& u : snapshot.base.nearby.units) {
        if (u.dead || u.kind != pb::Unit::NPC ||
            u.reaction != pb::Unit::HOSTILE || u.guid.empty() ||
            u.rel.dist_yd > 40.0f) {
          continue;
        }
        if (!best || u.rel.dist_yd < best->rel.dist_yd) {
          best = &u;
        }
      }
      if (best) {
        out->push_back({"mark:" + best->guid + ":7",
                        "Mark " + best->name + " with Skull"});
      }
    }
  }

  // 7. Mount (outdoors, out of combat, not mounted, not in dungeon).
  if (bot->IsAlive() && !bot->IsInCombat() && !bot->IsMounted() &&
      bot->GetMap() && !bot->GetMap()->IsDungeon() && bot->IsOutdoors()) {
    const uint32_t locale = sWorld->GetDefaultDbcLocale();
    for (const auto& [spell_id, state] : bot->GetSpellMap()) {
      if (!state || !state->Active || state->State == PLAYERSPELL_REMOVED) {
        continue;
      }
      const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
      if (!info || !info->HasAura(SPELL_AURA_MOUNTED) ||
          info->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)) {
        continue;
      }
      const char* name = info->SpellName[locale];
      if (!name || !*name) {
        continue;
      }
      out->push_back(
          {"mount:" + std::to_string(spell_id), std::string("Mount ") + name});
      break;
    }
  }
}

void CollectUsableBagItems(Player* bot,
                           std::vector<pb::OptionFacts_Item>* out) {
  out->clear();
  if (!bot || bot->m_mover != bot) {
    return;
  }
  static constexpr size_t kMaxUsableBagItems = 32;
  for (const BagSlotItem& entry : BagItems(bot)) {
    if (out->size() >= kMaxUsableBagItems) {
      break;
    }
    const ItemTemplate* proto =
        entry.item ? entry.item->GetTemplate() : nullptr;
    if (!proto || proto->Name1.empty() ||
        proto->InventoryType != INVTYPE_NON_EQUIP ||
        bot->CanUseItem(entry.item) != EQUIP_ERR_OK) {
      continue;
    }
    uint32_t spell_id = 0;
    uint32_t spell_cat = 0;
    bool has_charges = true;
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
      if (proto->Spells[i].SpellId > 0 &&
          proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE) {
        if (proto->Spells[i].SpellCharges != 0 &&
            entry.item->GetSpellCharges(i) == 0) {
          has_charges = false;
          continue;
        }
        spell_id = static_cast<uint32_t>(proto->Spells[i].SpellId);
        spell_cat = proto->Spells[i].SpellCategory;
        has_charges = true;
        break;
      }
    }
    if (spell_id == 0 || !has_charges) {
      continue;
    }
    // Food and drink are already surfaced by Rest.
    if (spell_cat == kSpellCategoryFood || spell_cat == kSpellCategoryDrink) {
      continue;
    }
    if (bot->HasSpellCooldown(spell_id)) {
      continue;
    }
    const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
    if (!info) {
      continue;
    }
    // Skip item-targeted enchants (sharpening stones, weightstones, weapon
    // oils): ExecuteUseItemOption only targets units/self, never an item slot.
    bool requires_item_target = false;
    for (uint8_t eff = 0; eff < MAX_SPELL_EFFECTS; ++eff) {
      const uint32_t e = info->Effects[eff].Effect;
      if (e == SPELL_EFFECT_ENCHANT_ITEM ||
          e == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY ||
          e == SPELL_EFFECT_ENCHANT_HELD_ITEM) {
        requires_item_target = true;
        break;
      }
    }
    if (requires_item_target) {
      continue;
    }
    // Match Spell.cpp:5910: in 3.3.5a, only one potion can be used per combat
    // (cooldown starts only after leaving combat, so HasSpellCooldown is
    // false).
    if (bot->GetLastPotionId() &&
        (entry.item->IsPotion() || info->IsCooldownStartedOnEvent())) {
      continue;
    }
    // Match SpellHandler.cpp: check combat usability across all spells on item.
    bool out_of_combat_only = false;
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
      if (proto->Spells[i].SpellId > 0) {
        if (const SpellInfo* si = sSpellMgr->GetSpellInfo(
                static_cast<uint32_t>(proto->Spells[i].SpellId));
            si && si->HasAttribute(SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL)) {
          out_of_combat_only = true;
          break;
        }
      }
    }
    bool dup = false;
    for (const pb::OptionFacts_Item& existing : *out) {
      if (existing.entry == proto->ItemId) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    pb::OptionFacts_Item item;
    item.entry = proto->ItemId;
    item.name = proto->Name1;
    item.out_of_combat_only = out_of_combat_only;
    // Any homebind teleport item (Hearthstone, Scroll of Recall, Ethereal
    // Portal, etc.): attach factual homebind destination from the spell effect
    // itself.
    for (uint8_t i = 0; i < MAX_SPELL_EFFECTS; ++i) {
      const SpellEffectInfo& eff = info->Effects[i];
      if (eff.Effect == SPELL_EFFECT_TELEPORT_UNITS &&
          (eff.TargetA.GetTarget() == TARGET_DEST_HOME ||
           eff.TargetB.GetTarget() == TARGET_DEST_HOME)) {
        const std::string home = AreaName(bot->m_homebindAreaId);
        item.detail =
            home.empty() ? "teleports home" : "teleports home to " + home;
        break;
      }
    }
    out->push_back(std::move(item));
  }

  // Also expose up to 8 equippable non-grey gear items from bags (B2: pure
  // harness — exposes factual quality, type, ilvl vs equipped slot, and stats
  // without hardcoded upgrade heuristics; the brain decides whether to equip).
  if (!bot->IsInCombat()) {
    static constexpr size_t kMaxEquipCandidates = 8;
    const auto quality_name = [](uint32_t q) -> const char* {
      switch (q) {
        case ITEM_QUALITY_NORMAL:
          return "common";
        case ITEM_QUALITY_UNCOMMON:
          return "uncommon";
        case ITEM_QUALITY_RARE:
          return "rare";
        case ITEM_QUALITY_EPIC:
          return "epic";
        case ITEM_QUALITY_LEGENDARY:
          return "legendary";
        case ITEM_QUALITY_HEIRLOOM:
          return "heirloom";
        default:
          return "";
      }
    };
    const auto armor_subclass_name = [](uint32_t sub) -> const char* {
      switch (sub) {
        case ITEM_SUBCLASS_ARMOR_CLOTH:
          return "Cloth";
        case ITEM_SUBCLASS_ARMOR_LEATHER:
          return "Leather";
        case ITEM_SUBCLASS_ARMOR_MAIL:
          return "Mail";
        case ITEM_SUBCLASS_ARMOR_PLATE:
          return "Plate";
        case ITEM_SUBCLASS_ARMOR_SHIELD:
          return "Shield";
        default:
          return "";
      }
    };
    const auto stat_short_name = [](uint32_t st) -> const char* {
      switch (st) {
        case ITEM_MOD_AGILITY:
          return "Agi";
        case ITEM_MOD_STRENGTH:
          return "Str";
        case ITEM_MOD_INTELLECT:
          return "Int";
        case ITEM_MOD_SPIRIT:
          return "Spi";
        case ITEM_MOD_STAMINA:
          return "Sta";
        case ITEM_MOD_SPELL_POWER:
          return "SP";
        case ITEM_MOD_ATTACK_POWER:
          return "AP";
        case ITEM_MOD_CRIT_RATING:
          return "Crit";
        case ITEM_MOD_HIT_RATING:
          return "Hit";
        case ITEM_MOD_HASTE_RATING:
          return "Haste";
        default:
          return "";
      }
    };
    const auto slot_name = [](uint32_t inv) -> const char* {
      switch (inv) {
        case INVTYPE_HEAD:
          return "Head";
        case INVTYPE_NECK:
          return "Neck";
        case INVTYPE_SHOULDERS:
          return "Shoulders";
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
          return "Chest";
        case INVTYPE_WAIST:
          return "Waist";
        case INVTYPE_LEGS:
          return "Legs";
        case INVTYPE_FEET:
          return "Feet";
        case INVTYPE_WRISTS:
          return "Wrists";
        case INVTYPE_HANDS:
          return "Hands";
        case INVTYPE_FINGER:
          return "Finger";
        case INVTYPE_TRINKET:
          return "Trinket";
        case INVTYPE_CLOAK:
          return "Back";
        case INVTYPE_WEAPON:
          return "1H";
        case INVTYPE_SHIELD:
          return "Off Hand";
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:
          return "Ranged";
        case INVTYPE_2HWEAPON:
          return "2H";
        case INVTYPE_WEAPONMAINHAND:
          return "Main Hand";
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_HOLDABLE:
          return "Off Hand";
        case INVTYPE_RELIC:
          return "Relic";
        default:
          return "";
      }
    };
    const auto weapon_subclass_name = [](uint32_t sub) -> const char* {
      switch (sub) {
        case ITEM_SUBCLASS_WEAPON_AXE:
        case ITEM_SUBCLASS_WEAPON_AXE2:
          return "Axe";
        case ITEM_SUBCLASS_WEAPON_BOW:
          return "Bow";
        case ITEM_SUBCLASS_WEAPON_GUN:
          return "Gun";
        case ITEM_SUBCLASS_WEAPON_MACE:
        case ITEM_SUBCLASS_WEAPON_MACE2:
          return "Mace";
        case ITEM_SUBCLASS_WEAPON_POLEARM:
          return "Polearm";
        case ITEM_SUBCLASS_WEAPON_SWORD:
        case ITEM_SUBCLASS_WEAPON_SWORD2:
          return "Sword";
        case ITEM_SUBCLASS_WEAPON_STAFF:
          return "Staff";
        case ITEM_SUBCLASS_WEAPON_FIST:
          return "Fist";
        case ITEM_SUBCLASS_WEAPON_DAGGER:
          return "Dagger";
        case ITEM_SUBCLASS_WEAPON_THROWN:
          return "Thrown";
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
          return "Crossbow";
        case ITEM_SUBCLASS_WEAPON_WAND:
          return "Wand";
        default:
          return "Weapon";
      }
    };
    struct RankedEquip {
      int32_t ilvl_delta = 0;
      pb::OptionFacts_Item item;
    };
    std::vector<RankedEquip> candidates;
    for (const BagSlotItem& entry : BagItems(bot)) {
      Item* bag_item = entry.item;
      const ItemTemplate* proto = bag_item ? bag_item->GetTemplate() : nullptr;
      if (!proto || proto->Name1.empty() ||
          proto->Quality <= ITEM_QUALITY_POOR ||
          proto->InventoryType == INVTYPE_NON_EQUIP ||
          (proto->Class != ITEM_CLASS_ARMOR &&
           proto->Class != ITEM_CLASS_WEAPON)) {
        continue;
      }
      uint16_t dest = 0;
      if (!PickEquipSlot(bot, bag_item, proto, &dest)) {
        continue;
      }
      const uint8_t equip_slot = static_cast<uint8_t>(dest & 0xFF);
      const Item* equipped =
          bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equip_slot);
      const ItemTemplate* equipped_proto =
          equipped ? equipped->GetTemplate() : nullptr;
      const uint32_t equipped_ilvl =
          equipped_proto ? equipped_proto->ItemLevel : 0;
      bool dup = false;
      for (const RankedEquip& existing : candidates) {
        if (existing.item.entry == proto->ItemId) {
          dup = true;
          break;
        }
      }
      if (dup) {
        continue;
      }
      std::string detail;
      if (const char* sl = slot_name(proto->InventoryType); *sl) {
        detail += sl;
      }
      if (const char* qn = quality_name(proto->Quality); *qn) {
        if (!detail.empty()) {
          detail += " ";
        }
        detail += qn;
      }
      if (proto->Class == ITEM_CLASS_ARMOR) {
        if (const char* sn = armor_subclass_name(proto->SubClass); *sn) {
          if (!detail.empty()) {
            detail += " ";
          }
          detail += sn;
        }
      } else if (proto->Class == ITEM_CLASS_WEAPON) {
        if (!detail.empty()) {
          detail += " ";
        }
        detail += weapon_subclass_name(proto->SubClass);
      }
      if (!detail.empty()) {
        detail += ", ";
      }
      if (equipped_proto) {
        detail += "ilvl " + std::to_string(proto->ItemLevel) + " vs ";
        if (!equipped_proto->Name1.empty()) {
          detail += equipped_proto->Name1 + " ";
        }
        detail += "ilvl " + std::to_string(equipped_ilvl);
      } else {
        detail += "ilvl " + std::to_string(proto->ItemLevel) + ", empty slot";
      }
      if (proto->Class == ITEM_CLASS_WEAPON && proto->Delay > 0) {
        float avg_dmg = 0.0f;
        for (int i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i) {
          avg_dmg +=
              (proto->Damage[i].DamageMin + proto->Damage[i].DamageMax) * 0.5f;
        }
        const float dps =
            avg_dmg / (static_cast<float>(proto->Delay) / 1000.0f);
        if (dps >= 0.5f) {
          detail += ", " + std::to_string(static_cast<int>(std::lround(dps))) +
                    " DPS";
        }
      } else if (proto->Armor > 0) {
        detail += ", " + std::to_string(proto->Armor) + " Armor";
      }
      size_t stats_added = 0;
      for (uint32_t s = 0; s < proto->StatsCount && s < MAX_ITEM_PROTO_STATS;
           ++s) {
        if (proto->ItemStat[s].ItemStatValue <= 0) {
          continue;
        }
        const char* sname = stat_short_name(proto->ItemStat[s].ItemStatType);
        if (!*sname) {
          continue;
        }
        detail += ", +" + std::to_string(proto->ItemStat[s].ItemStatValue) +
                  " " + sname;
        if (++stats_added >= 3) {
          break;
        }
      }
      if (proto->Bonding == BIND_WHEN_EQUIPPED && !bag_item->IsSoulBound()) {
        detail += ", BoE";
      }
      pb::OptionFacts_Item item;
      item.entry = proto->ItemId;
      item.name = proto->Name1;
      item.out_of_combat_only = true;
      item.equippable = true;
      item.detail = std::move(detail);
      candidates.push_back({static_cast<int32_t>(proto->ItemLevel) -
                                static_cast<int32_t>(equipped_ilvl),
                            std::move(item)});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const RankedEquip& a, const RankedEquip& b) {
                       return a.ilvl_delta > b.ilvl_delta;
                     });
    size_t equip_added = 0;
    for (RankedEquip& cand : candidates) {
      if (equip_added >= kMaxEquipCandidates ||
          out->size() >= kMaxUsableBagItems) {
        break;
      }
      out->push_back(std::move(cand.item));
      ++equip_added;
    }
  }
}

bool HasFoodOrDrinkAura(Player* bot) {
  return bot->HasAura(kSpellGraccusFruitcake) ||
         HasCategoryAura(bot, kSpellCategoryFood) ||
         HasCategoryAura(bot, kSpellCategoryDrink);
}

namespace {

// Eats and/or drinks only what the bags actually hold: with no drink,
// playerbots' "drink" falls back to food, and the food aura doesn't stop the
// drink check, so the bot ate again every second or two (review of f40b0b6,
// finding 2). Used on both the first tick (ExecuteRestOption) and later ticks
// (ContinueRest). Returns true if a consumable action fired.
bool TryEatAndDrink(Player* bot, PlayerbotAI* ai) {
  bool has_food = false;
  bool has_drink = false;
  CheckBagFoodAndDrink(bot, &has_food, &has_drink);
  bool used = false;
  if (has_food && !bot->IsFullHealth() &&
      !HasCategoryAura(bot, kSpellCategoryFood) &&
      !bot->HasAura(kSpellGraccusFruitcake)) {
    used |= ai->DoSpecificAction("eat", Event(), /*silent=*/true);
  }
  if (has_drink && bot->GetMaxPower(POWER_MANA) > 0 &&
      bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA) &&
      !HasCategoryAura(bot, kSpellCategoryDrink)) {
    used |= ai->DoSpecificAction("drink", Event(), /*silent=*/true);
  }
  return used;
}

}  // namespace

void ContinueRest(Player* bot) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai || bot->IsNonMeleeSpellCast(false) || bot->IsInCombat()) {
    return;
  }
  TryEatAndDrink(bot, ai);
  // Sitting with nothing to eat: keep the body from standing up (it does when
  // its next check is due), a little longer than the caller's 1 s period. Its
  // AI waits meanwhile, as a player sitting down reacts a moment late.
  if (bot->getStandState() == UNIT_STAND_STATE_SIT &&
      !HasFoodOrDrinkAura(bot)) {
    ai->SetNextCheckDelay(kSitHoldMs);
  }
}

bool IsRestInProgress(Player* bot, bool* had_food_drink_aura) {
  if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() ||
      bot->IsInWater()) {
    return false;
  }
  const bool full_hp = bot->IsFullHealth();
  const bool full_mana =
      bot->GetMaxPower(POWER_MANA) == 0 ||
      bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA);
  if (full_hp && full_mana) {
    if (bot->getStandState() == UNIT_STAND_STATE_SIT) {
      bot->SetStandState(UNIT_STAND_STATE_STAND);
    }
    return false;
  }
  const bool has_aura = HasFoodOrDrinkAura(bot);
  if (has_aura) {
    if (had_food_drink_aura) {
      *had_food_drink_aura = true;
    }
    return true;
  }
  if (had_food_drink_aura && *had_food_drink_aura) {
    // Food/drink aura expired before reaching 100%: finish the step so the
    // brain can decide whether to eat again or move on.
    if (bot->getStandState() == UNIT_STAND_STATE_SIT) {
      bot->SetStandState(UNIT_STAND_STATE_STAND);
    }
    return false;
  }
  return bot->getStandState() == UNIT_STAND_STATE_SIT;
}

WalkStepResult TickLongWalk(Player* bot,
                            base::TimeTicks now,
                            PendingWalk* walk,
                            const DungeonNavConfig& dungeon_nav) {
  const auto stop_walk = [&]() {
    ClearLastMovement(bot);
    if (bot->isMoving()) {
      bot->StopMoving();
    }
  };
  if (!walk) {
    return {.error = "walk interrupted"};
  }
  if (!bot->IsAlive()) {
    stop_walk();
    return {.error = "walk interrupted: you died"};
  }
  if (bot->IsInCombat()) {
    stop_walk();
    return {.error = "walk interrupted: you are in combat"};
  }
  if (bot->GetMapId() != walk->map_id) {
    stop_walk();
    return {.error = "walk interrupted: you changed map"};
  }
  const float dist_to_dest =
      bot->GetExactDist(walk->dest.x, walk->dest.y, walk->dest.z);
  const wlb::snapshot::Vec3 pos{bot->GetPositionX(), bot->GetPositionY(),
                                bot->GetPositionZ()};
  switch (CheckWalkProgress(pos, dist_to_dest, now, &walk->progress)) {
    case WalkProgressVerdict::kArrived:
      stop_walk();
      return {.arrived = true};
    case WalkProgressVerdict::kStuck:
      stop_walk();
      return {.error = "stuck"};
    case WalkProgressVerdict::kContinue:
      break;
  }

  if (walk->has_step && bot->isMoving()) {
    const float dist_to_step = bot->GetExactDist(
        walk->step_dest.x, walk->step_dest.y, walk->step_dest.z);
    // Hold an active seam hop until the bot nears its validated landing point
    // (or the spline finishes), so kWalkStepAdvanceYd (10 yd) does not repath
    // mid-hop and prematurely exhaust kMaxConsecutiveSeamHops.
    const float advance_yd =
        walk->consecutive_seam_hops > 0 ? 2.0f : kWalkStepAdvanceYd;
    if (dist_to_step > advance_yd) {
      return {};
    }
  }
  // Not moving yet (or pushed off the path, or deferred while casting/locked):
  // ask again, but not every tick.
  if (!walk->last_path_at.is_null() &&
      now - walk->last_path_at < kWalkRepathEvery) {
    return {};
  }
  walk->last_path_at = now;

  const base::TimeTicks path_start = base::TimeTicks::Now();
  constexpr uint32_t kTypeOk =
      PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
  G3D::Vector3 next_pt;
  bool found = false;

  {
    PathGenerator path(bot);
    path.CalculatePath(walk->dest.x, walk->dest.y, walk->dest.z);
    const uint32_t path_type = path.GetPathType();
    if ((path_type & ~kTypeOk) == 0 && path.GetPath().size() >= 2) {
      const G3D::Vector3 cand = PointAlongPath(path.GetPath(), kWalkStepYd);
      const G3D::Vector3& end = path.GetActualEndPosition();
      const float end_dist_to_dest =
          std::hypot(end.x - walk->dest.x, end.y - walk->dest.y);
      if (bot->GetExactDist(cand.x, cand.y, cand.z) >= 2.0f &&
          ((path_type & PATHFIND_NORMAL) != 0 ||
           end_dist_to_dest + 2.0f < dist_to_dest)) {
        next_pt = cand;
        found = true;
      }
    }
  }

  if (!found && dist_to_dest <= kWalkNearEnoughArrivalYd) {
    stop_walk();
    return {.arrived = true, .path_time = base::TimeTicks::Now() - path_start};
  }

  if (!found) {
    Map* map = bot->GetMap();
    const float base_angle = bot->GetAngle(walk->dest.x, walk->dest.y);
    const float step_len = std::min(kWalkStepYd, dist_to_dest);
    static constexpr float kOffsets[] = {0.0f, 0.7f, -0.7f};
    for (float offset : kOffsets) {
      const float angle = base_angle + offset;
      const float tx = bot->GetPositionX() + std::cos(angle) * step_len;
      const float ty = bot->GetPositionY() + std::sin(angle) * step_len;
      float tz = walk->dest.z;
      if (map) {
        const float h = map->GetHeight(bot->GetPhaseMask(), tx, ty,
                                       bot->GetPositionZ() + 10.0f);
        if (h != INVALID_HEIGHT && h != VMAP_INVALID_HEIGHT_VALUE) {
          tz = h;
        }
      }
      PathGenerator path(bot);
      path.CalculatePath(tx, ty, tz);
      if ((path.GetPathType() & ~kTypeOk) == 0 && path.GetPath().size() >= 2) {
        const G3D::Vector3 cand = PointAlongPath(path.GetPath(), kWalkStepYd);
        if (bot->GetExactDist(cand.x, cand.y, cand.z) >= 3.0f) {
          next_pt = cand;
          found = true;
          break;
        }
      }
    }
  }

  const base::TimeDelta path_time = base::TimeTicks::Now() - path_start;
  if (!found) {
    // Disconnected dungeon navmesh seam (e.g. an entrance stair/ledge drop-off
    // where Detour cannot cross between polygons): if a terrain- and navmesh-
    // validated hop exists toward the destination, launch a direct spline
    // across the seam (capped at kMaxConsecutiveSeamHops) so the next tick
    // resumes normal navmesh pathing.
    constexpr uint8_t kMaxConsecutiveSeamHops = 2;
    if (walk->consecutive_seam_hops < kMaxConsecutiveSeamHops) {
      pb::Vec3 hop;
      std::vector<pb::Vec3> hop_path;
      if (FindDungeonSeamHop(bot->GetMap(), bot->GetPhaseMask(), pos,
                             bot->GetAngle(walk->dest.x, walk->dest.y),
                             dist_to_dest, dungeon_nav, &hop, &hop_path)) {
        // Defer instead of aborting as "stuck" when movement is transiently
        // prevented (e.g., casting a buff, brief root/stun, or controlled
        // motion); CheckWalkProgress's 10 s timeout bounds true stalls.
        if (bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) ||
            bot->HasUnitState(UNIT_STATE_NOT_MOVE) ||
            bot->IsMovementPreventedByCasting() ||
            bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) !=
                NULL_MOTION_TYPE) {
          return {.path_time = path_time};
        }
        // Clear overlying active generators (MoveIdle only mutates slot 0)
        // before launching a spline along the validated terrain polyline onto
        // the navmesh landing point.
        bot->GetMotionMaster()->Clear(/*reset=*/false);
        Movement::MoveSplineInit init(bot);
        if (hop_path.size() >= 2) {
          Movement::PointsArray points;
          points.reserve(hop_path.size());
          for (const pb::Vec3& p : hop_path) {
            points.emplace_back(p.x, p.y, p.z);
          }
          init.MovebyPath(points);
        } else {
          init.MoveTo(hop.x, hop.y, hop.z, /*generatePath=*/false);
        }
        init.SetWalk(false);
        if (init.Launch() > 0) {
          ++walk->consecutive_seam_hops;
          walk->step_dest = hop;
          walk->has_step = true;
          RecordLastMovement(bot, hop.x, hop.y, hop.z);
          return {.path_time = path_time};
        }
      }
    }
    ClearLastMovement(bot);
    if (bot->isMoving()) {
      bot->StopMoving();
    }
    return {.error = "stuck", .path_time = path_time};
  }

  walk->consecutive_seam_hops = 0;
  walk->step_dest = {next_pt.x, next_pt.y, next_pt.z};
  walk->has_step = true;
  if (!WalkTo(bot, next_pt.x, next_pt.y, next_pt.z)) {
    return {.error = "stuck", .path_time = path_time};
  }
  return {.path_time = path_time};
}

namespace {

// The loot slots of `creature` that `bot` may take now, as its loot window
// shows them: built from the same LootView the client gets (round robin,
// roll winners, master loot, quest and free-for-all items), with the
// permission Player::SendLoot gives for a corpse. Both of LootCorpseNow's
// loops use it, so the bot never takes an item on another's turn nor reports
// one it may not take (review of f40b0b6, finding 9).
std::vector<uint8_t> TakeableLootSlots(Player* bot, Creature* creature) {
  PermissionTypes permission = NONE_PERMISSION;
  if (Group* group = creature->GetLootRecipientGroup()) {
    if (bot->GetGroup() == group) {
      switch (group->GetLootMethod()) {
        case MASTER_LOOT:
          permission = group->GetMasterLooterGuid() == bot->GetGUID()
                           ? MASTER_PERMISSION
                           : RESTRICTED_PERMISSION;
          break;
        case FREE_FOR_ALL:
          permission = ALL_PERMISSION;
          break;
        case ROUND_ROBIN:
          permission = ROUND_ROBIN_PERMISSION;
          break;
        default:
          permission = GROUP_PERMISSION;
          break;
      }
    }
  } else if (creature->GetLootRecipient() == bot) {
    permission = OWNER_PERMISSION;
  }
  ByteBuffer view;
  view << LootView(creature->loot, bot, permission);
  std::vector<uint8_t> slots;
  view.read_skip<uint32_t>();  // Gold.
  const uint8_t count = view.read<uint8_t>();
  for (uint8_t i = 0; i < count; ++i) {
    const auto slot = view.read<uint8_t>();
    view.read_skip(5 * sizeof(uint32_t));  // Item id, count, display, random.
    const auto type = view.read<uint8_t>();
    if (type == LOOT_SLOT_TYPE_ALLOW_LOOT || type == LOOT_SLOT_TYPE_OWNER) {
      slots.push_back(slot);
    }
  }
  return slots;
}

}  // namespace

bool HasTakeableLoot(Player* bot, Creature* creature) {
  if (!bot || !creature || !creature->IsInWorld()) {
    return false;
  }
  const std::vector<uint8_t> slots = TakeableLootSlots(bot, creature);
  size_t storable_items = 0;
  for (uint8_t slot : slots) {
    const LootItem* item = creature->loot.LootItemInSlot(slot, bot);
    if (!item || item->is_looted) {
      continue;
    }
    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item->itemid,
                             item->count) == EQUIP_ERR_OK) {
      ++storable_items;
    }
  }
  return HasCorpseTakeableLoot(
      creature->isDead(), creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE),
      bot->isAllowedToLoot(creature), creature->loot.gold, storable_items);
}

std::string LootCorpseNow(Player* bot, Creature* creature) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return "not a bot";
  }
  if (!creature || !creature->IsInWorld() || !creature->isDead()) {
    return "nothing to loot";
  }
  if (!bot->isAllowedToLoot(creature) ||
      !creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)) {
    return "nothing to loot on " + creature->GetName();
  }
  if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE)) {
    return base::StringPrintf("%s is %.0f yd away", creature->GetName().c_str(),
                              bot->GetDistance(creature));
  }
  ClearLastMovement(bot);
  if (bot->isMoving()) {
    bot->StopMoving();
  }
  if (AiObjectContext* ctx = ai->GetAiObjectContext()) {
    if (auto* avail = ctx->GetValue<LootObjectStack*>("available loot");
        avail && avail->Get()) {
      avail->Get()->Remove(creature->GetGUID());
    }
    if (auto* lt = ctx->GetValue<LootObject>("loot target");
        lt && lt->Get().guid == creature->GetGUID()) {
      lt->Set(LootObject());
    }
  }
  const ObjectGuid guid = creature->GetGUID();
  if (const ObjectGuid prev = bot->GetLootGUID(); prev && prev != guid) {
    bot->GetSession()->DoLootRelease(prev);
  }
  WorldPacket open_pkt(CMSG_LOOT, 8);
  open_pkt << guid;
  open_pkt.rpos(0);
  bot->GetSession()->HandleLootOpcode(open_pkt);
  if (bot->GetLootGUID() != guid) {
    return "nothing to loot on " + creature->GetName();
  }
  if (creature->loot.gold > 0) {
    WorldPacket money_pkt(CMSG_LOOT_MONEY, 0);
    bot->GetSession()->HandleLootMoneyOpcode(money_pkt);
  }
  for (const uint8_t slot : TakeableLootSlots(bot, creature)) {
    WorldPacket item_pkt(CMSG_AUTOSTORE_LOOT_ITEM, 1);
    item_pkt << static_cast<uint8_t>(slot);
    item_pkt.rpos(0);
    bot->GetSession()->HandleAutostoreLootItemOpcode(item_pkt);
  }
  // What is still there that the bot may take, and why: a player sees
  // "Inventory is full" only when that is the reason. Items being rolled for,
  // held for the master looter or on another's round-robin turn are not the
  // bot's to take now (the roll is its own thing: `loot_roll` events), so the
  // step is done.
  std::string no_room;
  std::string refused;
  for (const uint8_t slot : TakeableLootSlots(bot, creature)) {
    const LootItem* left = creature->loot.LootItemInSlot(slot, bot);
    if (left == nullptr) {
      continue;
    }
    const ItemTemplate* proto = sObjectMgr->GetItemTemplate(left->itemid);
    const std::string item_name = proto ? proto->Name1 : "an item";
    ItemPosCountVec dest;
    switch (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, left->itemid,
                                 left->count)) {
      case EQUIP_ERR_OK:
        refused = item_name;  // Room, yet not taken: some other rule.
        break;
      case EQUIP_ERR_BAG_FULL:
      case EQUIP_ERR_INVENTORY_FULL:
        no_room = item_name;
        break;
      default:
        refused = item_name;  // Unique, quest item limit, ...
        break;
    }
  }
  WorldPacket release_pkt(CMSG_LOOT_RELEASE, 8);
  release_pkt << guid;
  release_pkt.rpos(0);
  bot->GetSession()->HandleLootReleaseOpcode(release_pkt);
  ai->ResetActionDuration();
  if (!no_room.empty()) {
    return "Inventory is full";
  }
  if (!refused.empty()) {
    return "You can't carry " + refused;
  }
  return "";
}

namespace {

ExecuteResult ExecuteLootOption(Player* bot, const OptionTarget& target) {
  Creature* creature =
      ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(target.guid));
  if (!creature || !creature->IsInWorld()) {
    return {target.name + " is gone"};
  }
  if (!creature->isDead()) {
    return {target.name + " isn't dead"};
  }
  LootObject loot(bot, creature->GetGUID());
  if (loot.IsEmpty() || !bot->isAllowedToLoot(creature) ||
      !creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)) {
    return {"nothing to loot on " + target.name};
  }
  if (IsInLootRange(bot, creature)) {
    std::string error = LootCorpseNow(bot, creature);
    const bool used = error.empty();
    return {std::move(error), used};
  }
  if (!WalkNextTo(bot, creature)) {
    return {"no path to " + target.name};
  }
  return {};
}

ExecuteResult ExecuteObjectOption(Player* bot, const OptionTarget& target) {
  GameObject* go =
      ObjectAccessor::GetGameObject(*bot, ParseSnapshotGuid(target.guid));
  if (!go || !go->IsInWorld() || !go->isSpawned()) {
    return {target.name + " is gone"};
  }
  if (IsInUseRange(bot, go)) {
    std::string error = UseObjectNow(bot, go);
    const bool used = error.empty();
    return {std::move(error), used};
  }
  // Out of reach: walk there first, as a player would before clicking. The
  // world script uses it on arrival, and the harness times the step out if
  // the body never gets there.
  if (!WalkNextTo(bot, go)) {
    return {"no path to " + target.name};
  }
  return {};
}

ExecuteResult ExecuteTalkOption(Player* bot, const OptionTarget& target) {
  if (bot->IsInCombat()) {
    return {"you are in combat"};
  }
  Creature* npc =
      ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(target.guid));
  if (!npc || !npc->IsInWorld() || !npc->IsAlive()) {
    return {target.name + " is gone"};
  }
  if (IsInTalkRange(bot, npc)) {
    TalkResult res = TalkToNpcNow(bot, npc);
    const bool used = res.error.empty();
    return {std::move(res.error), used, std::move(res.info)};
  }
  if (!WalkNextTo(bot, npc)) {
    return {"no path to " + target.name};
  }
  return {};
}

ExecuteResult ExecuteRestOption(Player* bot, PlayerbotAI* ai) {
  if (bot->IsInCombat()) {
    return {"you are in combat"};
  }
  if (bot->IsMounted()) {
    return {"you are mounted"};
  }
  if (bot->IsInWater()) {
    return {"you are swimming"};
  }
  ClearLastMovement(bot);
  if (bot->isMoving()) {
    bot->StopMoving();
  }
  const bool used_consumable = TryEatAndDrink(bot, ai);
  // No ResetActionDuration here: eat / drink set the body's action duration
  // to the meal, and playerbots stands a sitting bot up as soon as its next
  // check is due (PlayerbotAI::UpdateAIInternal), which would end the meal.
  if (!used_consumable) {
    bot->SetStandState(UNIT_STAND_STATE_SIT);
    ai->SetNextCheckDelay(kSitHoldMs);
  }
  return {};
}

ExecuteResult ExecuteWalkOption(Player* bot,
                                const OptionTarget& target,
                                PendingWalk* walk,
                                const DungeonNavConfig& dungeon_nav) {
  if (bot->IsInCombat()) {
    return {"you are in combat"};
  }
  if (!target.pos || !walk) {
    return {"no destination for " + target.name};
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  *walk = PendingWalk{
      .map_id = bot->GetMapId(),
      .dest = *target.pos,
      .step_dest = {},
      .has_step = false,
      .progress =
          {
              .progress_pos = {bot->GetPositionX(), bot->GetPositionY(),
                               bot->GetPositionZ()},
              .last_progress_at = now,
              .best_dist_to_dest = bot->GetExactDist(
                  target.pos->x, target.pos->y, target.pos->z),
              .last_closer_at = now,
          },
      .last_path_at = {},
  };
  const WalkStepResult step = TickLongWalk(bot, now, walk, dungeon_nav);
  if (!step.error.empty()) {
    return {step.error, /*used=*/false, "", step.path_time};
  }
  return {"", /*used=*/step.arrived, "", step.path_time};
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

ExecuteResult ExecuteUseItemOption(Player* bot, const OptionTarget& target) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {"no session"};
  }
  constexpr std::string_view kPrefix = "item:";
  if (!target.guid.starts_with(kPrefix)) {
    return {"invalid item option"};
  }
  std::string_view id_str = target.guid;
  id_str.remove_prefix(kPrefix.size());
  uint32_t entry = 0;
  const auto [end, ec] =
      std::from_chars(id_str.data(), id_str.data() + id_str.size(), entry);
  if (ec != std::errc() || end != id_str.data() + id_str.size() || entry == 0) {
    return {"invalid item id"};
  }
  const BagSlotItem* found = nullptr;
  uint32_t item_spell_id = 0;
  int spell_slot = -1;
  std::vector<BagSlotItem> items = BagItems(bot);
  for (const BagSlotItem& slot_item : items) {
    if (!slot_item.item || slot_item.item->GetEntry() != entry) {
      continue;
    }
    const ItemTemplate* p = slot_item.item->GetTemplate();
    if (!p || p->InventoryType != INVTYPE_NON_EQUIP) {
      continue;
    }
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
      if (p->Spells[i].SpellId > 0 &&
          p->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE) {
        if (p->Spells[i].SpellCharges != 0 &&
            slot_item.item->GetSpellCharges(i) == 0) {
          continue;
        }
        found = &slot_item;
        item_spell_id = static_cast<uint32_t>(p->Spells[i].SpellId);
        spell_slot = i;
        break;
      }
    }
    if (found) {
      break;
    }
  }
  if (!found || item_spell_id == 0) {
    return {"can't use " + target.name + " now"};
  }
  Item* item = found->item;
  const ItemTemplate* proto = item->GetTemplate();
  if (!proto || bot->CanUseItem(item) != EQUIP_ERR_OK) {
    return {"can't use " + target.name + " now"};
  }
  if (bot->HasSpellCooldown(item_spell_id)) {
    return {target.name + " is on cooldown"};
  }
  if (bot->m_mover != bot) {
    return {"can't use " + target.name + " now"};
  }
  const SpellInfo* item_spell = sSpellMgr->GetSpellInfo(item_spell_id);
  if (!item_spell) {
    return {"can't use " + target.name + " now"};
  }
  if (bot->IsInCombat()) {
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
      if (proto->Spells[i].SpellId > 0) {
        if (const SpellInfo* si = sSpellMgr->GetSpellInfo(
                static_cast<uint32_t>(proto->Spells[i].SpellId));
            si && si->HasAttribute(SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL)) {
          return {"can't use " + target.name + " in combat"};
        }
      }
    }
  }
  if (item_spell->CalcCastTime() > 0) {
    bot->StopMoving();
  }
  Unit* item_target = bot->GetSelectedUnit();
  if (item_spell->IsPositive() && item_target && item_target != bot &&
      !bot->IsFriendlyTo(item_target)) {
    item_target = bot;
  }
  const ObjectGuid item_guid = item->GetGUID();
  const uint32_t count_before = bot->GetItemCount(proto->ItemId);
  const int32_t charges_before =
      spell_slot >= 0 ? item->GetSpellCharges(spell_slot) : 0;
  Unit* aura_unit = item_target ? item_target : bot;
  const ObjectGuid aura_unit_guid = aura_unit->GetGUID();
  bool had_aura_before = false;
  int32_t aura_dur_before = -1;
  if (const Aura* aura_before = aura_unit->GetAura(item_spell_id)) {
    had_aura_before = true;
    aura_dur_before = aura_before->GetDuration();
  }
  const uint32_t last_potion_before = bot->GetLastPotionId();
  const bool gcd_before =
      bot->GetGlobalCooldownMgr().HasGlobalCooldown(item_spell);
  const size_t queue_before = QueuedSpellCount(bot, item_spell_id);
  const void* vehicle_before = bot->GetVehicle();
  WorldPacket p(CMSG_USE_ITEM);
  p << found->bag << found->slot << static_cast<uint8_t>(0) << item_spell_id
    << item_guid << static_cast<uint32_t>(0) << static_cast<uint8_t>(0);
  SpellCastTargets targets;
  if (item_target) {
    targets.SetUnitTarget(item_target);
  }
  targets.Write(p);
  p.rpos(0);
  session->HandleUseItemOpcode(p);
  const bool count_dropped = bot->GetItemCount(proto->ItemId) < count_before;
  // Re-fetch by bag/slot + GUID after HandleUseItemOpcode to avoid dangling
  // `Item*` dereference if an OnItemUse script destroyed/replaced the stack.
  bool charges_dropped = false;
  if (!count_dropped && spell_slot >= 0 &&
      proto->Spells[spell_slot].SpellCharges != 0) {
    if (Item* post_item = bot->GetItemByPos(found->bag, found->slot);
        post_item && post_item->GetGUID() == item_guid) {
      charges_dropped =
          post_item->GetSpellCharges(spell_slot) != charges_before;
    }
  }
  const bool cast_started = IsPreparingOrChannelingSpell(bot, item_spell_id);
  const bool queued_now = QueuedSpellCount(bot, item_spell_id) > queue_before;
  const bool cd_started = bot->HasSpellCooldown(item_spell_id);
  const bool gcd_started =
      !gcd_before && bot->GetGlobalCooldownMgr().HasGlobalCooldown(item_spell);
  Unit* post_aura_unit = (aura_unit_guid == bot->GetGUID())
                             ? static_cast<Unit*>(bot)
                             : ObjectAccessor::GetUnit(*bot, aura_unit_guid);
  const Aura* aura_after =
      post_aura_unit ? post_aura_unit->GetAura(item_spell_id) : nullptr;
  const bool aura_applied =
      aura_after != nullptr &&
      (!had_aura_before || aura_after->GetDuration() > aura_dur_before);
  const bool potion_recorded = last_potion_before != proto->ItemId &&
                               bot->GetLastPotionId() == proto->ItemId;
  const bool vehicle_changed = bot->GetVehicle() != vehicle_before;
  if (!count_dropped && !charges_dropped && !cast_started && !queued_now &&
      !cd_started && !gcd_started && !aura_applied && !potion_recorded &&
      !vehicle_changed) {
    return {"can't use " + target.name + " now"};
  }
  // Cast-time or SpellQueue-pending items stay pending in OptionTargetProgress
  // until the specific item spell finishes.
  const bool still_pending = cast_started || queued_now;
  return {.used = !still_pending,
          .info = still_pending ? "" : "used " + target.name,
          .pending_spell_id = still_pending ? item_spell_id : 0};
}

ExecuteResult ExecuteEquipItemOption(Player* bot, const OptionTarget& target) {
  if (bot->IsInCombat()) {
    return {"you are in combat"};
  }
  constexpr std::string_view kPrefix = "equip:";
  if (!target.guid.starts_with(kPrefix)) {
    return {"invalid equip option"};
  }
  std::string_view id_str = target.guid;
  id_str.remove_prefix(kPrefix.size());
  uint32_t entry = 0;
  const auto [end, ec] =
      std::from_chars(id_str.data(), id_str.data() + id_str.size(), entry);
  if (ec != std::errc() || end != id_str.data() + id_str.size() || entry == 0) {
    return {"invalid item id"};
  }
  for (const BagSlotItem& slot_item : BagItems(bot)) {
    if (!slot_item.item || slot_item.item->GetEntry() != entry) {
      continue;
    }
    const ItemTemplate* proto = slot_item.item->GetTemplate();
    uint16_t dest = 0;
    if (!proto || !PickEquipSlot(bot, slot_item.item, proto, &dest)) {
      return {"cannot equip " + target.name};
    }
    const uint16_t src = static_cast<uint16_t>(
        (static_cast<uint16_t>(slot_item.bag) << 8) | slot_item.slot);
    bot->SwapItem(src, dest);
    Item* now_equipped = bot->GetItemByPos(dest);
    if (!now_equipped || now_equipped->GetEntry() != entry) {
      return {"couldn't equip " + target.name};
    }
    return {.used = true, .info = "equipped " + target.name};
  }
  return {target.name + " is not in your bags"};
}

}  // namespace

bool IsSpellCastPending(const Player* bot, uint32_t spell_id) {
  return QueuedSpellCount(bot, spell_id) > 0 ||
         IsPreparingOrChannelingSpell(bot, spell_id);
}

ExecuteResult ExecuteOption(Player* bot,
                            const OptionTarget& target,
                            PendingWalk* walk,
                            const DungeonNavConfig& dungeon_nav) {
  if (target.kind == OptionTarget::Kind::kKeep) {
    return {};  // The body carries on with what it was doing.
  }
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {"not a bot"};
  }
  if (target.kind == OptionTarget::Kind::kUse && target.guid == "rez:accept") {
    if (bot->IsAlive()) {
      return {"you are already alive"};
    }
    if (!bot->isResurrectRequested()) {
      return {"no resurrection pending"};
    }
    if (bot->HasPreventResurectionAura()) {
      return {"resurrection is currently prevented"};
    }
    bot->ResurectUsingRequestData();
    return {.used = true, .info = "accepted resurrection"};
  }
  if (target.kind == OptionTarget::Kind::kUse && target.guid == "rez:release") {
    if (bot->IsAlive()) {
      return {"you are already alive"};
    }
    if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
      return {"already released"};
    }
    if (bot->HasPreventResurectionAura()) {
      return {"release spirit is currently prevented"};
    }
    WorldSession* session = bot->GetSession();
    if (!session) {
      return {"no session"};
    }
    // Route through AzerothCore's real repop opcode handler so JustDied,
    // RemovePet, BuildPlayerRepop (corpse + ghost aura/flag), and
    // RepopAtGraveyard all run exactly as for a human client.
    WorldPacket p(CMSG_REPOP_REQUEST, 1);
    p << uint8_t(0);
    p.rpos(0);
    session->HandleRepopRequestOpcode(p);
    return {.used = true, .info = "released spirit"};
  }
  if (target.kind == OptionTarget::Kind::kLeaveGroup) {
    WorldSession* session = bot->GetSession();
    if (!session || !bot->GetGroup()) {
      return {"you are not in a group"};
    }
    WorldPacket pkt(CMSG_GROUP_DISBAND, 0);
    session->HandleGroupDisbandOpcode(pkt);
    return {.used = true, .info = "left the group"};
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("roll:")) {
    WorldSession* session = bot->GetSession();
    if (!session) {
      return {"no session"};
    }
    // Format: roll:<item_guid>:<slot>:<roll_type>
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        target.guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t slot = 0;
    uint32_t vote = 0;
    if (parts.size() != 4 || !ParseUint32(parts[2], &slot) ||
        !ParseUint32(parts[3], &vote) || vote > 2) {
      return {"invalid loot roll"};
    }
    WorldPacket p(CMSG_LOOT_ROLL, 8 + 4 + 1);
    p << ParseSnapshotGuid(parts[1]) << slot << static_cast<uint8_t>(vote);
    p.rpos(0);
    session->HandleLootRoll(p);
    return {.used = true, .info = target.name};
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("ready:")) {
    WorldSession* session = bot->GetSession();
    if (!session) {
      return {"no session"};
    }
    if (!bot->GetGroup()) {
      BotManager::ClearPendingReadyCheck(bot);
      return {"you are not in a group"};
    }
    const uint8_t state = target.guid == "ready:yes" ? 1 : 0;
    WorldPacket p(MSG_RAID_READY_CHECK, 1);
    p << state;
    p.rpos(0);
    session->HandleRaidReadyCheckOpcode(p);
    BotManager::ClearPendingReadyCheck(bot);
    return {.used = true,
            .info = state ? "confirmed ready check" : "declined ready check"};
  }
  if (!bot->IsAlive()) {
    return {"you are dead"};  // A choice from before the bot died.
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("qshare:")) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        target.guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t qid = 0;
    if (parts.size() != 3 || !ParseUint32(parts[2], &qid)) {
      return {"invalid shared quest"};
    }
    BotManager::ClearPendingSharedQuest(bot);
    if (parts[1] == "decline") {
      bot->SetDivider(ObjectGuid::Empty);
      return {.used = true, .info = "declined shared quest"};
    }
    const Quest* quest = sObjectMgr->GetQuestTemplate(qid);
    if (!quest || !bot->GetDivider()) {
      return {"shared quest is no longer available"};
    }
    if (!bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false)) {
      bot->SetDivider(ObjectGuid::Empty);
      return {"cannot accept shared quest"};
    }
    bot->AddQuestAndCheckCompletion(quest, nullptr);
    bot->SetDivider(ObjectGuid::Empty);
    ai->RecordQuestAccepted(qid);
    return {.used = true, .info = "accepted shared quest " + quest->GetTitle()};
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("lfg:")) {
    if (target.guid.starts_with("lfg:queue:")) {
      if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetGUID())) {
        return {"only the group leader can queue for Dungeon Finder"};
      }
      uint32_t role_mask = 0;
      if (!ParseUint32(std::string_view(target.guid).substr(10), &role_mask) ||
          role_mask == 0) {
        return {"invalid LFG role"};
      }
      const uint32_t did = FindRandomLfgDungeonId(bot);
      if (did == 0) {
        return {"no random dungeon available for your level"};
      }
      lfg::LfgDungeonSet dungeons = {did};
      sLFGMgr->JoinLfg(bot, static_cast<uint8_t>(role_mask), dungeons, "");
      if (sLFGMgr->GetState(bot->GetGUID()) == lfg::LFG_STATE_NONE) {
        return {"could not join Dungeon Finder queue"};
      }
      return {.used = true, .info = "queued for Random Dungeon"};
    }
    if (target.guid == "lfg:leave") {
      sLFGMgr->LeaveLfg(bot->GetGUID());
      BotManager::ClearPendingLfgProposal(bot);
      return {.used = true, .info = "left Dungeon Finder queue"};
    }
    if (target.guid.starts_with("lfg:role:")) {
      uint32_t role_mask = 0;
      if (!ParseUint32(std::string_view(target.guid).substr(9), &role_mask) ||
          role_mask == 0) {
        return {"invalid LFG role"};
      }
      if (Group* g = bot->GetGroup()) {
        sLFGMgr->UpdateRoleCheck(g->GetGUID(), bot->GetGUID(),
                                 static_cast<uint8_t>(role_mask));
        return {.used = true, .info = "confirmed Dungeon Finder role"};
      }
      return {"you are not in a group"};
    }
    if (target.guid == "lfg:tele_in" || target.guid == "lfg:tele_out") {
      sLFGMgr->TeleportPlayer(bot, target.guid == "lfg:tele_out");
      return {.used = true,
              .info = target.guid == "lfg:tele_out"
                          ? "teleported out of LFG dungeon"
                          : "teleported to LFG dungeon"};
    }
    if (target.guid.starts_with("lfg:accept:") ||
        target.guid.starts_with("lfg:decline:")) {
      const bool accept = target.guid.starts_with("lfg:accept:");
      uint32_t pid = 0;
      if (!ParseUint32(std::string_view(target.guid).substr(accept ? 11 : 12),
                       &pid)) {
        return {"invalid LFG proposal"};
      }
      sLFGMgr->UpdateProposal(pid, bot->GetGUID(), accept);
      BotManager::ClearPendingLfgProposal(bot);
      return {.used = true,
              .info = accept ? "accepted Dungeon Finder proposal"
                             : "declined Dungeon Finder proposal"};
    }
    return {"unknown LFG action"};
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("follow:")) {
    Player* leader = ObjectAccessor::FindConnectedPlayer(
        ParseSnapshotGuid(std::string_view(target.guid).substr(7)));
    if (!leader || !leader->IsInWorld() || !leader->IsAlive() ||
        leader->GetMapId() != bot->GetMapId() ||
        leader->GetInstanceId() != bot->GetInstanceId()) {
      return {"party leader is not nearby"};
    }
    RecordLastMovement(bot, leader->GetPositionX(), leader->GetPositionY(),
                       leader->GetPositionZ());
    bot->GetMotionMaster()->MoveFollow(leader, 4.0f, std::numbers::pi_v<float>);
    return {.used = true, .info = "following " + leader->GetName()};
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("mark:")) {
    Group* group = bot->GetGroup();
    if (!group || (!group->IsLeader(bot->GetGUID()) &&
                   !group->IsAssistant(bot->GetGUID()))) {
      return {"you cannot mark targets"};
    }
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        target.guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t icon = 0;
    if (parts.size() != 3 || !ParseUint32(parts[2], &icon) || icon >= 8) {
      return {"invalid raid mark"};
    }
    Unit* u = ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(parts[1]));
    if (!u || !u->IsInWorld() || !u->IsAlive()) {
      return {"target is gone"};
    }
    group->SetTargetIcon(static_cast<uint8_t>(icon), bot->GetGUID(),
                         u->GetGUID());
    return {.used = true, .info = "marked " + u->GetName() + " with Skull"};
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("mount:")) {
    uint32_t spell_id = 0;
    if (!ParseUint32(std::string_view(target.guid).substr(6), &spell_id)) {
      return {"invalid mount"};
    }
    if (bot->IsInCombat()) {
      return {"cannot mount in combat"};
    }
    if (bot->isMoving()) {
      bot->StopMoving();
    }
    bot->CastSpell(bot, spell_id, false);
    const bool casting = IsSpellCastPending(bot, spell_id);
    if (!casting && !bot->IsMounted()) {
      return {"cannot mount now"};
    }
    return {.used = !casting,
            .info = casting ? "" : ("mounted " + target.name),
            .pending_spell_id = casting ? spell_id : 0};
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("npc_")) {
    if (bot->IsInCombat()) {
      return {"you are in combat"};
    }
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        target.guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    if (parts.size() < 2) {
      return {"invalid NPC action"};
    }
    Creature* npc =
        ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(parts[1]));
    if (!npc || !npc->IsInWorld() || !npc->IsAlive() ||
        !IsInTalkRange(bot, npc)) {
      return {"that NPC is not in talk range"};
    }
    bot->SetFacingToObject(npc);
    bot->SetSelection(npc->GetGUID());
    if (parts[0] == "npc_qaccept" && parts.size() == 3) {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {"no session"};
      }
      uint32_t qid = 0;
      if (!ParseUint32(parts[2], &qid)) {
        return {"invalid quest id"};
      }
      const Quest* quest = sObjectMgr->GetQuestTemplate(qid);
      if (!quest || !bot->CanTakeQuest(quest, false) ||
          !bot->CanAddQuest(quest, false)) {
        return {"cannot accept that quest"};
      }
      WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
      uint32_t unk1 = 0;
      p << npc->GetGUID() << quest->GetQuestId() << unk1;
      p.rpos(0);
      session->HandleQuestgiverAcceptQuestOpcode(p);
      ai->RecordQuestAccepted(quest->GetQuestId());
      return {.used = true, .info = "accepted " + quest->GetTitle()};
    }
    if (parts[0] == "npc_qturnin" && parts.size() == 4) {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {"no session"};
      }
      uint32_t qid = 0;
      uint32_t reward_idx = 0;
      if (!ParseUint32(parts[2], &qid) || !ParseUint32(parts[3], &reward_idx)) {
        return {"invalid quest turn-in"};
      }
      const Quest* quest = sObjectMgr->GetQuestTemplate(qid);
      const uint32_t choices = quest ? quest->GetRewChoiceItemsCount() : 0;
      const bool valid_idx =
          (choices == 0 && reward_idx == 0) || reward_idx < choices;
      if (!quest || !valid_idx ||
          bot->GetQuestStatus(qid) != QUEST_STATUS_COMPLETE ||
          bot->GetQuestRewardStatus(qid) ||
          !bot->CanRewardQuest(quest, false) ||
          !bot->CanRewardQuest(quest, reward_idx, false)) {
        return {"cannot turn in that quest"};
      }
      WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
      p << npc->GetGUID() << quest->GetQuestId() << reward_idx;
      p.rpos(0);
      session->HandleQuestgiverChooseRewardOpcode(p);
      ai->RecordQuestRewarded(quest->GetQuestId());
      return {.used = true, .info = "turned in " + quest->GetTitle()};
    }
    if (parts[0] == "npc_train") {
      Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
      if (!trainer || trainer->GetTrainerType() != Trainer::Type::Class ||
          !trainer->IsTrainerValidForPlayer(bot)) {
        return {"cannot train your class"};
      }
      const float discount = bot->GetReputationPriceDiscount(npc);
      uint32_t learned = 0;
      for (const Trainer::Spell& sp : trainer->GetSpells()) {
        if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer, &sp) ||
            !trainer->CanTeachSpell(bot, &sp)) {
          continue;
        }
        const int32_t cost = static_cast<int32_t>(sp.MoneyCost * discount);
        if (!bot->HasEnoughMoney(cost)) {
          continue;
        }
        const uint32_t money_before = bot->GetMoney();
        trainer->TeachSpell(npc, bot, sp.SpellId);
        if (!trainer->CanTeachSpell(bot, &sp) ||
            (cost > 0 && bot->GetMoney() != money_before)) {
          ++learned;
        }
      }
      if (learned == 0) {
        return {"no affordable spells to learn"};
      }
      return {.used = true,
              .info = "learned " + std::to_string(learned) + " class spell(s)"};
    }
    if (parts[0] == "npc_vendor") {
      std::vector<std::string> done;
      if (npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR)) {
        const uint32_t sold = SellGreyItemsTo(bot, npc->GetGUID());
        if (sold > 0) {
          done.push_back("sold " + std::to_string(sold) + " grey item(s)");
        }
      }
      if (npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) &&
          bot->DurabilityRepairAll(true, bot->GetReputationPriceDiscount(npc),
                                   false) > 0) {
        done.push_back("repaired gear");
      }
      if (done.empty()) {
        return {"nothing to sell or repair"};
      }
      return {.used = true, .info = base::JoinString(done, ", ")};
    }
    if (parts[0] == "npc_buy" && parts.size() == 3) {
      uint32_t item_entry = 0;
      if (!ParseUint32(parts[2], &item_entry)) {
        return {"invalid vendor item"};
      }
      const VendorItemData* vitems = npc->GetVendorItems();
      if (!vitems) {
        return {"vendor has no items"};
      }
      for (uint32_t slot = 0; slot < vitems->GetItemCount(); ++slot) {
        const VendorItem* vi = vitems->GetItem(slot);
        if (vi && vi->item == item_entry) {
          const ItemTemplate* proto = sObjectMgr->GetItemTemplate(item_entry);
          // Count 1 buys one vendor stack (e.g. 5 food/water or 200 arrows).
          if (!bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, item_entry, 1,
                                          NULL_BAG, NULL_SLOT)) {
            return {"could not buy " +
                    (proto ? proto->Name1 : std::string("item"))};
          }
          return {
              .used = true,
              .info = "bought " + (proto ? proto->Name1 : std::string("item"))};
        }
      }
      return {"item not sold by this vendor"};
    }
    if (parts[0] == "npc_fly_discover") {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {"no session"};
      }
      if (!session->SendLearnNewTaxiNode(npc)) {
        return {"flight path already known or unavailable"};
      }
      return {.used = true,
              .info = "discovered flight path at " + npc->GetName()};
    }
    if (parts[0] == "npc_fly" && parts.size() == 3) {
      uint32_t dest_node = 0;
      if (!ParseUint32(parts[2], &dest_node)) {
        return {"invalid flight destination"};
      }
      const uint32_t cur_node =
          sObjectMgr->GetNearestTaxiNode(*npc, bot->GetTeamId(true));
      if (cur_node == 0 ||
          !bot->ActivateTaxiPathTo({cur_node, dest_node}, npc)) {
        return {"could not start flight"};
      }
      return {.used = true, .info = "took flight path"};
    }
    return {"unknown NPC action"};
  }
  if (target.kind == OptionTarget::Kind::kLoot) {
    return ExecuteLootOption(bot, target);
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("talent:")) {
    if (bot->IsInCombat()) {
      return {"cannot spend talents in combat"};
    }
    const uint32_t before = bot->GetFreeTalentPoints();
    if (before == 0) {
      return {"no unspent talent points"};
    }
    uint32_t tab_page = 0;
    const std::string_view suffix = std::string_view(target.guid).substr(7);
    const auto [ptr, ec] =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), tab_page);
    if (ec != std::errc{} || ptr != suffix.data() + suffix.size() ||
        tab_page > 2) {
      return {"invalid talent tree"};
    }
    const uint32_t class_mask = bot->getClassMask();
    uint32_t target_tab_id = 0;
    for (uint32_t i = 0; i < sTalentTabStore.GetNumRows(); ++i) {
      const TalentTabEntry* entry = sTalentTabStore.LookupEntry(i);
      if (entry && (entry->ClassMask & class_mask) != 0 &&
          entry->tabpage == tab_page) {
        target_tab_id = entry->TalentTabID;
        break;
      }
    }
    if (target_tab_id == 0) {
      return {"invalid talent tree"};
    }
    std::vector<const TalentEntry*> tab_talents;
    for (uint32_t i = 0; i < sTalentStore.GetNumRows(); ++i) {
      const TalentEntry* t = sTalentStore.LookupEntry(i);
      if (t && t->TalentTab == target_tab_id) {
        tab_talents.push_back(t);
      }
    }
    std::sort(tab_talents.begin(), tab_talents.end(),
              [](const TalentEntry* a, const TalentEntry* b) {
                if (a->Row != b->Row) {
                  return a->Row < b->Row;
                }
                if (a->Col != b->Col) {
                  return a->Col < b->Col;
                }
                return a->TalentID < b->TalentID;
              });
    const uint8_t active_spec = bot->GetActiveSpec();
    auto learned_rank = [&](const TalentEntry* t) -> int {
      const PlayerTalentMap& tmap = bot->GetTalentMap();
      for (int r = MAX_TALENT_RANK - 1; r >= 0; --r) {
        const uint32_t sid = t->RankID[r];
        if (!sid) {
          continue;
        }
        auto it = tmap.find(sid);
        if (it != tmap.end() && it->second &&
            it->second->State != PLAYERSPELL_REMOVED &&
            it->second->IsInSpec(active_spec)) {
          return r;
        }
      }
      return -1;
    };
    uint32_t spent_in_tab = 0;
    for (const TalentEntry* t : tab_talents) {
      const int r = learned_rank(t);
      if (r >= 0) {
        spent_in_tab += static_cast<uint32_t>(r + 1);
      }
    }
    while (bot->GetFreeTalentPoints() > 0) {
      bool progressed = false;
      for (const TalentEntry* t : tab_talents) {
        if (t->Row * 5 > spent_in_tab) {
          continue;
        }
        if (t->DependsOn != 0) {
          const TalentEntry* prereq = sTalentStore.LookupEntry(t->DependsOn);
          if (!prereq ||
              learned_rank(prereq) < static_cast<int>(t->DependsOnRank)) {
            continue;
          }
        }
        const int cur = learned_rank(t);
        const int next = cur + 1;
        if (next >= MAX_TALENT_RANK || t->RankID[next] == 0) {
          continue;
        }
        const uint32_t free_before = bot->GetFreeTalentPoints();
        bot->LearnTalent(t->TalentID, static_cast<uint32_t>(next));
        if (bot->GetFreeTalentPoints() < free_before) {
          ++spent_in_tab;
          progressed = true;
          break;
        }
      }
      if (!progressed) {
        break;
      }
    }
    const uint32_t after = bot->GetFreeTalentPoints();
    if (after >= before) {
      return {"no learnable talents in " + target.name};
    }
    bot->SendTalentsInfoData(false);
    return {.used = true,
            .info = "spent " + std::to_string(before - after) +
                    " talent point(s) in " + target.name};
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("item:")) {
    return ExecuteUseItemOption(bot, target);
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("equip:")) {
    return ExecuteEquipItemOption(bot, target);
  }
  if (target.kind == OptionTarget::Kind::kGather ||
      target.kind == OptionTarget::Kind::kUse) {
    return ExecuteObjectOption(bot, target);
  }
  if (target.kind == OptionTarget::Kind::kTalk) {
    return ExecuteTalkOption(bot, target);
  }
  if (target.kind == OptionTarget::Kind::kRest) {
    return ExecuteRestOption(bot, ai);
  }
  if (target.kind == OptionTarget::Kind::kExplore ||
      target.kind == OptionTarget::Kind::kGoTo) {
    return ExecuteWalkOption(bot, target, walk, dungeon_nav);
  }
  if (target.kind == OptionTarget::Kind::kInvite) {
    WorldSession* session = bot->GetSession();
    if (!session) {
      return {"no session"};
    }
    std::string player_name = target.name;
    Player* other = normalizePlayerName(player_name)
                        ? ObjectAccessor::FindPlayerByName(player_name)
                        : nullptr;
    if (!other || !other->IsInWorld()) {
      return {target.name + " is not online"};
    }
    if (other == bot) {
      return {"you can't invite yourself"};
    }
    if (other->GetGroup() != nullptr) {
      return {target.name + " is already in a group"};
    }
    if (Group* g = bot->GetGroup();
        g && !g->IsLeader(bot->GetGUID()) && !g->IsAssistant(bot->GetGUID())) {
      return {"only the group leader can invite"};
    }
    WorldPacket pkt(CMSG_GROUP_INVITE, player_name.size() + 5);
    pkt << player_name << uint32(0);
    session->HandleGroupInviteOpcode(pkt);
    return {.used = true, .info = "invited " + target.name + " to group"};
  }
  if (target.kind == OptionTarget::Kind::kAcceptInvite) {
    WorldSession* session = bot->GetSession();
    if (!session || !bot->GetGroupInvite()) {
      return {"no pending group invite"};
    }
    WorldPacket pkt(CMSG_GROUP_ACCEPT, 4);
    pkt << uint32(0);
    session->HandleGroupAcceptOpcode(pkt);
    if (!bot->GetGroup()) {
      return {"couldn't join group"};
    }
    return {.used = true, .info = "joined " + target.name + "'s group"};
  }
  if (target.kind == OptionTarget::Kind::kDeclineInvite) {
    WorldSession* session = bot->GetSession();
    if (!session || !bot->GetGroupInvite()) {
      return {"no pending group invite"};
    }
    WorldPacket pkt(CMSG_GROUP_DECLINE, 0);
    session->HandleGroupDeclineOpcode(pkt);
    return {.used = true, .info = "declined group invite from " + target.name};
  }

  Unit* unit = ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(target.guid));
  if (!unit || !unit->IsInWorld()) {
    return {target.name + " is gone"};
  }
  AiObjectContext* context = ai->GetAiObjectContext();
  // Playerbots returns null for a value name it doesn't know (a renamed one
  // after an update): fail the choice, never dereference it.
  auto* current_target = context->GetValue<Unit*>("current target");
  if (!current_target) {
    return {"the body can't do that (playerbots values missing)"};
  }
  switch (target.kind) {
    case OptionTarget::Kind::kAttack:
      // Playerbots' attack action attacks the "current target" and runs its
      // own checks; the combat strategies take over from there.
      {
        ClearAvoidedTargetForBot(bot->GetGUID().GetRawValue(),
                                 unit->GetGUID().GetRawValue());
        Unit* old_target = current_target->Get();
        current_target->Set(unit);
        if (!ai->DoSpecificAction("attack", Event(), /*silent=*/true)) {
          // Playerbots' AttackAction also refuses when the body already has
          // this target in attack mode (e.g. still closing in on it). That is
          // the choice already being carried out, not a failure: reporting
          // it woke the brain, which chose the same again, in a loop
          // (6 wakes in ~1 s, 2026-11-17).
          if (old_target == unit || bot->GetVictim() == unit) {
            ai->ResetActionDuration();
            return {};
          }
          // Playerbots' AttackAction refuses targets that are currently out of
          // line of sight (e.g. around a dungeon corridor bend or behind a
          // hill). Walk toward valid targets instead of failing in a loop.
          if (ShouldApproachOutOfLosTarget(unit->IsAlive(),
                                           bot->IsFriendlyTo(unit),
                                           bot->IsValidAttackTarget(unit),
                                           bot->IsWithinLOSInMap(unit)) &&
              WalkNextTo(bot, unit)) {
            ai->ResetActionDuration();
            return {};
          }
          // Refused: leave the body's target as it was, so nothing acts on
          // the refused one later.
          current_target->Set(old_target);
          return {"can't attack " + target.name + " now"};
        }
        ai->ResetActionDuration();
      }
      return {};
    case OptionTarget::Kind::kLoot:
    case OptionTarget::Kind::kGather:
    case OptionTarget::Kind::kUse:
    case OptionTarget::Kind::kTalk:
    case OptionTarget::Kind::kRest:
    case OptionTarget::Kind::kExplore:
    case OptionTarget::Kind::kGoTo:
    case OptionTarget::Kind::kKeep:
    case OptionTarget::Kind::kInvite:
    case OptionTarget::Kind::kAcceptInvite:
    case OptionTarget::Kind::kDeclineInvite:
    case OptionTarget::Kind::kLeaveGroup:
      break;  // Handled above.
  }
  return {};
}

}  // namespace llm_brain
