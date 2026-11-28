// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_npc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ConditionMgr.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "GameObject.h"
#include "GossipDef.h"
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
#include "SpellAuraDefines.h"
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
#include "options/leader_travel.h"
#include "options/npc_gossip_and_bags.h"
#include "options/options.h"
#include "options/riding_mounts.h"
#include "snapshot/snapshot.h"
#include "snapshot/world_util.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

static_assert(kGossipOptionGossip == GOSSIP_OPTION_GOSSIP);

bool HasOpenGossipMenuFor(const Player* bot, const Creature* npc) {
  if (!bot || !npc || !bot->PlayerTalkClass) {
    return false;
  }
  const GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
  return menu.GetSenderGUID() == npc->GetGUID() && !menu.Empty();
}

bool HasDbDialogueOptions(Player* bot, Creature* npc) {
  if (!bot || !npc || !npc->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP)) {
    return false;
  }
  const uint32_t menu_id = npc->GetGossipMenuId();
  if (menu_id == 0) {
    return false;
  }
  const uint32_t npc_flags = npc->GetNpcFlags();
  const GossipMenuItemsMapBounds bounds =
      sObjectMgr->GetGossipMenuItemsMapBounds(menu_id);
  for (auto itr = bounds.first; itr != bounds.second; ++itr) {
    const GossipMenuItems& item = itr->second;
    if (item.OptionType != GOSSIP_OPTION_GOSSIP || item.BoxCoded ||
        (item.OptionNpcFlag & npc_flags) == 0) {
      continue;
    }
    if (!sConditionMgr->IsObjectMeetToConditions(bot, npc, item.Conditions)) {
      continue;
    }
    if (!SanitizeGossipText(item.OptionText).empty() ||
        item.OptionBroadcastTextID != 0) {
      return true;
    }
  }
  return false;
}

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

VendorSellCandidate BuildVendorSellCandidate(
    const Player* bot,
    const Item* item,
    uint8_t bag,
    uint8_t slot,
    const std::vector<uint32_t>& active_quest_items) {
  VendorSellCandidate cand;
  cand.bag = bag;
  cand.slot = slot;
  const uint16_t pos = static_cast<uint16_t>((static_cast<uint16_t>(bag) << 8) |
                                             static_cast<uint16_t>(slot));
  cand.is_equipped_or_bag_slot = bot->IsEquipmentPos(pos) || bot->IsBagPos(pos);
  if (!item) {
    return cand;
  }
  cand.is_non_empty_bag = item->IsNotEmptyBag();
  cand.is_refundable = item->IsRefundable();
  const ItemTemplate* proto = item->GetTemplate();
  if (!proto) {
    return cand;
  }
  cand.entry = proto->ItemId;
  cand.name = proto->Name1;
  cand.quality = proto->Quality;
  cand.item_class = proto->Class;
  cand.bonding = proto->Bonding;
  cand.start_quest = proto->StartQuest;
  cand.item_level = proto->ItemLevel;
  cand.sell_price = proto->SellPrice;
  cand.stack_count = item->GetCount();
  cand.required_for_active_quest =
      std::find(active_quest_items.begin(), active_quest_items.end(),
                proto->ItemId) != active_quest_items.end();
  return cand;
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

uint32_t MountSpellTaughtByItem(const ItemTemplate* proto) {
  if (!proto) {
    return 0;
  }
  for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
    if (proto->Spells[i].SpellId > 0 &&
        proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID) {
      return static_cast<uint32_t>(proto->Spells[i].SpellId);
    }
  }
  for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
    if (proto->Spells[i].SpellId <= 0 ||
        proto->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE) {
      continue;
    }
    const SpellInfo* sp = sSpellMgr->GetSpellInfo(
        static_cast<uint32_t>(proto->Spells[i].SpellId));
    if (!sp) {
      continue;
    }
    for (uint8_t eff = 0; eff < MAX_SPELL_EFFECTS; ++eff) {
      if (sp->Effects[eff].Effect == SPELL_EFFECT_LEARN_SPELL &&
          sp->Effects[eff].TriggerSpell > 0) {
        return sp->Effects[eff].TriggerSpell;
      }
    }
  }
  return 0;
}

