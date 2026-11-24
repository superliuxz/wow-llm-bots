// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_social.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "LootMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/strings/stringprintf.h"
#include "bot_manager/bot_manager.h"
#include "options/action_types.h"
#include "options/exec_items.h"
#include "options/exec_shared.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

struct LfgRoleChoice {
  uint8_t mask = lfg::PLAYER_ROLE_DAMAGE;
  const char* name = "DPS";
};

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

std::vector<uint8_t> SlotsFor(Player* bot, uint32_t inventory_type) {
  switch (inventory_type) {
    case INVTYPE_HEAD:
      return {EQUIPMENT_SLOT_HEAD};
    case INVTYPE_NECK:
      return {EQUIPMENT_SLOT_NECK};
    case INVTYPE_SHOULDERS:
      return {EQUIPMENT_SLOT_SHOULDERS};
    case INVTYPE_BODY:
      return {EQUIPMENT_SLOT_BODY};
    case INVTYPE_CHEST:
    case INVTYPE_ROBE:
      return {EQUIPMENT_SLOT_CHEST};
    case INVTYPE_WAIST:
      return {EQUIPMENT_SLOT_WAIST};
    case INVTYPE_LEGS:
      return {EQUIPMENT_SLOT_LEGS};
    case INVTYPE_FEET:
      return {EQUIPMENT_SLOT_FEET};
    case INVTYPE_WRISTS:
      return {EQUIPMENT_SLOT_WRISTS};
    case INVTYPE_HANDS:
      return {EQUIPMENT_SLOT_HANDS};
    case INVTYPE_FINGER:
      return {EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2};
    case INVTYPE_TRINKET:
      return {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2};
    case INVTYPE_CLOAK:
      return {EQUIPMENT_SLOT_BACK};
    case INVTYPE_WEAPON:
      if (bot->CanDualWield()) {
        return {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND};
      }
      return {EQUIPMENT_SLOT_MAINHAND};
    case INVTYPE_2HWEAPON:
    case INVTYPE_WEAPONMAINHAND:
      return {EQUIPMENT_SLOT_MAINHAND};
    case INVTYPE_SHIELD:
    case INVTYPE_WEAPONOFFHAND:
    case INVTYPE_HOLDABLE:
      return {EQUIPMENT_SLOT_OFFHAND};
    case INVTYPE_RANGED:
    case INVTYPE_RANGEDRIGHT:
    case INVTYPE_THROWN:
    case INVTYPE_RELIC:
      return {EQUIPMENT_SLOT_RANGED};
    default:
      return {};
  }
}

std::string RollItemFacts(Player* bot, const ItemTemplate* proto) {
  std::string out = "ilvl " + std::to_string(proto->ItemLevel);
  const std::vector<uint8_t> slots = SlotsFor(bot, proto->InventoryType);
  if (slots.empty()) {
    return out + ", not gear";
  }
  if (bot->CanUseItem(proto) != EQUIP_ERR_OK) {
    return out + ", you can't use it";
  }
  uint32_t worn = UINT32_MAX;
  bool empty_slot = false;
  for (const uint8_t slot : slots) {
    const Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    const ItemTemplate* p = item ? item->GetTemplate() : nullptr;
    if (!p) {
      empty_slot = true;
      break;
    }
    worn = std::min(worn, p->ItemLevel);
  }
  if (empty_slot) {
    return out + ", usable, upgrade (empty slot)";
  }
  if (proto->ItemLevel > worn) {
    return out + ", usable, upgrade over ilvl " + std::to_string(worn);
  }
  return out + ", usable, not an upgrade (you wear ilvl " +
         std::to_string(worn) + ")";
}

}  // namespace

