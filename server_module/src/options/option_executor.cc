// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/option_executor.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "ByteBuffer.h"
#include "Creature.h"
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
#include "LastMovementValue.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "MotionMaster.h"
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
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "Trainer.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "options/act_executor.h"
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
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return true;
  }
  auto* last =
      ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement");
  if (!last) {
    return true;
  }
  const float speed = bot->GetSpeed(MOVE_RUN);
  const float ms =
      speed > 0 ? bot->GetExactDist(x, y, z) / speed * 1000.0f : 1000.0f;
  last->Get().Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), ms + 500.0f,
                  MovementPriority::MOVEMENT_NORMAL);
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

// Picks the quest reward index using playerbots' item usage + stat weight
// calculator (NewRpgBaseAction::BestRewardIndex).
uint32_t BestQuestRewardIndex(Player* bot,
                              PlayerbotAI* ai,
                              const Quest* quest) {
  if (!ai || quest->GetRewChoiceItemsCount() <= 1) {
    return 0;
  }
  AiObjectContext* ctx = ai->GetAiObjectContext();
  ItemUsage best_usage = ITEM_USAGE_NONE;
  for (uint8_t i = 0; i < quest->GetRewChoiceItemsCount(); ++i) {
    auto* val =
        ctx->GetValue<ItemUsage>("item usage", quest->RewardChoiceItemId[i]);
    const ItemUsage usage = val ? val->Get() : ITEM_USAGE_NONE;
    if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE) {
      best_usage = ITEM_USAGE_EQUIP;
    } else if (usage == ITEM_USAGE_BAD_EQUIP &&
               best_usage != ITEM_USAGE_EQUIP) {
      best_usage = usage;
    } else if (usage != ITEM_USAGE_NONE && best_usage == ITEM_USAGE_NONE) {
      best_usage = usage;
    }
  }
  StatsWeightCalculator calc(bot);
  uint32_t best = 0;
  float best_score = 0.0f;
  for (uint8_t i = 0; i < quest->GetRewChoiceItemsCount(); ++i) {
    auto* val =
        ctx->GetValue<ItemUsage>("item usage", quest->RewardChoiceItemId[i]);
    const ItemUsage usage = val ? val->Get() : ITEM_USAGE_NONE;
    if (usage == best_usage || usage == ITEM_USAGE_REPLACE) {
      const uint32_t item_id = quest->RewardChoiceItemId[i];
      const ItemTemplate* proto = sObjectMgr->GetItemTemplate(item_id);
      const uint8_t slot =
          proto ? ai->FindEquipSlot(proto, NULL_SLOT, true) : NULL_SLOT;
      const float score = calc.CalculateItem(item_id, 0, slot);
      if (score > best_score) {
        best_score = score;
        best = i;
      }
    }
  }
  return best;
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

  std::vector<std::string> done;

  // 1. Quest giver: turn in completed quests first, then accept available ones
  // (two passes so a follow-up quest unlocked by a turn-in is also accepted).
  if (npc->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) &&
      bot->CanInteractWithQuestGiver(npc)) {
    for (int pass = 0; pass < 2; ++pass) {
      bot->PrepareQuestMenu(npc->GetGUID());
      const QuestMenu& menu = bot->PlayerTalkClass->GetQuestMenu();
      bool turned_in_any = false;
      for (uint8_t idx = 0; idx < menu.GetMenuItemCount(); ++idx) {
        const QuestMenuItem& item = menu.GetItem(idx);
        const Quest* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest) {
          continue;
        }
        const QuestStatus status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_COMPLETE &&
            !bot->GetQuestRewardStatus(item.QuestId) &&
            bot->CanRewardQuest(quest, false)) {
          const uint32_t reward_idx = BestQuestRewardIndex(bot, ai, quest);
          if (bot->CanRewardQuest(quest, reward_idx, false)) {
            WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
            p << npc->GetGUID() << quest->GetQuestId() << reward_idx;
            p.rpos(0);
            bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
            ai->RecordQuestRewarded(quest->GetQuestId());
            done.push_back("turned in " + quest->GetTitle());
            turned_in_any = true;
          }
        } else if (status == QUEST_STATUS_NONE &&
                   bot->CanTakeQuest(quest, false) &&
                   bot->CanAddQuest(quest, false)) {
          WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
          uint32_t unk1 = 0;
          p << npc->GetGUID() << quest->GetQuestId() << unk1;
          p.rpos(0);
          bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);
          ai->RecordQuestAccepted(quest->GetQuestId());
          done.push_back("accepted " + quest->GetTitle());
        }
      }
      if (!turned_in_any) {
        break;
      }
    }
  }

  std::string trainer_refusal;
  // 2. Trainer: learn what the bot can afford, through the server's own
  // trainer code (Trainer::TeachSpell: the checks, the cost, the packets),
  // as a click on "Train" would.
  if (npc->HasNpcFlag(UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS)) {
    if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
        trainer && trainer->GetTrainerType() != Trainer::Type::Class) {
      // A profession, riding or pet trainer: what to learn there (a new
      // profession is a big choice) is the brain's, and there is no option
      // for it yet: Talk learns nothing (review of f40b0b6, finding 13).
      if (done.empty()) {
        return {
            npc->GetName() + " trains a profession or skill, not your class",
            ""};
      }
    } else if (trainer) {
      if (!trainer->IsTrainerValidForPlayer(bot)) {
        if (done.empty()) {
          return {npc->GetName() + " cannot train your class", ""};
        }
      } else {
        const float discount = bot->GetReputationPriceDiscount(npc);
        std::vector<std::string> learned;
        bool too_expensive = false;
        for (const Trainer::Spell& sp : trainer->GetSpells()) {
          if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer,
                                                             &sp) ||
              !trainer->CanTeachSpell(bot, &sp)) {
            continue;
          }
          const int32_t cost = static_cast<int32_t>(sp.MoneyCost * discount);
          if (!bot->HasEnoughMoney(cost)) {
            too_expensive = true;
            continue;
          }
          const uint32_t money_before = bot->GetMoney();
          trainer->TeachSpell(npc, bot, sp.SpellId);
          if (bot->GetMoney() == money_before && cost > 0) {
            continue;  // Refused.
          }
          if (const SpellInfo* info = sSpellMgr->GetSpellInfo(sp.SpellId)) {
            const char* name = info->SpellName[sWorld->GetDefaultDbcLocale()];
            if (name && *name) {
              learned.push_back(name);
            }
          }
        }
        if (!learned.empty()) {
          done.push_back("learned " + base::JoinString(learned, ", "));
        } else if (too_expensive) {
          trainer_refusal = "not enough money for new spells";
        } else {
          trainer_refusal = "no new spells to learn";
        }
      }
    }
  }

  // 3. Vendor / repair: sell grey items, repair equipment. Non-grey items are
  // left to the brain so BoEs, trade goods, and quest-starting items are never
  // sold automatically. Binding the hearthstone at an innkeeper is also the
  // brain's choice (docs/specs/options.md, "Later").
  if (npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR)) {
    const uint32_t sold = SellGreyItemsTo(bot, npc->GetGUID());
    if (sold > 0) {
      done.push_back(base::StringPrintf("sold %u grey item%s", sold,
                                        sold == 1 ? "" : "s"));
    }
  }
  if (npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR)) {
    // Equipped items and bags; items the bot can't afford stay broken (and
    // cost nothing).
    if (bot->DurabilityRepairAll(true, bot->GetReputationPriceDiscount(npc),
                                 false) > 0) {
      done.push_back("repaired gear");
    }
  }

  ai->ResetActionDuration();
  TalkOutcome out = FormatTalkOutcome(npc->GetName(), done, trainer_refusal);
  return {std::move(out.error), std::move(out.info)};
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
    out->push_back(std::move(item));
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
                            PendingWalk* walk) {
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
    if (dist_to_step > kWalkStepAdvanceYd) {
      return {};
    }
  }
  // Not moving yet (or pushed off the path): ask again, but not every tick.
  if (walk->has_step && now - walk->last_path_at < kWalkRepathEvery) {
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
    ClearLastMovement(bot);
    if (bot->isMoving()) {
      bot->StopMoving();
    }
    return {.error = "stuck", .path_time = path_time};
  }

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
                                PendingWalk* walk) {
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
  const WalkStepResult step = TickLongWalk(bot, now, walk);
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
  const Aura* aura_before = aura_unit->GetAura(item_spell_id);
  const int32_t aura_dur_before = aura_before ? aura_before->GetDuration() : -1;
  const time_t aura_apply_before =
      aura_before ? aura_before->GetApplyTime() : 0;
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
      (aura_before == nullptr || aura_after->GetDuration() > aura_dur_before ||
       aura_after->GetApplyTime() > aura_apply_before);
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

}  // namespace