void MeasureMountSpellSpeed(const SpellInfo* info,
                            uint32_t* ground_speed_pct,
                            uint32_t* flight_speed_pct) {
  *ground_speed_pct = 0;
  *flight_speed_pct = 0;
  if (!info) {
    return;
  }
  for (uint8_t i = 0; i < MAX_SPELL_EFFECTS; ++i) {
    const uint32_t aura = info->Effects[i].ApplyAuraName;
    if (aura == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED) {
      const int32_t val = info->Effects[i].CalcValue();
      if (val > 0 && static_cast<uint32_t>(val) > *ground_speed_pct) {
        *ground_speed_pct = static_cast<uint32_t>(val);
      }
    } else if (aura == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) {
      const int32_t val = info->Effects[i].CalcValue();
      if (val > 0 && static_cast<uint32_t>(val) > *flight_speed_pct) {
        *flight_speed_pct = static_cast<uint32_t>(val);
      }
    }
  }
}

void BestKnownMountSpeeds(Player* bot,
                          uint32_t* best_ground_pct,
                          uint32_t* best_flight_pct) {
  *best_ground_pct = 0;
  *best_flight_pct = 0;
  for (const auto& [spell_id, state] : bot->GetSpellMap()) {
    if (!state || !state->Active || state->State == PLAYERSPELL_REMOVED) {
      continue;
    }
    const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
    if (!info || !info->HasAura(SPELL_AURA_MOUNTED)) {
      continue;
    }
    uint32_t ground = 0;
    uint32_t flight = 0;
    MeasureMountSpellSpeed(info, &ground, &flight);
    if (flight > *best_flight_pct) {
      *best_flight_pct = flight;
    }
    if (flight == 0 && ground > *best_ground_pct) {
      *best_ground_pct = ground;
    }
  }
}