void CollectGroupAndUpkeepActions(Player* bot,
                                  const pb::Snapshot& snapshot,
                                  std::vector<pb::OptionFacts_Action>* out) {
  out->clear();
  if (!bot || !bot->IsInWorld()) {
    return;
  }

  // 1. Need / Greed / Pass loot rolls.
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
          proto ? " (" + RollItemFacts(bot, proto) + ")" : std::string();
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

  // 2. Ready check.
  if (BotManager::HasPendingReadyCheck(bot)) {
    out->push_back({"ready:yes", "Ready (confirm ready check)"});
    out->push_back({"ready:no", "Not ready (decline ready check)"});
  }

  // 3. Shared quest accept / decline.
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

  // 5. Close-range Follow / Regroup with party leader.
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

  // 5a. Share a quest with the party.
  if (bot->IsAlive() && !bot->IsInCombat()) {
    if (Group* group = bot->GetGroup()) {
      size_t shares = 0;
      for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE && shares < 2; ++slot) {
        const uint32_t qid = bot->GetQuestSlotQuestId(slot);
        const Quest* quest = qid ? sObjectMgr->GetQuestTemplate(qid) : nullptr;
        if (!quest || !bot->CanShareQuest(qid)) {
          continue;
        }
        bool someone_lacks_it = false;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
             itr = itr->next()) {
          Player* m = itr->GetSource();
          if (m && m != bot && m->IsInWorld() &&
              m->GetMapId() == bot->GetMapId() &&
              bot->GetDistance(m) <= 40.0f &&
              m->GetQuestStatus(qid) == QUEST_STATUS_NONE &&
              m->CanTakeQuest(quest, false)) {
            someone_lacks_it = true;
            break;
          }
        }
        if (someone_lacks_it) {
          out->push_back(
              {"qpush:" + std::to_string(qid),
               "Share quest \"" + quest->GetTitle() + "\" with the party"});
          ++shares;
        }
      }
    }
  }

  // 5b. Group policies.
  if (bot->IsAlive() && !bot->IsInCombat()) {
    if (Group* group = bot->GetGroup()) {
      const BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
      if (policy.not_tank) {
        out->push_back({"grouppolicy:tank", "Tank for the group again"});
      } else if (PlayerbotAI::IsTank(bot)) {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
             itr = itr->next()) {
          Player* m = itr->GetSource();
          if (m && m != bot && m->IsInWorld() &&
              m->GetMapId() == bot->GetMapId() &&
              PlayerbotAI::IsTank(m,
                                  /*bySpec=*/GET_PLAYERBOT_AI(m) == nullptr)) {
            out->push_back({"grouppolicy:notank",
                            "Let " + m->GetName() + " tank (stop tanking)"});
            break;
          }
        }
      }
      const ObjectGuid leader_guid = group->GetLeaderGUID();
      if (leader_guid && leader_guid != bot->GetGUID()) {
        if (policy.stay) {
          out->push_back(
              {"grouppolicy:follow", "Follow the party leader again"});
        } else {
          out->push_back(
              {"grouppolicy:stay", "Wait here (stop following the leader)"});
        }
      }
    }
  }

  // 6. Raid target mark (unified rule: anyone in party, leader/assistant in
  //    raid).
  if (bot->IsAlive()) {
    if (Group* group = bot->GetGroup();
        group &&
        (!group->isRaidGroup() || group->IsLeader(bot->GetGUID()) ||
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

  // 7. Mount.
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

ActionOutcome ExecuteInviteAction(Player* bot, const InviteAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  std::string player_name = action.player_name;
  Player* other = normalizePlayerName(player_name)
                      ? ObjectAccessor::FindPlayerByName(player_name)
                      : nullptr;
  if (!other || !other->IsInWorld()) {
    return {.error = action.player_name + " is not online"};
  }
  if (other == bot) {
    return {.error = "you can't invite yourself"};
  }
  if (other->GetGroup() != nullptr) {
    return {.error = action.player_name + " is already in a group"};
  }
  if (Group* g = bot->GetGroup();
      g && !g->IsLeader(bot->GetGUID()) && !g->IsAssistant(bot->GetGUID())) {
    return {.error = "only the group leader can invite"};
  }
  WorldPacket pkt(CMSG_GROUP_INVITE, player_name.size() + 5);
  pkt << player_name << uint32(0);
  session->HandleGroupInviteOpcode(pkt);
  return {.used = true, .info = "invited " + action.player_name + " to group"};
}

ActionOutcome ExecuteAcceptInviteAction(Player* bot,
                                        const AcceptInviteAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session || !bot->GetGroupInvite()) {
    return {.error = "no pending group invite"};
  }
  WorldPacket pkt(CMSG_GROUP_ACCEPT, 4);
  pkt << uint32(0);
  session->HandleGroupAcceptOpcode(pkt);
  if (!bot->GetGroup()) {
    return {.error = "couldn't join group"};
  }
  return {.used = true, .info = "joined " + action.from + "'s group"};
}

ActionOutcome ExecuteDeclineInviteAction(Player* bot,
                                         const DeclineInviteAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session || !bot->GetGroupInvite()) {
    return {.error = "no pending group invite"};
  }
  WorldPacket pkt(CMSG_GROUP_DECLINE, 0);
  session->HandleGroupDeclineOpcode(pkt);
  return {.used = true, .info = "declined group invite from " + action.from};
}

ActionOutcome ExecuteLeaveGroupAction(Player* bot) {
  WorldSession* session = bot->GetSession();
  if (!session || !bot->GetGroup()) {
    return {.error = "you are not in a group"};
  }
  WorldPacket pkt(CMSG_GROUP_DISBAND, 0);
  session->HandleGroupDisbandOpcode(pkt);
  return {.used = true, .info = "left the group"};
}

ActionOutcome ExecuteResurrectAction(Player* bot,
                                     const ResurrectAction& action) {
  switch (action.mode) {
    case ResurrectAction::Mode::kAccept:
      if (bot->IsAlive())
        return {.error = "you are already alive"};
      if (!bot->isResurrectRequested()) {
        return {.error = "no resurrection pending"};
      }
      if (bot->HasPreventResurectionAura()) {
        return {.error = "resurrection is currently prevented"};
      }
      bot->ResurectUsingRequestData();
      return {.used = true, .info = "accepted resurrection"};
    case ResurrectAction::Mode::kWait:
      if (bot->IsAlive() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
        return {.error = "you are not lying dead"};
      }
      BotManager::WaitForResurrection(bot, /*wait_ms=*/5 * 60 * 1000);
      return {.used = true, .info = "waiting for a resurrection"};
    case ResurrectAction::Mode::kRelease: {
      if (bot->IsAlive())
        return {.error = "you are already alive"};
      if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
        return {.error = "already released"};
      }
      if (bot->HasPreventResurectionAura()) {
        return {.error = "release spirit is currently prevented"};
      }
      WorldSession* session = bot->GetSession();
      if (!session)
        return {.error = "no session"};
      WorldPacket p(CMSG_REPOP_REQUEST, 1);
      p << uint8_t(0);
      p.rpos(0);
      session->HandleRepopRequestOpcode(p);
      return {.used = true, .info = "released spirit"};
    }
  }
  return {.error = "invalid resurrection action"};
}

