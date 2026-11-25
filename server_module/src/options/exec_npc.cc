// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_npc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Group.h"
#include "Item.h"
#include "ItemPackets.h"
#include "ItemTemplate.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "chat/chat_scripts.h"
#include "options/action_types.h"
#include "options/exec_shared.h"
#include "options/options.h"
#include "snapshot/snapshot.h"
#include "snapshot/world_util.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

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

bool IsBoundNearHere(const Player* bot) {
  return bot->m_homebindMapId == bot->GetMapId() &&
         bot->m_homebindAreaId == bot->GetAreaId() &&
         bot->GetExactDist2d(bot->m_homebindX, bot->m_homebindY) <= 50.0f;
}

// Holds starting a `QUEST_FLAGS_PARTY_ACCEPT` escort quest until the party has
// it or every party member is nearby, alive, and past any prerequisite turn-in
// (Now #8).
std::string EscortHoldReason(Player* bot, Creature* npc, const Quest* quest) {
  if (!quest->HasFlag(QUEST_FLAGS_PARTY_ACCEPT)) {
    return "";
  }
  Group* group = bot->GetGroup();
  if (!group) {
    return "";
  }
  const uint32_t qid = quest->GetQuestId();
  const int32_t prev = quest->GetPrevQuestId();
  const uint32_t req_prev = prev > 0 ? static_cast<uint32_t>(prev) : 0;
  std::vector<EscortPartyMember> members;
  for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
    if (slot.guid == bot->GetGUID()) {
      continue;
    }
    EscortPartyMember m;
    Player* p = ObjectAccessor::FindConnectedPlayer(slot.guid);
    if (!p || !p->IsInWorld()) {
      m.online = false;
      members.push_back(m);
      continue;
    }
    m.online = true;
    m.is_human = IsHumanPlayer(p);
    m.alive = p->IsAlive();
    m.same_map = p->GetMapId() == bot->GetMapId() &&
                 p->GetInstanceId() == bot->GetInstanceId();
    m.dist_to_npc_yd = m.same_map ? p->GetDistance(npc) : 9999.0f;
    m.has_quest = p->GetQuestStatus(qid) != QUEST_STATUS_NONE;
    m.missing_prereq = req_prev != 0 && !p->GetQuestRewardStatus(req_prev);
    members.push_back(m);
  }
  return EscortStartHoldReason(group->IsLeader(bot->GetGUID()), members);
}

}  // namespace

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
  return {"", "approached " + npc->GetName()};
}

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

    // 1. Quest turn-ins and quest accepts (reading quest relations directly
    //    without calling PrepareQuestMenu so PlayerTalkClass is never mutated).
    if (npc->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) &&
        bot->CanInteractWithQuestGiver(npc)) {
      std::vector<uint32_t> seen_quests;
      const auto record_seen = [&](uint32_t qid) {
        if (std::find(seen_quests.begin(), seen_quests.end(), qid) !=
            seen_quests.end()) {
          return false;
        }
        seen_quests.push_back(qid);
        return true;
      };
      const QuestRelationBounds involved =
          sObjectMgr->GetCreatureQuestInvolvedRelationBounds(npc->GetEntry());
      for (auto itr = involved.first;
           itr != involved.second && out->size() < kMaxNpcActions; ++itr) {
        const uint32_t qid = itr->second;
        if (!record_seen(qid)) {
          continue;
        }
        const Quest* quest = sObjectMgr->GetQuestTemplate(qid);
        if (!quest) {
          continue;
        }
        if (bot->GetQuestStatus(qid) == QUEST_STATUS_COMPLETE &&
            !bot->GetQuestRewardStatus(qid) &&
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
        }
      }
      const QuestRelationBounds starters =
          sObjectMgr->GetCreatureQuestRelationBounds(npc->GetEntry());
      for (auto itr = starters.first;
           itr != starters.second && out->size() < kMaxNpcActions; ++itr) {
        const uint32_t qid = itr->second;
        if (!record_seen(qid)) {
          continue;
        }
        const Quest* quest = sObjectMgr->GetQuestTemplate(qid);
        if (!quest) {
          continue;
        }
        if (bot->GetQuestStatus(qid) == QUEST_STATUS_NONE &&
            bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            EscortHoldReason(bot, npc, quest).empty()) {
          const char* verb = quest->HasFlag(QUEST_FLAGS_PARTY_ACCEPT)
                                 ? "Accept escort quest \""
                                 : "Accept quest \"";
          out->push_back(
              {"npc_qaccept:" + u.guid + ":" +
                   std::to_string(quest->GetQuestId()),
               u.guid,
               std::string(verb) + quest->GetTitle() + "\" from " + npc_name});
        }
      }
    }

    // 2. Class trainer.
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

    // 4. Vendor upkeep buy (food, water, hunter ammo).
    if (out->size() < kMaxNpcActions && npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR)) {
      if (const VendorItemData* vitems = npc->GetVendorItems()) {
        const float discount = bot->GetReputationPriceDiscount(npc);
        const bool need_drink = !has_drink && bot->getPowerType() == POWER_MANA;
        const bool need_ammo =
            bot->getClass() == CLASS_HUNTER &&
            (bot->GetUInt32Value(PLAYER_AMMO_ID) == 0 ||
             bot->GetItemCount(bot->GetUInt32Value(PLAYER_AMMO_ID), false) <
                 200);
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

    // 5. Flight master travel.
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

    // 6. Innkeeper: bind hearthstone.
    if (out->size() < kMaxNpcActions &&
        npc->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER) && bot->GetMap() &&
        !bot->GetMap()->Instanceable() &&
        bot->GetNPCIfCanInteractWith(npc->GetGUID(), UNIT_NPC_FLAG_INNKEEPER)) {
      if (!IsBoundNearHere(bot)) {
        const std::string area = AreaName(bot->GetAreaId());
        std::string label = "Bind hearthstone at " + npc_name;
        if (!area.empty()) {
          label += " (" + area + ")";
        }
        out->push_back({"npc_bind:" + u.guid, u.guid, std::move(label)});
      }
    }
  }
}