void CollectKnownTaxiGraph(const Player* bot,
                           uint32_t map_id,
                           std::vector<TaxiNodeFact>* nodes,
                           std::vector<TaxiLinkFact>* links) {
  nodes->clear();
  links->clear();
  const uint32_t locale = sWorld->GetDefaultDbcLocale();
  for (uint32_t i = 0; i < sTaxiNodesStore.GetNumRows(); ++i) {
    const TaxiNodesEntry* node = sTaxiNodesStore.LookupEntry(i);
    if (!node || node->map_id != map_id ||
        !bot->m_taxi.IsTaximaskNodeKnown(node->ID)) {
      continue;
    }
    const char* node_name = node->name[locale];
    nodes->push_back(TaxiNodeFact{
        .node_id = node->ID,
        .map_id = node->map_id,
        .pos = {node->x, node->y, node->z},
        .name = (node_name && *node_name) ? node_name : "",
    });
  }
  if (nodes->size() < 2) {
    return;
  }
  for (uint32_t i = 0; i < sTaxiPathStore.GetNumRows(); ++i) {
    const TaxiPathEntry* path = sTaxiPathStore.LookupEntry(i);
    if (!path || !bot->m_taxi.IsTaximaskNodeKnown(path->from) ||
        !bot->m_taxi.IsTaximaskNodeKnown(path->to)) {
      continue;
    }
    links->push_back(TaxiLinkFact{
        .from_node = path->from,
        .to_node = path->to,
        .price_copper = path->price,
    });
  }
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

bool NpcHasDialogue(Player* bot, Creature* npc) {
  if (!bot || !npc || !npc->IsInWorld() || !npc->IsAlive() ||
      !npc->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP)) {
    return false;
  }
  if (HasOpenGossipMenuFor(bot, npc)) {
    return true;
  }
  if (HasDbDialogueOptions(bot, npc)) {
    return true;
  }
  return npc->GetScriptId() != 0;
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
  if (npc->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) && bot->GetSession()) {
    WorldPacket p(CMSG_GOSSIP_HELLO, 8);
    p << npc->GetGUID();
    p.rpos(0);
    bot->GetSession()->HandleGossipHelloOpcode(p);
  }
  ai->ResetActionDuration();
  if (HasOpenGossipMenuFor(bot, npc)) {
    return {"", "talked to " + npc->GetName()};
  }
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
  static constexpr size_t kMaxNpcActions = 24;
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
    const size_t npc_actions_before = out->size();

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

    // 2. Class & riding trainers.
    if (out->size() < kMaxNpcActions &&
        npc->HasNpcFlag(UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS)) {
      if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
          trainer && trainer->IsTrainerValidForPlayer(bot)) {
        const float discount = bot->GetReputationPriceDiscount(npc);
        if (trainer->GetTrainerType() == Trainer::Type::Class) {
          uint32_t affordable = 0;
          for (const Trainer::Spell& sp : trainer->GetSpells()) {
            if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer,
                                                               &sp) ||
                !trainer->CanTeachSpell(bot, &sp)) {
              continue;
            }
            const uint32_t cost = DiscountedCopperCost(sp.MoneyCost, discount);
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
        } else if (trainer->GetTrainerType() == Trainer::Type::Mount) {
          const uint32_t locale = sWorld->GetDefaultDbcLocale();
          std::vector<RidingTrainingCandidate> riding_candidates;
          for (const Trainer::Spell& sp : trainer->GetSpells()) {
            if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer,
                                                               &sp)) {
              continue;
            }
            const SpellInfo* sinfo = sSpellMgr->GetSpellInfo(sp.SpellId);
            const char* sname = sinfo ? sinfo->SpellName[locale] : nullptr;
            riding_candidates.push_back(RidingTrainingCandidate{
                .spell_id = sp.SpellId,
                .name = (sname && *sname) ? std::string(sname) : std::string(),
                .req_level = sp.ReqLevel,
                .cost_copper = DiscountedCopperCost(sp.MoneyCost, discount),
                .can_teach = trainer->CanTeachSpell(bot, &sp),
            });
          }
          for (const RidingTrainingOption& opt : SelectAffordableRidingTraining(
                   riding_candidates, bot->GetMoney(), npc_name)) {
            if (out->size() >= kMaxNpcActions) {
              break;
            }
            out->push_back({"npc_train_riding:" + u.guid + ":" +
                                std::to_string(opt.spell_id),
                            u.guid, opt.label});
          }
        }
      }
    }

    // 3. Vendor sell (bulk grey + per-item non-grey) & repair.
    if (out->size() < kMaxNpcActions &&
        (npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) ||
         npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))) {
      const bool vendor_buys =
          npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) &&
          !npc->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR);
      const bool can_sell_grey = vendor_buys && HasGreySellableItem(bot);
      const bool can_repair =
          npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && HasDamagedEquipment(bot);
      if (can_sell_grey || can_repair) {
        std::string label = can_sell_grey && can_repair
                                ? "Sell grey items & repair at "
                            : can_sell_grey ? "Sell grey items at "
                                            : "Repair gear at ";
        out->push_back({"npc_vendor:" + u.guid, u.guid, label + npc_name});
      }
      if (vendor_buys && out->size() < kMaxNpcActions) {
        const std::vector<uint32_t> quest_items =
            CollectActiveQuestItemIds(bot);
        std::vector<VendorSellCandidate> sell_candidates;
        for (const BagSlotItem& entry : BagItems(bot)) {
          if (!entry.item) {
            continue;
          }
          sell_candidates.push_back(BuildVendorSellCandidate(
              bot, entry.item, entry.bag, entry.slot, quest_items));
        }
        for (const VendorSellOption& opt :
             SelectVendorSellOptions(u.guid, npc_name, sell_candidates)) {
          if (out->size() >= kMaxNpcActions) {
            break;
          }
          out->push_back({opt.action_guid, u.guid, opt.label});
        }
      }
    }

    // 4. Vendor upkeep buy (food, water, hunter ammo, mounts, bags).
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
        std::vector<MountVendorCandidate> mount_candidates;
        std::vector<BagVendorCandidate> bag_candidates;
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
          if (vi->maxcount != 0 &&
              npc->GetVendorItemCurrentCount(vi) < proto->BuyCount) {
            continue;
          }
          const uint32_t price = DiscountedCopperCost(
              proto->BuyPrice > 0 ? static_cast<uint32_t>(proto->BuyPrice) : 0u,
              discount);
          if (!bot->HasEnoughMoney(price)) {
            continue;
          }
          if (proto->Class == ITEM_CLASS_CONTAINER &&
              proto->SubClass == ITEM_SUBCLASS_CONTAINER &&
              proto->InventoryType == INVTYPE_BAG &&
              proto->ContainerSlots > 0) {
            const bool race_ok =
                proto->AllowableRace == 0 ||
                (proto->AllowableRace & bot->getRaceMask()) != 0;
            const bool class_ok =
                proto->AllowableClass == 0 ||
                (proto->AllowableClass & bot->getClassMask()) != 0;
            const bool skill_ok = proto->RequiredSkill == 0 ||
                                  bot->GetSkillValue(proto->RequiredSkill) >=
                                      proto->RequiredSkillRank;
            const bool rep_ok =
                proto->RequiredReputationFaction == 0 ||
                bot->GetReputationRank(proto->RequiredReputationFaction) >=
                    static_cast<ReputationRank>(proto->RequiredReputationRank);
            const bool spell_ok = proto->RequiredSpell == 0 ||
                                  bot->HasSpell(proto->RequiredSpell);
            bag_candidates.push_back(BagVendorCandidate{
                .item_id = proto->ItemId,
                .name = proto->Name1,
                .container_slots = proto->ContainerSlots,
                .price_copper = price,
                .meets_requirements =
                    race_ok && class_ok && skill_ok && rep_ok && spell_ok,
            });
            continue;
          }
          if (proto->Class == ITEM_CLASS_MISC &&
              proto->SubClass == ITEM_SUBCLASS_JUNK_MOUNT) {
            const uint32_t mount_spell_id = MountSpellTaughtByItem(proto);
            const SpellInfo* mspell =
                mount_spell_id ? sSpellMgr->GetSpellInfo(mount_spell_id)
                               : nullptr;
            if (mspell && mspell->HasAura(SPELL_AURA_MOUNTED)) {
              const bool race_ok =
                  proto->AllowableRace == 0 ||
                  (proto->AllowableRace & bot->getRaceMask()) != 0;
              const bool class_ok =
                  proto->AllowableClass == 0 ||
                  (proto->AllowableClass & bot->getClassMask()) != 0;
              const bool skill_ok = proto->RequiredSkill == 0 ||
                                    bot->GetSkillValue(proto->RequiredSkill) >=
                                        proto->RequiredSkillRank;
              const bool rep_ok =
                  proto->RequiredReputationFaction == 0 ||
                  bot->GetReputationRank(proto->RequiredReputationFaction) >=
                      static_cast<ReputationRank>(
                          proto->RequiredReputationRank);
              const bool spell_ok = proto->RequiredSpell == 0 ||
                                    bot->HasSpell(proto->RequiredSpell);
              uint32_t ground_spd = 0;
              uint32_t flight_spd = 0;
              MeasureMountSpellSpeed(mspell, &ground_spd, &flight_spd);
              const bool is_flying = flight_spd > 0;
              mount_candidates.push_back(MountVendorCandidate{
                  .item_id = proto->ItemId,
                  .name = proto->Name1,
                  .price_copper = price,
                  .speed_pct = is_flying ? flight_spd : ground_spd,
                  .is_flying = is_flying,
                  .already_known = bot->HasSpell(mount_spell_id),
                  .in_bags = bot->GetItemCount(proto->ItemId, true) > 0,
                  .meets_requirements =
                      race_ok && class_ok && skill_ok && rep_ok && spell_ok,
              });
            }
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
        if (!mount_candidates.empty() && out->size() < kMaxNpcActions) {
          uint32_t best_ground = 0;
          uint32_t best_flight = 0;
          BestKnownMountSpeeds(bot, &best_ground, &best_flight);
          for (const MountPurchaseOption& pick :
               SelectMountPurchases(mount_candidates, bot->GetMoney(),
                                    best_ground, best_flight, npc_name)) {
            if (out->size() >= kMaxNpcActions) {
              break;
            }
            out->push_back(
                {"npc_buy:" + u.guid + ":" + std::to_string(pick.item_id),
                 u.guid, pick.label});
          }
        }
        if (!bag_candidates.empty() && out->size() < kMaxNpcActions) {
          const uint32_t free_slots =
              snapshot.base.self.indicators.bag_free_slots.value_or(0u);
          for (const BagPurchaseOption& pick :
               SelectBagPurchases(bag_candidates, bot->GetMoney(), free_slots,
                                  ReadEquippedBagSlots(bot), npc_name)) {
            if (out->size() >= kMaxNpcActions) {
              break;
            }
            out->push_back(
                {"npc_buy:" + u.guid + ":" + std::to_string(pick.item_id),
                 u.guid, pick.label});
          }
        }
      }
    }

    // 5. Flight master travel (direct and multi-hop over known nodes).
    if (out->size() < kMaxNpcActions &&
        npc->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER)) {
      const uint32_t cur_node =
          sObjectMgr->GetNearestTaxiNode(*npc, bot->GetTeamId(true));
      if (cur_node != 0 && !bot->m_taxi.IsTaximaskNodeKnown(cur_node)) {
        out->push_back({"npc_fly_discover:" + u.guid, u.guid,
                        "Discover flight path at " + npc_name});
      } else if (cur_node != 0) {
        std::vector<TaxiNodeFact> known_nodes;
        std::vector<TaxiLinkFact> taxi_links;
        CollectKnownTaxiGraph(bot, bot->GetMapId(), &known_nodes, &taxi_links);
        const std::vector<TaxiRouteOption> routes = ComputeReachableTaxiRoutes(
            cur_node, bot->GetMoney(), bot->GetReputationPriceDiscount(npc),
            known_nodes, taxi_links, kMaxFlightDestinations);
        for (const TaxiRouteOption& route : routes) {
          if (out->size() >= kMaxNpcActions) {
            break;
          }
          std::string label = "Fly to " + route.dest_name + " via " + npc_name;
          if (route.nodes.size() > 2) {
            label += " (" + std::to_string(route.nodes.size() - 1) + " hops)";
          }
          out->push_back(
              {"npc_fly:" + u.guid + ":" + std::to_string(route.dest_node),
               u.guid, std::move(label)});
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

    // 7. Gossip / dialogue options (read-only: never mutates PlayerTalkClass).
    if (out->size() < kMaxNpcActions && npc->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP)) {
      if (HasOpenGossipMenuFor(bot, npc)) {
        const GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
        std::vector<GossipOptionCandidate> candidates;
        candidates.reserve(menu.GetMenuItemCount());
        for (const auto& [list_id, item] : menu.GetMenuItems()) {
          candidates.push_back(GossipOptionCandidate{
              .list_id = list_id,
              .sender = item.Sender,
              .option_type = item.OptionType,
              .message = item.Message,
              .box_money = item.BoxMoney,
              .is_coded = item.IsCoded,
          });
        }
        for (const GossipOptionChoice& choice : SelectGossipOptions(
                 menu.GetMenuId(), candidates, bot->GetMoney(), npc_name)) {
          if (out->size() >= kMaxNpcActions) {
            break;
          }
          out->push_back({"npc_gossip:" + u.guid + ":" +
                              std::to_string(choice.menu_id) + ":" +
                              std::to_string(choice.list_id),
                          u.guid, choice.label});
        }
      } else if (out->size() > npc_actions_before && NpcHasDialogue(bot, npc)) {
        // Explicit service/quest actions above hide generic `Talk to <npc>`,
        // so expose an explicit open-dialogue action when this NPC also has
        // dialogue options.
        out->push_back({"npc_gossip_open:" + u.guid, u.guid,
                        "Talk to " + npc_name + " (dialogue)"});
      }
    }
  }

  // 8. Nearby GameObject questgivers (e.g. "Wanted: Hogger", bounty boards,
  // and quest-ending objects within interact range).
  for (const pb::GameObject& obj : snapshot.base.nearby.objects) {
    if (out->size() >= kMaxNpcActions) {
      break;
    }
    if (obj.guid.empty() || !obj.usable ||
        obj.kind != pb::GameObject::QUEST_OBJECT) {
      continue;
    }
    GameObject* go =
        ObjectAccessor::GetGameObject(*bot, ParseSnapshotGuid(obj.guid));
    if (!go || !go->IsInWorld() || !go->isSpawned() ||
        go->GetGoType() != GAMEOBJECT_TYPE_QUESTGIVER ||
        !IsInUseRange(bot, go) || !bot->CanInteractWithQuestGiver(go)) {
      continue;
    }
    const std::string go_name = obj.name.empty() ? go->GetName() : obj.name;
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
        sObjectMgr->GetGOQuestInvolvedRelationBounds(go->GetEntry());
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
            out->push_back({"go_qturnin:" + obj.guid + ":" +
                                std::to_string(quest->GetQuestId()) + ":" +
                                std::to_string(r),
                            obj.guid,
                            "Turn in \"" + quest->GetTitle() + "\" to " +
                                go_name + " (pick " + rdesc + ")"});
          }
        } else {
          out->push_back(
              {"go_qturnin:" + obj.guid + ":" +
                   std::to_string(quest->GetQuestId()) + ":0",
               obj.guid,
               "Turn in \"" + quest->GetTitle() + "\" to " + go_name});
        }
      }
    }
    const QuestRelationBounds starters =
        sObjectMgr->GetGOQuestRelationBounds(go->GetEntry());
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
          bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false)) {
        out->push_back(
            {"go_qaccept:" + obj.guid + ":" +
                 std::to_string(quest->GetQuestId()),
             obj.guid,
             "Accept quest \"" + quest->GetTitle() + "\" from " + go_name});
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
    case NpcAction::Op::kTrainRiding: {
      Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
      const bool valid_trainer =
          trainer != nullptr &&
          trainer->GetTrainerType() == Trainer::Type::Mount &&
          trainer->IsTrainerValidForPlayer(bot);
      const Trainer::Spell* sp =
          valid_trainer ? trainer->GetSpell(action.arg1) : nullptr;
      const bool allowed =
          sp != nullptr &&
          PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer, sp) &&
          trainer->CanTeachSpell(bot, sp);
      const float discount = bot->GetReputationPriceDiscount(npc);
      const uint32_t cost =
          sp ? DiscountedCopperCost(sp->MoneyCost, discount) : 0;
      if (std::string err = ValidateRidingTrain(bot->IsInCombat(),
                                                valid_trainer, sp != nullptr,
                                                allowed, cost, bot->GetMoney());
          !err.empty()) {
        return {.error = std::move(err)};
      }
      const uint32_t money_before = bot->GetMoney();
      trainer->TeachSpell(npc, bot, sp->SpellId);
      if (trainer->CanTeachSpell(bot, sp) &&
          (cost == 0 || bot->GetMoney() == money_before)) {
        return {.error = "could not learn riding skill"};
      }
      const SpellInfo* sinfo = sSpellMgr->GetSpellInfo(sp->SpellId);
      const char* sname =
          sinfo ? sinfo->SpellName[sWorld->GetDefaultDbcLocale()] : nullptr;
      std::string learned_name =
          (sname && *sname) ? std::string(sname)
                            : std::string(RidingSpellName(sp->SpellId));
      if (learned_name.empty()) {
        learned_name = "riding skill";
      }
      return {.used = true, .info = "learned " + learned_name};
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
    case NpcAction::Op::kSellItem: {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {.error = "no session"};
      }
      if (!npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) ||
          npc->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR)) {
        return {.error = "that vendor will not buy items"};
      }
      if (bot->GetTradeData() != nullptr) {
        return {.error = "cannot sell items while trading"};
      }
      const uint8_t bag = static_cast<uint8_t>((action.arg2 >> 8) & 0xFF);
      const uint8_t slot = static_cast<uint8_t>(action.arg2 & 0xFF);
      Item* item = bot->GetItemByPos(bag, slot);
      if (!item || item->GetEntry() != action.arg1) {
        return {.error = "that item is no longer in that bag slot"};
      }
      const std::vector<uint32_t> quest_items = CollectActiveQuestItemIds(bot);
      const VendorSellCandidate cand =
          BuildVendorSellCandidate(bot, item, bag, slot, quest_items);
      if (std::string block = VendorSellSafetyBlockReason(cand);
          !block.empty()) {
        return {.error = std::move(block)};
      }
      const ObjectGuid item_guid = item->GetGUID();
      const std::string item_name = cand.name;
      WorldPacket p(CMSG_SELL_ITEM);
      p << npc->GetGUID() << item_guid << item->GetCount();
      WorldPackets::Item::SellItem packet(std::move(p));
      packet.Read();
      session->HandleSellItemOpcode(packet);
      Item* after = bot->GetItemByPos(bag, slot);
      if (after && after->GetGUID() == item_guid) {
        return {.error = "could not sell " + item_name};
      }
      return {.used = true, .info = "sold " + item_name};
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
          const std::string item_name =
              proto ? proto->Name1 : std::string("item");
          // If buying a general-purpose bag and an empty bag slot exists, buy
          // directly into that bag slot (works even when inventory is full).
          if (proto && proto->Class == ITEM_CLASS_CONTAINER &&
              proto->SubClass == ITEM_SUBCLASS_CONTAINER &&
              proto->InventoryType == INVTYPE_BAG &&
              proto->ContainerSlots > 0) {
            for (const EquippedBagSlotInfo& s : ReadEquippedBagSlots(bot)) {
              if (!s.occupied) {
                bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, action.arg1, 1,
                                           INVENTORY_SLOT_BAG_0, s.slot);
                Item* equipped =
                    bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s.slot);
                if (!equipped || equipped->GetEntry() != action.arg1) {
                  return {.error = "could not buy " + item_name};
                }
                return {.used = true,
                        .info = "bought and equipped " + item_name};
              }
            }
          }
          const uint32_t count_before = bot->GetItemCount(action.arg1, false);
          const uint32_t money_before = bot->GetMoney();
          bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, action.arg1, 1,
                                     NULL_BAG, NULL_SLOT);
          if (bot->GetItemCount(action.arg1, false) <= count_before &&
              bot->GetMoney() >= money_before) {
            return {.error = "could not buy " + item_name};
          }
          return {.used = true, .info = "bought " + item_name};
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
      if (cur_node == 0) {
        return {.error = "could not start flight"};
      }
      std::vector<TaxiNodeFact> known_nodes;
      std::vector<TaxiLinkFact> taxi_links;
      CollectKnownTaxiGraph(bot, bot->GetMapId(), &known_nodes, &taxi_links);
      const std::vector<uint32_t> route = FindTaxiNodePath(
          cur_node, action.arg1, bot->GetMoney(),
          bot->GetReputationPriceDiscount(npc), known_nodes, taxi_links);
      if (route.size() < 2) {
        return {.error = "could not start flight"};
      }
      if (bot->IsMounted()) {
        bot->Dismount();
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
      }
      if (!bot->ActivateTaxiPathTo(route, npc)) {
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
    case NpcAction::Op::kGossipOpen: {
      TalkResult res = TalkToNpcNow(bot, npc);
      return {.error = std::move(res.error),
              .used = res.error.empty(),
              .info = std::move(res.info)};
    }
    case NpcAction::Op::kGossipSelect: {
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {.error = "no session"};
      }
      const GossipMenu* menu = bot->PlayerTalkClass
                                   ? &bot->PlayerTalkClass->GetGossipMenu()
                                   : nullptr;
      const bool menu_matches_npc =
          menu != nullptr && menu->GetSenderGUID() == npc->GetGUID();
      const bool menu_id_matches =
          menu_matches_npc && menu->GetMenuId() == action.arg1;
      const GossipMenuItem* item =
          menu_id_matches ? menu->GetItem(action.arg2) : nullptr;
      if (std::string err = ValidateGossipSelect(
              bot->IsInCombat(), /*npc_in_range=*/true, menu_matches_npc,
              menu_id_matches, item != nullptr, item ? item->IsCoded : false,
              item ? item->BoxMoney : 0u, bot->GetMoney());
          !err.empty()) {
        return {.error = std::move(err)};
      }
      const std::string chosen_text = SanitizeGossipText(item->Message);
      WorldPacket p(CMSG_GOSSIP_SELECT_OPTION, 16);
      p << npc->GetGUID() << action.arg1 << action.arg2;
      p.rpos(0);
      session->HandleGossipSelectOptionOpcode(p);
      ai->ResetActionDuration();
      return {.used = true,
              .info = "selected \"" + chosen_text + "\" at " + npc->GetName()};
    }
  }
  return {.error = "unknown NPC action"};
}