ActionOutcome ExecuteLootRollAction(Player* bot, const LootRollAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  if (action.vote > 2) {
    return {.error = "invalid loot roll"};
  }
  WorldPacket p(CMSG_LOOT_ROLL, 8 + 4 + 1);
  p << ParseSnapshotGuid(action.item_guid) << action.slot << action.vote;
  p.rpos(0);
  session->HandleLootRoll(p);
  return {.used = true, .info = action.label};
}

ActionOutcome ExecuteReadyCheckAction(Player* bot,
                                      const ReadyCheckAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  if (!bot->GetGroup()) {
    BotManager::ClearPendingReadyCheck(bot);
    return {.error = "you are not in a group"};
  }
  const uint8_t state = action.ready ? 1 : 0;
  WorldPacket p(MSG_RAID_READY_CHECK, 1);
  p << state;
  p.rpos(0);
  session->HandleRaidReadyCheckOpcode(p);
  BotManager::ClearPendingReadyCheck(bot);
  return {.used = true,
          .info = state ? "confirmed ready check" : "declined ready check"};
}

ActionOutcome ExecuteSharedQuestAction(Player* bot,
                                       const SharedQuestAction& action) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {.error = "not a bot"};
  }
  const uint32_t pending_qid = BotManager::GetPendingSharedQuest(bot);
  if (!action.accept) {
    if (pending_qid == action.quest_id) {
      BotManager::ClearPendingSharedQuest(bot);
      if (Player* sharer =
              ObjectAccessor::FindConnectedPlayer(bot->GetDivider())) {
        sharer->SendPushToPartyResponse(bot, QUEST_PARTY_MSG_DECLINE_QUEST);
      }
      bot->SetDivider(ObjectGuid::Empty);
    }
    return {.used = true, .info = "declined shared quest"};
  }
  WorldSession* session = bot->GetSession();
  const ObjectGuid divider = bot->GetDivider();
  const Quest* quest = sObjectMgr->GetQuestTemplate(action.quest_id);
  if (!session || !quest || !divider || pending_qid != action.quest_id) {
    return {.error = "shared quest is no longer available"};
  }
  BotManager::ClearPendingSharedQuest(bot);
  WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST, 8 + 4 + 4);
  p << divider << action.quest_id << uint32_t(0);
  p.rpos(0);
  session->HandleQuestgiverAcceptQuestOpcode(p);
  if (bot->GetQuestStatus(action.quest_id) == QUEST_STATUS_NONE) {
    bot->SetDivider(ObjectGuid::Empty);
    return {.error = "cannot accept shared quest"};
  }
  ai->RecordQuestAccepted(action.quest_id);
  return {.used = true, .info = "accepted shared quest " + quest->GetTitle()};
}