bool IsSpellCastPending(const Player* bot, uint32_t spell_id) {
  return QueuedSpellCount(bot, spell_id) > 0 ||
         IsPreparingOrChannelingSpell(bot, spell_id);
}

ExecuteResult ExecuteOption(Player* bot,
                            const OptionTarget& target,
                            PendingWalk* walk) {
  if (target.kind == OptionTarget::Kind::kKeep) {
    return {};  // The body carries on with what it was doing.
  }
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {"not a bot"};
  }
  if (!bot->IsAlive()) {
    return {"you are dead"};  // A choice from before the bot died.
  }
  if (target.kind == OptionTarget::Kind::kLoot) {
    return ExecuteLootOption(bot, target);
  }
  if (target.kind == OptionTarget::Kind::kUse &&
      target.guid.starts_with("item:")) {
    return ExecuteUseItemOption(bot, target);
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
    return ExecuteWalkOption(bot, target, walk);
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
  if (target.kind == OptionTarget::Kind::kLeaveGroup) {
    WorldSession* session = bot->GetSession();
    if (!session || !bot->GetGroup()) {
      return {"you are not in a group"};
    }
    WorldPacket pkt(CMSG_GROUP_DISBAND, 0);
    session->HandleGroupDisbandOpcode(pkt);
    return {.used = true, .info = "left the group"};
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