ActionOutcome ExecuteObjectQuestAction(Player* bot,
                                       const ObjectQuestAction& action) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {.error = "not a bot"};
  }
  if (bot->IsInCombat()) {
    return {.error = "you are in combat"};
  }
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  GameObject* go = ObjectAccessor::GetGameObject(
      *bot, ParseSnapshotGuid(action.object_guid));
  if (!go || !go->IsInWorld() || !go->isSpawned() || !IsInUseRange(bot, go) ||
      !bot->CanInteractWithQuestGiver(go)) {
    return {.error = "that object is not in interact range"};
  }
  bot->SetFacingToObject(go);
  const Quest* quest = sObjectMgr->GetQuestTemplate(action.quest_id);
  if (!quest) {
    return {.error = "unknown quest"};
  }
  switch (action.op) {
    case ObjectQuestAction::Op::kAccept: {
      if (!go->hasQuest(action.quest_id) ||
          bot->GetQuestStatus(action.quest_id) != QUEST_STATUS_NONE ||
          !bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false)) {
        return {.error = "cannot accept that quest"};
      }
      WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST, 8 + 4 + 4);
      p << go->GetGUID() << quest->GetQuestId() << uint32_t(0);
      p.rpos(0);
      session->HandleQuestgiverAcceptQuestOpcode(p);
      if (bot->GetQuestStatus(action.quest_id) == QUEST_STATUS_NONE) {
        return {.error = "cannot accept that quest"};
      }
      ai->RecordQuestAccepted(quest->GetQuestId());
      return {.used = true, .info = "accepted " + quest->GetTitle()};
    }
    case ObjectQuestAction::Op::kTurnIn: {
      const uint32_t choices = quest->GetRewChoiceItemsCount();
      const bool valid_idx = (choices == 0 && action.reward_idx == 0) ||
                             action.reward_idx < choices;
      if (!go->hasInvolvedQuest(action.quest_id) || !valid_idx ||
          bot->GetQuestStatus(action.quest_id) != QUEST_STATUS_COMPLETE ||
          bot->GetQuestRewardStatus(action.quest_id) ||
          !bot->CanRewardQuest(quest, false) ||
          !bot->CanRewardQuest(quest, action.reward_idx, false)) {
        return {.error = "cannot turn in that quest"};
      }
      WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD, 8 + 4 + 4);
      p << go->GetGUID() << quest->GetQuestId() << action.reward_idx;
      p.rpos(0);
      session->HandleQuestgiverChooseRewardOpcode(p);
      ai->RecordQuestRewarded(quest->GetQuestId());
      return {.used = true, .info = "turned in " + quest->GetTitle()};
    }
  }
  return {.error = "unknown object quest action"};
}

}  // namespace llm_brain