ActionOutcome ExecuteShareQuestAction(Player* bot,
                                      const ShareQuestAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  if (!bot->CanShareQuest(action.quest_id)) {
    return {.error = "you can't share that quest"};
  }
  WorldPacket raw(CMSG_PUSHQUESTTOPARTY, 4);
  raw << action.quest_id;
  WorldPackets::Quest::PushQuestToParty packet(std::move(raw));
  packet.Read();
  session->HandlePushQuestToParty(packet);
  const Quest* quest = sObjectMgr->GetQuestTemplate(action.quest_id);
  return {.used = true,
          .info = "shared quest " + (quest ? quest->GetTitle()
                                           : std::to_string(action.quest_id))};
}

ActionOutcome ExecuteLfgAction(Player* bot, const LfgAction& action) {
  switch (action.op) {
    case LfgAction::Op::kQueue: {
      if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetGUID())) {
        return {.error = "only the group leader can queue for Dungeon Finder"};
      }
      if (action.value == 0) {
        return {.error = "invalid LFG role"};
      }
      const uint32_t did = FindRandomLfgDungeonId(bot);
      if (did == 0) {
        return {.error = "no random dungeon available for your level"};
      }
      lfg::LfgDungeonSet dungeons = {did};
      sLFGMgr->JoinLfg(bot, static_cast<uint8_t>(action.value), dungeons, "");
      if (sLFGMgr->GetState(bot->GetGUID()) == lfg::LFG_STATE_NONE) {
        return {.error = "could not join Dungeon Finder queue"};
      }
      return {.used = true, .info = "queued for Random Dungeon"};
    }
    case LfgAction::Op::kLeave:
      sLFGMgr->LeaveLfg(bot->GetGUID());
      BotManager::ClearPendingLfgProposal(bot);
      return {.used = true, .info = "left Dungeon Finder queue"};
    case LfgAction::Op::kRole:
      if (action.value == 0) {
        return {.error = "invalid LFG role"};
      }
      if (Group* g = bot->GetGroup()) {
        sLFGMgr->UpdateRoleCheck(g->GetGUID(), bot->GetGUID(),
                                 static_cast<uint8_t>(action.value));
        return {.used = true, .info = "confirmed Dungeon Finder role"};
      }
      return {.error = "you are not in a group"};
    case LfgAction::Op::kTeleIn:
      sLFGMgr->TeleportPlayer(bot, false);
      return {.used = true, .info = "teleported to LFG dungeon"};
    case LfgAction::Op::kTeleOut:
      sLFGMgr->TeleportPlayer(bot, true);
      return {.used = true, .info = "teleported out of LFG dungeon"};
    case LfgAction::Op::kAcceptProposal:
    case LfgAction::Op::kDeclineProposal: {
      const bool accept = action.op == LfgAction::Op::kAcceptProposal;
      sLFGMgr->UpdateProposal(action.value, bot->GetGUID(), accept);
      BotManager::ClearPendingLfgProposal(bot);
      return {.used = true,
              .info = accept ? "accepted Dungeon Finder proposal"
                             : "declined Dungeon Finder proposal"};
    }
  }
  return {.error = "unknown LFG action"};
}

