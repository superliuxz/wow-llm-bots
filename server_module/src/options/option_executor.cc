// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/option_executor.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "Bag.h"
#include "Creature.h"
#include "Define.h"
#include "Event.h"
#include "G3D/Vector3.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "Item.h"
#include "ItemPackets.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "LastMovementValue.h"
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
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "options/options.h"
#include "proto/snapshot.h"

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
constexpr float kWalkArrivalYd = 10.0f;
constexpr float kWalkStallProgressYd = 5.0f;
constexpr base::TimeDelta kWalkStallTimeout = base::Seconds(10);
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

namespace {

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
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
      // Playerbots won't gather with bags more than 80% full
      // (LootObject::IsLootPossible).
      if (auto* bag_space =
              ai->GetAiObjectContext()->GetValue<uint8>("bag space");
          bag_space && bag_space->Get() > 80) {
        return base::StringPrintf("your bags are %d%% full",
                                  static_cast<int>(bag_space->Get()));
      }
    }
    LootObject loot(bot, go->GetGUID());
    if (!loot.IsLootPossible(bot)) {
      return "you lack the skill, tool or key for " + name;
    }
  }
  return "";
}

// Walks to (x, y, z) and tells playerbots it is the bot's current movement,
// so its own idle movement (wander, travel) doesn't replace the walk before it
// arrives; combat movement still does.
void WalkTo(Player* bot, float x, float y, float z) {
  bot->GetMotionMaster()->MovePoint(/*id=*/0, x, y, z);
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

// Walks to the bot's side of `target`, within contact distance.
void WalkNextTo(Player* bot, WorldObject* target) {
  float x = target->GetPositionX();
  float y = target->GetPositionY();
  float z = target->GetPositionZ();
  // The angle is relative to the target's facing.
  target->GetClosePoint(x, y, z, bot->GetObjectSize(), CONTACT_DISTANCE,
                        target->GetAngle(bot) - target->GetOrientation(), bot);
  WalkTo(bot, x, y, z);
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
  const auto sell_if_grey = [&](Item* item) {
    if (!item) {
      return;
    }
    const ItemTemplate* proto = item->GetTemplate();
    if (!proto || proto->Quality != ITEM_QUALITY_POOR ||
        proto->SellPrice == 0) {
      return;
    }
    WorldPacket p(CMSG_SELL_ITEM);
    p << vendor_guid << item->GetGUID() << item->GetCount();
    WorldPackets::Item::SellItem packet(std::move(p));
    packet.Read();
    bot->GetSession()->HandleSellItemOpcode(packet);
    ++sold;
  };
  for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END;
       ++slot) {
    sell_if_grey(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
  }
  for (uint8_t i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i) {
    if (const Bag* bag = bot->GetBagByPos(i)) {
      for (uint32_t slot = 0; slot < bag->GetBagSize(); ++slot) {
        sell_if_grey(bot->GetItemByPos(i, static_cast<uint8_t>(slot)));
      }
    }
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

  // 2. Trainer: learn what the bot can afford, through the server's own
  // trainer code (Trainer::TeachSpell: the checks, the cost, the packets),
  // as a click on "Train" would.
  if (npc->HasNpcFlag(UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS)) {
    if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry())) {
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
          done.push_back("not enough money for new spells");
        } else {
          done.push_back("no new spells to learn");
        }
      }
    }
  }

  // 3. Vendor / repair: sell grey items, repair equipment. Binding the
  // hearthstone at an innkeeper is the brain's choice, not part of Talk
  // (docs/plans/options.md, "Later").
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
  const std::string summary = base::StrCat(
      {"talked to ", npc->GetName(), ": ",
       done.empty() ? "nothing new" : base::JoinString(done, "; ")});
  return {"", summary};
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

bool HasFoodOrDrinkAura(Player* bot) {
  return bot->HasAura(kSpellGraccusFruitcake) ||
         HasCategoryAura(bot, kSpellCategoryFood) ||
         HasCategoryAura(bot, kSpellCategoryDrink);
}

void ContinueRest(Player* bot) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai || bot->IsNonMeleeSpellCast(false) || bot->IsInCombat()) {
    return;
  }
  if (!bot->IsFullHealth() && !HasCategoryAura(bot, kSpellCategoryFood) &&
      !bot->HasAura(kSpellGraccusFruitcake)) {
    ai->DoSpecificAction("eat", Event(), /*silent=*/true);
  }
  if (bot->GetMaxPower(POWER_MANA) > 0 &&
      bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA) &&
      !HasCategoryAura(bot, kSpellCategoryDrink)) {
    ai->DoSpecificAction("drink", Event(), /*silent=*/true);
  }
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
  if (dist_to_dest <= kWalkArrivalYd) {
    stop_walk();
    return {.arrived = true};
  }

  if (bot->GetExactDist(walk->progress_pos.x, walk->progress_pos.y,
                        walk->progress_pos.z) >= kWalkStallProgressYd) {
    walk->progress_pos = {bot->GetPositionX(), bot->GetPositionY(),
                          bot->GetPositionZ()};
    walk->last_progress_at = now;
  } else if (now - walk->last_progress_at >= kWalkStallTimeout) {
    stop_walk();
    return {.error = "stuck"};
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
  WalkTo(bot, next_pt.x, next_pt.y, next_pt.z);
  return {.path_time = path_time};
}