ActionOutcome ExecuteTalkAction(Player* bot, const TalkAction& action) {
  if (bot->IsInCombat()) {
    return {.error = "you are in combat"};
  }
  Creature* npc =
      ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(action.guid));
  if (!npc || !npc->IsInWorld() || !npc->IsAlive()) {
    return {.error = action.name + " is gone"};
  }
  if (IsInTalkRange(bot, npc)) {
    TalkResult res = TalkToNpcNow(bot, npc);
    const bool used = res.error.empty();
    return {.error = std::move(res.error),
            .used = used,
            .info = std::move(res.info)};
  }
  if (!WalkNextTo(bot, npc)) {
    return {.error = "no path to " + action.name};
  }
  return {};
}

ActionOutcome ExecuteNpcAction(Player* bot, const NpcAction& action) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {.error = "not a bot"};
  }
  if (bot->IsInCombat()) {
    return {.error = "you are in combat"};
  }
  Creature* npc =
      ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(action.npc_guid));
  if (!npc || !npc->IsInWorld() || !npc->IsAlive() ||
      !IsInTalkRange(bot, npc)) {
    return {.error = "that NPC is not in talk range"};
  }
  bot->SetFacingToObject(npc);
  bot->SetSelection(npc->GetGUID());

  switch (action.op) {
    case NpcAction::Op::kQuestAccept: {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {.error = "no session"};
      }
      const Quest* quest = sObjectMgr->GetQuestTemplate(action.arg1);
      if (!quest || !bot->CanTakeQuest(quest, false) ||
          !bot->CanAddQuest(quest, false)) {
        return {.error = "cannot accept that quest"};
      }
      if (std::string hold = EscortHoldReason(bot, npc, quest); !hold.empty()) {
        return {.error = std::move(hold)};
      }
      WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
      uint32_t unk1 = 0;
      p << npc->GetGUID() << quest->GetQuestId() << unk1;
      p.rpos(0);
      session->HandleQuestgiverAcceptQuestOpcode(p);
      ai->RecordQuestAccepted(quest->GetQuestId());
      return {.used = true, .info = "accepted " + quest->GetTitle()};
    }
    case NpcAction::Op::kQuestTurnIn: {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {.error = "no session"};
      }
      const Quest* quest = sObjectMgr->GetQuestTemplate(action.arg1);
      const uint32_t choices = quest ? quest->GetRewChoiceItemsCount() : 0;
      const bool valid_idx =
          (choices == 0 && action.arg2 == 0) || action.arg2 < choices;
      if (!quest || !valid_idx ||
          bot->GetQuestStatus(action.arg1) != QUEST_STATUS_COMPLETE ||
          bot->GetQuestRewardStatus(action.arg1) ||
          !bot->CanRewardQuest(quest, false) ||
          !bot->CanRewardQuest(quest, action.arg2, false)) {
        return {.error = "cannot turn in that quest"};
      }
      WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
      p << npc->GetGUID() << quest->GetQuestId() << action.arg2;
      p.rpos(0);
      session->HandleQuestgiverChooseRewardOpcode(p);
      ai->RecordQuestRewarded(quest->GetQuestId());
      return {.used = true, .info = "turned in " + quest->GetTitle()};
    }
    case NpcAction::Op::kTrain: {
      Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
      if (!trainer || trainer->GetTrainerType() != Trainer::Type::Class ||
          !trainer->IsTrainerValidForPlayer(bot)) {
        return {.error = "cannot train your class"};
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
        return {.error = "no affordable spells to learn"};
      }
      return {.used = true,
              .info = "learned " + std::to_string(learned) + " class spell(s)"};
    }
    case NpcAction::Op::kVendor: {
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
        return {.error = "nothing to sell or repair"};
      }
      return {.used = true, .info = base::JoinString(done, ", ")};
    }
    case NpcAction::Op::kBuy: {
      const VendorItemData* vitems = npc->GetVendorItems();
      if (!vitems) {
        return {.error = "vendor has no items"};
      }
      for (uint32_t slot = 0; slot < vitems->GetItemCount(); ++slot) {
        const VendorItem* vi = vitems->GetItem(slot);
        if (vi && vi->item == action.arg1) {
          const ItemTemplate* proto = sObjectMgr->GetItemTemplate(action.arg1);
          if (!bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, action.arg1, 1,
                                          NULL_BAG, NULL_SLOT)) {
            return {.error = "could not buy " +
                             (proto ? proto->Name1 : std::string("item"))};
          }
          return {
              .used = true,
              .info = "bought " + (proto ? proto->Name1 : std::string("item"))};
        }
      }
      return {.error = "item not sold by this vendor"};
    }
    case NpcAction::Op::kFlyDiscover: {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {.error = "no session"};
      }
      if (!session->SendLearnNewTaxiNode(npc)) {
        return {.error = "flight path already known or unavailable"};
      }
      return {.used = true,
              .info = "discovered flight path at " + npc->GetName()};
    }
    case NpcAction::Op::kFlyTo: {
      const uint32_t cur_node =
          sObjectMgr->GetNearestTaxiNode(*npc, bot->GetTeamId(true));
      if (cur_node == 0 ||
          !bot->ActivateTaxiPathTo({cur_node, action.arg1}, npc)) {
        return {.error = "could not start flight"};
      }
      return {.used = true, .info = "took flight path"};
    }
    case NpcAction::Op::kBind: {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {.error = "no session"};
      }
      if (!npc->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER)) {
        return {.error = "that NPC is not an innkeeper"};
      }
      if (!bot->GetMap() || bot->GetMap()->Instanceable()) {
        return {.error = "cannot bind hearthstone inside an instance"};
      }
      if (!bot->GetNPCIfCanInteractWith(npc->GetGUID(),
                                        UNIT_NPC_FLAG_INNKEEPER)) {
        return {.error = "cannot bind hearthstone with " + npc->GetName()};
      }
      WorldPacket p(CMSG_BINDER_ACTIVATE, 8);
      p << npc->GetGUID();
      p.rpos(0);
      session->HandleBinderActivateOpcode(p);
      if (!IsBoundNearHere(bot)) {
        return {.error = "could not bind hearthstone at " + npc->GetName()};
      }
      return {.used = true, .info = "bound hearthstone at " + npc->GetName()};
    }
  }
  return {.error = "unknown NPC action"};
}

}  // namespace llm_brain