ActionOutcome ExecuteGroupPolicyAction(Player* bot,
                                       const GroupPolicyAction& action) {
  if (!bot->GetGroup()) {
    return {.error = "you are not in a group"};
  }
  BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
  std::string info;
  switch (action.op) {
    case GroupPolicyAction::Op::kNoTank:
      policy.not_tank = true;
      info = "stopped tanking";
      break;
    case GroupPolicyAction::Op::kTank:
      policy.not_tank = false;
      info = "tanking again";
      break;
    case GroupPolicyAction::Op::kStay:
      policy.stay = true;
      if (CanClearBotMovement(bot)) {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
      }
      info = "waiting here";
      break;
    case GroupPolicyAction::Op::kFollow:
      policy.stay = false;
      info = "following the leader again";
      break;
  }
  BotManager::SetGroupPolicy(bot, policy);
  return {.used = true, .info = std::move(info)};
}

ActionOutcome ExecuteFollowLeaderAction(Player* bot,
                                        const FollowLeaderAction& action) {
  {
    BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
    policy.stay = false;
    BotManager::SetGroupPolicy(bot, policy);
  }
  Player* leader = ObjectAccessor::FindConnectedPlayer(
      ParseSnapshotGuid(action.leader_guid));
  if (!leader || !leader->IsInWorld() || !leader->IsAlive() ||
      leader->GetMapId() != bot->GetMapId() ||
      leader->GetInstanceId() != bot->GetInstanceId()) {
    return {.error = "party leader is not nearby"};
  }
  RecordLastMovement(bot, leader->GetPositionX(), leader->GetPositionY(),
                     leader->GetPositionZ());
  bot->GetMotionMaster()->MoveFollow(leader, 4.0f, std::numbers::pi_v<float>);
  return {.used = true, .info = "following " + leader->GetName()};
}

ActionOutcome ExecuteMarkTargetAction(Player* bot,
                                      const MarkTargetAction& action) {
  Unit* u =
      ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(action.target_guid));
  return MarkUnitWithIcon(bot, u, action.icon);
}

ItemActionOutcome ExecuteMountAction(Player* bot, const MountAction& action) {
  if (bot->IsInCombat()) {
    return {.error = "cannot mount in combat"};
  }
  if (bot->isMoving()) {
    bot->StopMoving();
  }
  bot->CastSpell(bot, action.spell_id, false);
  const bool casting = IsSpellCastPending(bot, action.spell_id);
  if (!casting && !bot->IsMounted()) {
    return {.error = "cannot mount now"};
  }
  return {.used = !casting,
          .info = casting ? "" : ("mounted " + action.label),
          .pending_spell_id = casting ? action.spell_id : 0};
}

}  // namespace llm_brain