bool IsLootInProgress(Player* bot, ObjectGuid guid) {
  if (bot->GetLootGUID() == guid) {
    return true;
  }
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return false;
  }
  auto* loot_target =
      ai->GetAiObjectContext()->GetValue<LootObject>("loot target");
  return loot_target && loot_target->Get().guid == guid;
}

namespace {

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
  WalkNextTo(bot, go);
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
  WalkNextTo(bot, npc);
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
  bool used_consumable = false;
  if (!bot->IsFullHealth()) {
    used_consumable |= ai->DoSpecificAction("eat", Event(), /*silent=*/true);
  }
  if (bot->GetMaxPower(POWER_MANA) > 0 &&
      bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA)) {
    used_consumable |= ai->DoSpecificAction("drink", Event(), /*silent=*/true);
  }
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
      .progress_pos = {bot->GetPositionX(), bot->GetPositionY(),
                       bot->GetPositionZ()},
      .last_progress_at = now,
      .last_path_at = {},
  };
  const WalkStepResult step = TickLongWalk(bot, now, walk);
  if (!step.error.empty()) {
    return {step.error, /*used=*/false, "", step.path_time};
  }
  return {"", /*used=*/step.arrived, "", step.path_time};
}

}  // namespace

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

  Unit* unit = ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(target.guid));
  if (!unit || !unit->IsInWorld()) {
    return {target.name + " is gone"};
  }
  AiObjectContext* context = ai->GetAiObjectContext();
  // Playerbots returns null for a value name it doesn't know (a renamed one
  // after an update): fail the choice, never dereference it.
  auto* current_target = context->GetValue<Unit*>("current target");
  auto* available_loot = context->GetValue<LootObjectStack*>("available loot");
  auto* loot_target = context->GetValue<LootObject>("loot target");
  if (!current_target || !available_loot || !available_loot->Get() ||
      !loot_target) {
    return {"the body can't do that (playerbots values missing)"};
  }
  switch (target.kind) {
    case OptionTarget::Kind::kAttack:
      // Playerbots' attack action attacks the "current target" and runs its
      // own checks; the combat strategies take over from there.
      {
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
          // Refused: leave the body's target as it was, so nothing acts on
          // the refused one later.
          current_target->Set(old_target);
          return {"can't attack " + target.name + " now"};
        }
        ai->ResetActionDuration();
      }
      return {};
    case OptionTarget::Kind::kLoot:
      if (!unit->isDead()) {
        return {target.name + " isn't dead"};
      }
      {
        // The snapshot that offered it may be seconds old: someone else may
        // have looted it since. Only loot rights are checked here, not
        // IsLootPossible(), which also wants the corpse within 3 yd of the
        // bot's height: true once it has walked there, not up a slope or
        // stairs before it sets off.
        Creature* creature = unit->ToCreature();
        LootObject loot(bot, unit->GetGUID());
        if (!creature || loot.IsEmpty() || !bot->isAllowedToLoot(creature)) {
          return {"nothing to loot on " + target.name};
        }
        // Playerbots' loot strategy doesn't set off while the corpse is more
        // than 3 yd above or below (IsLootPossible): walk there ourselves, and
        // it takes over once level with it.
        if (std::abs(unit->GetPositionZ() - bot->GetPositionZ()) >
            INTERACTION_DISTANCE - 2.0f) {
          WalkNextTo(bot, unit);
        }
        // The loot strategy walks to the "loot target" and opens it; clear any
        // leftover check delay (SitDelay, RpgDelay, LootDelay) so UpdateAI acts
        // on the next world tick instead of sleeping it out.
        available_loot->Get()->Add(unit->GetGUID());
        loot_target->Set(loot);
        ai->ResetActionDuration();
      }
      return {};
    case OptionTarget::Kind::kGather:
    case OptionTarget::Kind::kUse:
    case OptionTarget::Kind::kTalk:
    case OptionTarget::Kind::kRest:
    case OptionTarget::Kind::kExplore:
    case OptionTarget::Kind::kGoTo:
    case OptionTarget::Kind::kKeep:
      break;  // Handled above.
  }
  return {};
}

}  // namespace llm_brain
