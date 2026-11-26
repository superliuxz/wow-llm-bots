// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/test_commands.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "Bag.h"
#include "Cell.h"
#include "CellImpl.h"  // IWYU pragma: keep
#include "Channel.h"
#include "ChannelMgr.h"
#include "CharmInfo.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "ChatCommandTags.h"
#include "CommandScript.h"
#include "Common.h"
#include "Creature.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"  // IWYU pragma: keep
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceSaveMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFGMgr.h"
#include "LootMgr.h"
#include "Map.h"
#include "MiscPackets.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Optional.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "Value.h"
#include "WorldPacket.h"
#include "bot_manager/bot_manager.h"
#include "chat/chat_scripts.h"

namespace llm_brain {
namespace {

using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;
using Acore::ChatCommands::Tail;

Player* FindOnlinePlayer(ChatHandler* handler, std::string name) {
  Player* player = normalizePlayerName(name)
                       ? ObjectAccessor::FindPlayerByName(name)
                       : nullptr;
  if (player == nullptr || !player->IsInWorld()) {
    handler->SendErrorMessage("{} is not online", name);
    return nullptr;
  }
  return player;
}

class LlmBrainTestCommandScript : public CommandScript {
 public:
  LlmBrainTestCommandScript() : CommandScript("LlmBrainTestCommandScript") {}

  ChatCommandTable GetCommands() const override {
    static ChatCommandTable test_table = {
        {"learn", HandleLearn, SEC_ADMINISTRATOR, Console::Yes},
        {"group", HandleGroup, SEC_ADMINISTRATOR, Console::Yes},
        {"aura", HandleAura, SEC_ADMINISTRATOR, Console::Yes},
        {"unaura", HandleUnaura, SEC_ADMINISTRATOR, Console::Yes},
        {"move", HandleMove, SEC_ADMINISTRATOR, Console::Yes},
        {"tele", HandleTele, SEC_ADMINISTRATOR, Console::Yes},
        {"gear", HandleGear, SEC_ADMINISTRATOR, Console::Yes},
        {"questcomplete", HandleQuestComplete, SEC_ADMINISTRATOR, Console::Yes},
        {"questadd", HandleQuestAdd, SEC_ADMINISTRATOR, Console::Yes},
        {"ungroup", HandleUngroup, SEC_ADMINISTRATOR, Console::Yes},
        {"readycheck", HandleReadyCheck, SEC_ADMINISTRATOR, Console::Yes},
        {"istank", HandleIsTank, SEC_ADMINISTRATOR, Console::Yes},
        {"master", HandleMaster, SEC_ADMINISTRATOR, Console::Yes},
        {"bossemote", HandleBossEmote, SEC_ADMINISTRATOR, Console::Yes},
        {"marknear", HandleMarkNear, SEC_ADMINISTRATOR, Console::Yes},
        {"ping", HandlePing, SEC_ADMINISTRATOR, Console::Yes},
        {"maketank", HandleMakeTank, SEC_ADMINISTRATOR, Console::Yes},
        {"lootroll", HandleLootRoll, SEC_ADMINISTRATOR, Console::Yes},
        {"chat", HandleChat, SEC_ADMINISTRATOR, Console::Yes},
        {"joinchannel", HandleJoinChannel, SEC_ADMINISTRATOR, Console::Yes},
        {"testhuman", HandleTestHuman, SEC_ADMINISTRATOR, Console::Yes},
        {"rezoffer", HandleRezOffer, SEC_ADMINISTRATOR, Console::Yes},
        {"summon", HandleSummon, SEC_ADMINISTRATOR, Console::Yes},
        {"item", HandleItem, SEC_ADMINISTRATOR, Console::Yes},
        {"checkstrat", HandleCheckStrat, SEC_ADMINISTRATOR, Console::Yes},
        {"setstrat", HandleSetStrat, SEC_ADMINISTRATOR, Console::Yes},
        {"clearstrat", HandleClearStrat, SEC_ADMINISTRATOR, Console::Yes},
        {"resetbot", HandleResetBot, SEC_ADMINISTRATOR, Console::Yes},
        {"where", HandleWhere, SEC_ADMINISTRATOR, Console::Yes},
        {"cleartrash", HandleClearTrash, SEC_ADMINISTRATOR, Console::Yes},
        {"difficulty", HandleDifficulty, SEC_ADMINISTRATOR, Console::Yes},
        {"setskill", HandleSetSkill, SEC_ADMINISTRATOR, Console::Yes},
        {"logout", HandleLogout, SEC_ADMINISTRATOR, Console::Yes},
        {"kill", HandleKill, SEC_ADMINISTRATOR, Console::Yes},
        {"lfgboot", HandleLfgBoot, SEC_ADMINISTRATOR, Console::Yes},
        {"spellmiss", HandleSpellMiss, SEC_ADMINISTRATOR, Console::Yes},
    };
    static ChatCommandTable root_table = {
        {"llmtest", test_table},
    };
    return root_table;
  }

 private:
  static std::string FormatToken(const Optional<std::string>& token) {
    if (!token || token->empty()) {
      return "";
    }
    return " tok=" + *token;
  }

  static bool HandleLearn(ChatHandler* handler,
                          std::string player_name,
                          uint32_t spell_id) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    player->learnSpell(spell_id, false);
    handler->PSendSysMessage("llmtest: {} learned spell {}", player->GetName(),
                             spell_id);
    return true;
  }

  static bool HandleGroup(ChatHandler* handler,
                          std::string leader_name,
                          std::string member_name,
                          Optional<std::string> token) {
    Player* leader = FindOnlinePlayer(handler, std::move(leader_name));
    Player* member = FindOnlinePlayer(handler, std::move(member_name));
    if (!leader || !member || leader == member) {
      return false;
    }
    if (Group* old_grp = member->GetGroup();
        old_grp && old_grp != leader->GetGroup()) {
      old_grp->RemoveMember(member->GetGUID());
    }
    Group* group = leader->GetGroup();
    if (!group) {
      group = new Group();
      if (!group->Create(leader)) {
        delete group;
        handler->SendErrorMessage("llmtest: failed to create group");
        return false;
      }
      sGroupMgr->AddGroup(group);
    }
    auto ensure_raid_leader_flags = [group, leader]() {
      if (!group->isRaidGroup()) {
        return;
      }
      uint8_t flags = 0;
      for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
        if (slot.guid == leader->GetGUID()) {
          flags = slot.flags;
          break;
        }
      }
      if ((flags & MEMBER_FLAG_MAINTANK) == 0) {
        group->SetGroupMemberFlag(leader->GetGUID(), true,
                                  MEMBER_FLAG_MAINTANK);
      }
      if ((flags & MEMBER_FLAG_MAINASSIST) == 0) {
        group->SetGroupMemberFlag(leader->GetGUID(), true,
                                  MEMBER_FLAG_MAINASSIST);
      }
    };
    if (member->GetGroup() == group) {
      ensure_raid_leader_flags();
      handler->PSendSysMessage("llmtest: grouped {} (leader) and {}{}",
                               leader->GetName(), member->GetName(),
                               FormatToken(token));
      return true;
    }
    if (!group->isRaidGroup() && group->GetMembersCount() >= MAXGROUPSIZE) {
      group->ConvertToRaid();
    }
    if (!group->AddMember(member)) {
      handler->SendErrorMessage("llmtest: failed to add {} to group",
                                member->GetName());
      return false;
    }
    ensure_raid_leader_flags();
    group->BroadcastGroupUpdate();
    handler->PSendSysMessage("llmtest: grouped {} (leader) and {}{}",
                             leader->GetName(), member->GetName(),
                             FormatToken(token));
    return true;
  }

  static bool HandleAura(ChatHandler* handler,
                         std::string player_name,
                         uint32_t spell_id) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    if (!player->AddAura(spell_id, player)) {
      handler->SendErrorMessage("llmtest: AddAura {} failed on {}", spell_id,
                                player->GetName());
      return false;
    }
    handler->PSendSysMessage("llmtest: added aura {} on {}", spell_id,
                             player->GetName());
    return true;
  }

  static bool HandleUnaura(ChatHandler* handler,
                           std::string player_name,
                           uint32_t spell_id) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    player->RemoveAurasDueToSpell(spell_id);
    handler->PSendSysMessage("llmtest: removed aura {} from {}", spell_id,
                             player->GetName());
    return true;
  }

  static bool HandleMove(ChatHandler* handler,
                         std::string player_name,
                         float x,
                         float y,
                         float z,
                         float o) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    if (!player->IsInCombat()) {
      player->AttackStop();
      player->InterruptNonMeleeSpells(true);
      player->SetSelection(ObjectGuid::Empty);
      if (Pet* pet = player->GetPet()) {
        pet->AttackStop();
        pet->InterruptNonMeleeSpells(true);
      }
    }
    player->NearTeleportTo(x, y, z, o);
    if (Pet* pet = player->GetPet(); pet && pet->IsAlive()) {
      pet->NearTeleportTo(x, y, z, o);
    }
    handler->PSendSysMessage("llmtest: moved {} to ({:.1f}, {:.1f}, {:.1f})",
                             player->GetName(), x, y, z);
    return true;
  }

  static bool HandleTele(ChatHandler* handler,
                         std::string player_name,
                         uint32_t map_id,
                         float x,
                         float y,
                         float z,
                         float o,
                         Optional<uint32_t> gm_mode,
                         Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    // Defaults to TELE_TO_GM_MODE (1) for benchmark staging; pass 0 to enforce
    // normal dungeon entry rules (PlayerCannotEnter / AccountInstancesPerHour).
    const uint32_t flags = (!gm_mode || *gm_mode != 0) ? TELE_TO_GM_MODE : 0;
    if (const MapEntry* map_entry = sMapStore.LookupEntry(map_id);
        map_entry && map_entry->IsDungeon()) {
      // Bots that mounted outdoors on map 0 otherwise arrive inside the
      // dungeon mounted and can trigger CheckMountStateAction fall damage.
      player->Dismount();
      player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }
    if (!player->TeleportTo(map_id, x, y, z, o, flags)) {
      handler->SendErrorMessage("llmtest: TeleportTo map {} failed for {}",
                                map_id, player->GetName());
      return false;
    }
    handler->PSendSysMessage(
        "llmtest: teleported {} to map {} ({:.1f}, {:.1f}, {:.1f}) "
        "instance={}{}",
        player->GetName(), map_id, x, y, z, player->GetInstanceId(),
        FormatToken(token));
    return true;
  }

  static bool HandleGear(ChatHandler* handler,
                         std::string player_name,
                         uint32_t level,
                         Optional<int32_t> spec_tab,
                         Optional<std::string> token,
                         Optional<uint32_t> quality) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    const uint32_t item_quality =
        (quality && *quality <= ITEM_QUALITY_LEGENDARY) ? *quality
                                                        : ITEM_QUALITY_EPIC;
    PlayerbotFactory factory(player, level, item_quality);
    factory.Randomize(false);
    if (spec_tab && *spec_tab >= 0 && *spec_tab <= 3) {
      PlayerbotFactory::InitTalentsBySpecNo(player, *spec_tab, true);
      // Randomize() stored the RANDOM roll's specNo, and InitTalentsBySpecNo
      // can return before storing ours (empty talent row), so store it here.
      sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "specNo",
                                   static_cast<uint32_t>(*spec_tab) + 1);
      factory.InitAvailableSpells();
      factory.InitGlyphs(false);
      factory.InitEquipment(false);
      // Randomize() equipped a random spec before InitEquipment(false) swapped
      // in the requested spec_tab, leaving a full off-spec gear set in bags.
      // Clear unequipped bag gear so freshly geared bots start cleanly.
      for (uint8_t slot = INVENTORY_SLOT_ITEM_START;
           slot < INVENTORY_SLOT_ITEM_END; ++slot) {
        if (Item* it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) {
          if (const ItemTemplate* p = it->GetTemplate();
              p && p->InventoryType != INVTYPE_NON_EQUIP &&
              (p->Class == ITEM_CLASS_ARMOR || p->Class == ITEM_CLASS_WEAPON)) {
            player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
          }
        }
      }
      for (uint8_t bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END;
           ++bag) {
        Bag* b = player->GetBagByPos(bag);
        if (!b) {
          continue;
        }
        for (uint8_t slot = 0; slot < b->GetBagSize(); ++slot) {
          if (Item* it = b->GetItemByPos(slot)) {
            if (const ItemTemplate* p = it->GetTemplate();
                p && p->InventoryType != INVTYPE_NON_EQUIP &&
                (p->Class == ITEM_CLASS_ARMOR ||
                 p->Class == ITEM_CLASS_WEAPON)) {
              player->DestroyItem(bag, slot, true);
            }
          }
        }
      }
      factory.InitAmmo();
      factory.InitReagents();
      factory.InitPet();
      if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player)) {
        ai->ResetStrategies();
      }
    }
    player->SetFullHealth();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    const char* role = PlayerbotAI::IsTank(player)
                           ? "tank"
                           : (PlayerbotAI::IsHeal(player) ? "heal" : "dps");
    handler->PSendSysMessage(
        "llmtest: geared {} at level {} (role={}, hp={}, specNo={}){}",
        player->GetName(), level, role, player->GetMaxHealth(),
        sRandomPlayerbotMgr.GetValue(player->GetGUID().GetCounter(), "specNo"),
        FormatToken(token));
    return true;
  }

  static bool HandleCheckStrat(ChatHandler* handler,
                               std::string player_name,
                               std::string strat_name,
                               Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai || !ai->GetAiObjectContext()) {
      handler->SendErrorMessage("llmtest: {} has no PlayerbotAI",
                                player->GetName());
      return false;
    }
    const auto supported = ai->GetAiObjectContext()->GetSupportedStrategies();
    const bool exists = supported.count(strat_name) != 0;
    const bool in_combat = ai->HasStrategy(strat_name, BOT_STATE_COMBAT);
    const bool non_combat = ai->HasStrategy(strat_name, BOT_STATE_NON_COMBAT);
    handler->PSendSysMessage(
        "llmtest: strat {} on {} exists={} combat={} noncombat={}{}",
        strat_name, player->GetName(), exists ? 1 : 0, in_combat ? 1 : 0,
        non_combat ? 1 : 0, FormatToken(token));
    return exists;
  }

  static bool HandleSetStrat(ChatHandler* handler,
                             std::string player_name,
                             std::string changes,
                             Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai) {
      handler->SendErrorMessage("llmtest: {} has no PlayerbotAI",
                                player->GetName());
      return false;
    }
    BotManager::SetBotStrategyOverride(player->GetName(), changes, changes);
    ai->ChangeStrategy(changes, BOT_STATE_COMBAT);
    ai->ChangeStrategy(changes, BOT_STATE_NON_COMBAT);
    handler->PSendSysMessage("llmtest: setstrat {} {}{}", player->GetName(),
                             changes, FormatToken(token));
    return true;
  }

  // `llmtest clearstrat` drops every per-bot override; `llmtest clearstrat
  // <name>` drops one. The server-wide strategies apply again on the next tick.
  static bool HandleClearStrat(ChatHandler* handler,
                               Optional<std::string> player_name) {
    if (!player_name || player_name->empty()) {
      BotManager::ClearBotStrategyOverrides();
      handler->PSendSysMessage("llmtest: clearstrat all");
      return true;
    }
    std::string name = *player_name;
    if (!normalizePlayerName(name)) {
      handler->SendErrorMessage("llmtest: bad name {}", *player_name);
      return false;
    }
    BotManager::ClearBotStrategyOverride(name);
    handler->PSendSysMessage("llmtest: clearstrat {}", name);
    return true;
  }

  static bool HandleQuestComplete(ChatHandler* handler,
                                  std::string player_name,
                                  uint32_t quest_id,
                                  Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    const Quest* quest = sObjectMgr->GetQuestTemplate(quest_id);
    if (!quest) {
      handler->SendErrorMessage("llmtest: questcomplete: no template for id {}",
                                quest_id);
      return false;
    }
    if (player->FindQuestSlot(quest_id) < MAX_QUEST_LOG_SIZE) {
      player->CompleteQuest(quest_id);
      player->RewardQuest(quest, 0, player, false);
      handler->PSendSysMessage(
          "llmtest: completed and rewarded quest {} on {}{}", quest_id,
          player->GetName(), FormatToken(token));
      return true;
    }
    player->SetRewardedQuest(quest_id);
    handler->PSendSysMessage("llmtest: marked quest {} rewarded on {}{}",
                             quest_id, player->GetName(), FormatToken(token));
    return true;
  }

  // `llmtest chat <name> <party|raid|say|channel:<Name>> <text>`: the player
  // types a chat line, through the client's own CMSG_MESSAGECHAT handler (so
  // the chat hooks run exactly as for a client).
  static bool HandleChat(ChatHandler* handler,
                         std::string player_name,
                         std::string where,
                         Tail text) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player || !player->GetSession()) {
      return false;
    }
    uint32_t type = 0;
    std::string channel;
    if (where == "party") {
      type = CHAT_MSG_PARTY;
    } else if (where == "raid") {
      type = CHAT_MSG_RAID;
    } else if (where == "say") {
      type = CHAT_MSG_SAY;
    } else if (where.starts_with("channel:")) {
      type = CHAT_MSG_CHANNEL;
      channel = where.substr(8);
    } else {
      handler->SendErrorMessage("llmtest: chat: unknown channel {}", where);
      return false;
    }
    WorldPacket packet(CMSG_MESSAGECHAT, 64);
    packet << type << static_cast<uint32_t>(LANG_ORCISH);
    if (type == CHAT_MSG_CHANNEL) {
      packet << channel;
    }
    packet << std::string(text);
    packet.rpos(0);
    player->GetSession()->HandleMessagechatOpcode(packet);
    handler->PSendSysMessage("llmtest: {} typed in {}", player->GetName(),
                             where);
    return true;
  }

  // `llmtest joinchannel <name> <channel>`: the player joins a chat channel
  // (for the LookingForGroup channel the client joins by itself).
  static bool HandleJoinChannel(ChatHandler* handler,
                                std::string player_name,
                                std::string channel_name) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    ChannelMgr* mgr = ChannelMgr::forTeam(player->GetTeamId());
    Channel* channel = nullptr;
    for (uint32_t i = 0; i < sChatChannelsStore.GetNumRows() && !channel; ++i) {
      const ChatChannelsEntry* e = sChatChannelsStore.LookupEntry(i);
      if (e && std::string(e->pattern[0]).starts_with(channel_name)) {
        channel =
            mgr ? mgr->GetJoinChannel(channel_name, e->ChannelID) : nullptr;
      }
    }
    if (!channel) {
      handler->SendErrorMessage("llmtest: joinchannel: no channel {}",
                                channel_name);
      return false;
    }
    channel->JoinChannel(player, "");
    handler->PSendSysMessage("llmtest: {} joined {} ({} members, id {})",
                             player->GetName(), channel->GetName(),
                             channel->GetNumPlayers(), channel->GetChannelId());
    return true;
  }

  // `llmtest testhuman <name> <0|1>`: the chat hooks treat the player's lines
  // as a human's (test builds only), so party-chat rules can be tested with
  // bots standing in for the user.
  static bool HandleTestHuman(ChatHandler* handler,
                              std::string player_name,
                              uint32_t human) {
    if (!normalizePlayerName(player_name)) {
      return false;
    }
    SetTestHuman(player_name, human != 0);
    handler->PSendSysMessage("llmtest: {} {} a human in chat", player_name,
                             human ? "is" : "is not");
    return true;
  }

  // `llmtest istank <name>`: whether playerbots runs the bot as a tank (a
  // tank-type strategy such as `tank assist` is on: PlayerbotAI::IsTank).
  static bool HandleIsTank(ChatHandler* handler, std::string player_name) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player || !GET_PLAYERBOT_AI(player)) {
      handler->SendErrorMessage("llmtest: istank: not a bot");
      return false;
    }
    handler->PSendSysMessage("llmtest: istank {}={}", player->GetName(),
                             PlayerbotAI::IsTank(player) ? 1 : 0);
    return true;
  }

  // `llmtest master <bot>`: the player playerbots treats as the bot's master
  // (T5: a human in the bot's group becomes it).
  static bool HandleMaster(ChatHandler* handler, std::string player_name) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    PlayerbotAI* ai = player ? GET_PLAYERBOT_AI(player) : nullptr;
    if (!ai) {
      handler->SendErrorMessage("llmtest: master: not a bot");
      return false;
    }
    Player* master = ai->GetMaster();
    handler->PSendSysMessage("llmtest: master {}={}", player->GetName(),
                             master ? master->GetName() : "-");
    return true;
  }

  // `llmtest bossemote <near> <text>`: a creature next to `near` sends a raid
  // boss emote, as a boss script's Talk does (T10).
  static bool HandleBossEmote(ChatHandler* handler,
                              std::string near_name,
                              Tail text) {
    Player* near = FindOnlinePlayer(handler, std::move(near_name));
    if (!near) {
      return false;
    }
    // A temporary speaker next to `near` (gone in 30 s).
    constexpr uint32_t kMottledBoar = 3098;
    Creature* speaker = near->SummonCreature(
        kMottledBoar, near->GetPositionX() + 3.0f, near->GetPositionY(),
        near->GetPositionZ(), 0.0f, TEMPSUMMON_TIMED_DESPAWN, 30000);
    if (!speaker) {
      handler->SendErrorMessage("llmtest: bossemote: no creature nearby");
      return false;
    }
    speaker->TextEmote(std::string(text), nullptr, /*isBossEmote=*/true);
    handler->PSendSysMessage("llmtest: {} boss-emoted", speaker->GetName());
    return true;
  }

  // `llmtest ping <member> <dx> <dy>`: `member` pings the minimap `dx` yd
  // north and `dy` yd west of where it stands, as a minimap click sends
  // MSG_MINIMAP_PING (the server shows it to the rest of its group).
  static bool HandlePing(ChatHandler* handler,
                         std::string member_name,
                         float dx,
                         float dy) {
    Player* member = FindOnlinePlayer(handler, std::move(member_name));
    if (!member || !member->GetSession() || !member->GetGroup()) {
      handler->SendErrorMessage("llmtest: ping: needs a grouped player");
      return false;
    }
    WorldPacket request(MSG_MINIMAP_PING, 8);
    request << float(member->GetPositionX() + dx)
            << float(member->GetPositionY() + dy);
    WorldPackets::Misc::MinimapPingClient ping(std::move(request));
    ping.Read();
    member->GetSession()->HandleMinimapPingOpcode(ping);
    handler->PSendSysMessage("llmtest: {} pinged {:.1f} {:.1f}",
                             member->GetName(), ping.MapX, ping.MapY);
    return true;
  }

  // `llmtest marknear <leader> <icon 0-7> [entry] [ahead yd]`: a temporary
  // Mottled Boar (or creature `entry`, kept passive so it starts no fight)
  // 12 yd north of the leader (or `ahead` yd in front of it, short of any wall:
  // indoors), marked with the icon (4 = moon, 7 = skull) by its group.
  static bool HandleMarkNear(ChatHandler* handler,
                             std::string leader_name,
                             uint32_t icon,
                             Optional<uint32_t> entry,
                             Optional<float> ahead) {
    Player* leader = FindOnlinePlayer(handler, std::move(leader_name));
    Group* group = leader ? leader->GetGroup() : nullptr;
    if (!group || icon > 7) {
      handler->SendErrorMessage("llmtest: marknear: needs a grouped leader");
      return false;
    }
    constexpr uint32_t kMottledBoar = 3098;
    Position spot(leader->GetPositionX() + 12.0f, leader->GetPositionY(),
                  leader->GetPositionZ());
    if (ahead) {
      spot = leader->GetFirstCollisionPosition(*ahead, 0.0f);
    }
    Creature* boar = leader->SummonCreature(
        entry.value_or(kMottledBoar), spot.GetPositionX(), spot.GetPositionY(),
        spot.GetPositionZ(), 0.0f, TEMPSUMMON_TIMED_DESPAWN, 120000);
    if (!boar) {
      handler->SendErrorMessage("llmtest: marknear: no creature");
      return false;
    }
    if (entry) {
      boar->SetReactState(REACT_PASSIVE);
    }
    group->SetTargetIcon(static_cast<uint8>(icon), leader->GetGUID(),
                         boar->GetGUID());
    handler->PSendSysMessage("llmtest: marked {} with icon {}", boar->GetName(),
                             icon);
    return true;
  }

  // `llmtest lootroll <leader> <item id>`: kills the nearest living Mottled
  // Boar (entry 3098, next to the test fixtures' Valley of Trials spawn)
  // within 60 yd of the leader, makes `item` its only loot and starts the
  // group's roll on it, as opening the corpse under group loot does.
  static bool HandleLootRoll(ChatHandler* handler,
                             std::string leader_name,
                             uint32_t item_id) {
    Player* leader = FindOnlinePlayer(handler, std::move(leader_name));
    Group* group = leader ? leader->GetGroup() : nullptr;
    if (!group || !sObjectMgr->GetItemTemplate(item_id)) {
      handler->SendErrorMessage(
          "llmtest: lootroll: needs a grouped leader and "
          "a real item");
      return false;
    }
    Creature* victim = nullptr;
    std::list<Creature*> nearby;
    constexpr uint32_t kMottledBoar = 3098;
    leader->GetCreatureListWithEntryInGrid(nearby, kMottledBoar, 60.0f);
    for (Creature* c : nearby) {
      if (c && c->IsAlive() && !c->IsPlayer() && !c->IsTotem() &&
          (!victim || leader->GetDistance(c) < leader->GetDistance(victim))) {
        victim = c;
      }
    }
    if (!victim) {
      // None about: a temporary one next to the leader (gone in 2 min).
      victim = leader->SummonCreature(
          kMottledBoar, leader->GetPositionX() + 3.0f, leader->GetPositionY(),
          leader->GetPositionZ(), 0.0f, TEMPSUMMON_TIMED_DESPAWN, 120000);
    }
    if (!victim) {
      handler->SendErrorMessage("llmtest: lootroll: no creature to loot");
      return false;
    }
    group->SetLootMethod(GROUP_LOOT);
    group->SetLootThreshold(ITEM_QUALITY_UNCOMMON);
    Unit::Kill(leader, victim);
    victim->loot.clear();
    victim->loot.items.emplace_back(
        LootStoreItem(item_id, 0, 100.0f, false, 1, 0, 1, 1));
    victim->loot.unlootedCount = 1;
    group->GroupLoot(&victim->loot, victim);
    handler->PSendSysMessage("llmtest: roll started on {} from {}", item_id,
                             victim->GetName());
    return true;
  }

  // `llmtest maketank <name>`: gives the bot playerbots' `tank assist`
  // strategy as a test override (setstrat can't pass a name with a space).
  static bool HandleMakeTank(ChatHandler* handler, std::string player_name) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    PlayerbotAI* ai = player ? GET_PLAYERBOT_AI(player) : nullptr;
    if (!ai) {
      handler->SendErrorMessage("llmtest: maketank: not a bot");
      return false;
    }
    BotManager::SetBotStrategyOverride(player->GetName(), "+tank assist", "");
    ai->ChangeStrategy("+tank assist", BOT_STATE_COMBAT);
    handler->PSendSysMessage("llmtest: {} tanks", player->GetName());
    return true;
  }

  // `llmtest readycheck <leader>`: the leader starts a ready check, as the
  // client's button does (an empty MSG_RAID_READY_CHECK).
  static bool HandleReadyCheck(ChatHandler* handler, std::string leader_name) {
    Player* leader = FindOnlinePlayer(handler, std::move(leader_name));
    if (!leader || !leader->GetSession() || !leader->GetGroup()) {
      handler->SendErrorMessage("llmtest: readycheck: no grouped leader");
      return false;
    }
    WorldPacket request(MSG_RAID_READY_CHECK, 0);
    leader->GetSession()->HandleRaidReadyCheckOpcode(request);
    handler->PSendSysMessage("llmtest: {} started a ready check",
                             leader->GetName());
    return true;
  }

  // `llmtest rezoffer <dead> <caster>`: `caster` offers `dead` a resurrection
  // at its body, as a finished Resurrection cast does (full health/mana).
  static bool HandleRezOffer(ChatHandler* handler,
                             std::string dead_name,
                             std::string caster_name) {
    Player* dead = FindOnlinePlayer(handler, std::move(dead_name));
    Player* caster = FindOnlinePlayer(handler, std::move(caster_name));
    if (!dead || !caster || dead->IsAlive()) {
      handler->SendErrorMessage("llmtest: rezoffer: needs a dead player");
      return false;
    }
    dead->setResurrectRequestData(caster->GetGUID(), dead->GetMapId(),
                                  dead->GetPositionX(), dead->GetPositionY(),
                                  dead->GetPositionZ(), dead->GetMaxHealth(),
                                  dead->GetMaxPower(POWER_MANA));
    handler->PSendSysMessage("llmtest: {} offered {} a resurrection",
                             caster->GetName(), dead->GetName());
    return true;
  }

  // `llmtest summon <target> <summoner>`: `summoner` summons `target` to where
  // it stands, as a Ritual of Summoning or meeting stone does (the summon
  // point and the SMSG_SUMMON_REQUEST popup of Spell::EffectSummonPlayer).
  static bool HandleSummon(ChatHandler* handler,
                           std::string target_name,
                           std::string summoner_name) {
    Player* target = FindOnlinePlayer(handler, std::move(target_name));
    Player* summoner = FindOnlinePlayer(handler, std::move(summoner_name));
    if (!target || !summoner || !target->IsAlive()) {
      handler->SendErrorMessage("llmtest: summon: needs a living target");
      return false;
    }
    target->SetSummonPoint(summoner->GetMapId(), summoner->GetPositionX(),
                           summoner->GetPositionY(), summoner->GetPositionZ());
    WorldPacket data(SMSG_SUMMON_REQUEST, 8 + 4 + 4);
    data << summoner->GetGUID();
    data << static_cast<uint32_t>(summoner->GetZoneId());
    data << static_cast<uint32_t>(MAX_PLAYER_SUMMON_DELAY * IN_MILLISECONDS);
    target->SendDirectMessage(&data);
    handler->PSendSysMessage("llmtest: {} summoned {}", summoner->GetName(),
                             target->GetName());
    return true;
  }

  // `llmtest ungroup <name>`: takes the player out of its group (e2e cleanup,
  // so a later case starts ungrouped).
  static bool HandleUngroup(ChatHandler* handler, std::string player_name) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    if (Group* group = player->GetGroup()) {
      group->RemoveMember(player->GetGUID());
    }
    handler->PSendSysMessage("llmtest: {} is not in a group",
                             player->GetName());
    return true;
  }

  // `llmtest questadd <name> <quest id>`: puts the quest in the log, as if
  // accepted (e2e_popups shares it).
  static bool HandleQuestAdd(ChatHandler* handler,
                             std::string player_name,
                             uint32_t quest_id) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    const Quest* quest = sObjectMgr->GetQuestTemplate(quest_id);
    if (!quest || !player->CanAddQuest(quest, false)) {
      handler->SendErrorMessage("llmtest: questadd: can't add quest {} to {}",
                                quest_id, player->GetName());
      return false;
    }
    player->AddQuestAndCheckCompletion(quest, nullptr);
    handler->PSendSysMessage("llmtest: added quest {} to {}", quest_id,
                             player->GetName());
    return true;
  }

  static bool HandleItem(ChatHandler* handler,
                         std::string player_name,
                         uint32_t item_id) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    if (!player->AddItem(item_id, 1)) {
      handler->SendErrorMessage("llmtest: AddItem {} failed on {}", item_id,
                                player->GetName());
      return false;
    }
    handler->PSendSysMessage("llmtest: added item {} to {}", item_id,
                             player->GetName());
    return true;
  }

  static bool HandleResetBot(ChatHandler* handler,
                             std::string player_name,
                             Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    // Strip Spirit of Redemption before resurrecting: removing aura 27827 while
    // alive executes `setDeathState(DeathState::JustDied)` in SpellAuraEffects.
    if (player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION)) {
      player->RemoveAurasByType(SPELL_AURA_SPIRIT_OF_REDEMPTION);
    }
    if (!player->IsAlive()) {
      player->ResurrectPlayer(1.0f);
      player->SpawnCorpseBones();
    }
    player->CombatStopWithPets(true);
    // Remove ONLY non-passive auras (clears Sated/Exhaustion, Res Sickness,
    // buffs, debuffs) while preserving passive talent, racial, and item-equip
    // auras (mirroring Unit::RemoveArenaAuras).
    auto& auras = player->GetOwnedAuras();
    for (auto itr = auras.begin(); itr != auras.end();) {
      Aura* aura = itr->second;
      const SpellInfo* info = aura ? aura->GetSpellInfo() : nullptr;
      if (aura && (aura->IsPassive() || (info && info->IsPassive()))) {
        ++itr;
      } else {
        player->RemoveOwnedAura(itr);
      }
    }
    if (!player->IsAlive()) {
      player->ResurrectPlayer(1.0f);
      player->SpawnCorpseBones();
    }
    // Count passives in a separate read-only pass (RemoveOwnedAura resets `itr`
    // in Unit.cpp:4777, which would otherwise double-count passives).
    uint32_t passive_count = 0;
    for (auto const& [spell_id, aura] : player->GetOwnedAuras()) {
      const SpellInfo* info = aura ? aura->GetSpellInfo() : nullptr;
      if (aura && (aura->IsPassive() || (info && info->IsPassive()))) {
        ++passive_count;
      }
    }
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player)) {
      if (AiObjectContext* ctx = ai->GetAiObjectContext()) {
        if (Value<uint32_t>* dc = ctx->GetValue<uint32_t>("death count")) {
          dc->Set(0);
        }
      }
    }
    sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "dead", 0);
    sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "revive", 0);
    player->RemoveAllSpellCooldown();
    player->DurabilityRepairAll(false, 0, false);
    player->SetFullHealth();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    player->SetPower(POWER_RAGE, 0);
    player->SetPower(POWER_ENERGY, player->GetMaxPower(POWER_ENERGY));
    player->SetPower(POWER_RUNIC_POWER, 0);
    Pet* pet = player->GetPet();
    if (!pet && player->getClass() == CLASS_HUNTER) {
      player->SummonPet(0, 0.0f, 0.0f, 0.0f, 0.0f, SUMMON_PET, 0ms, 100);
      pet = player->GetPet();
    }
    if (pet) {
      if (!pet->IsAlive()) {
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        player->GetClosePoint(px, py, pz, pet->GetCombatReach(),
                              PET_FOLLOW_DIST, pet->GetFollowAngle());
        pet->NearTeleportTo(px, py, pz, player->GetOrientation());
        pet->Relocate(px, py, pz, player->GetOrientation());
        pet->ReplaceAllDynamicFlags(UNIT_DYNFLAG_NONE);
        pet->RemoveUnitFlag(UNIT_FLAG_SKINNABLE);
        pet->setDeathState(DeathState::Alive);
        pet->ClearUnitState(
            uint32_t(UNIT_STATE_ALL_STATE & ~(UNIT_STATE_POSSESSED)));
        pet->SetDisplayId(pet->GetNativeDisplayId());
        if (CharmInfo* ci = pet->GetCharmInfo()) {
          ci->SetIsAtStay(false);
          ci->SetIsFollowing(false);
        }
      }
      pet->SetFullHealth();
      const Powers ptype = pet->getPowerType();
      pet->SetPower(ptype, pet->GetMaxPower(ptype));
    }
    PlayerbotFactory factory(player, player->GetLevel(), ITEM_QUALITY_EPIC);
    factory.InitAmmo();
    factory.InitReagents();
    if (Group* group = player->GetGroup()) {
      if (group->GetLeaderGUID() == player->GetGUID()) {
        group->ResetInstances(INSTANCE_RESET_ALL, false, player);
        group->ResetInstances(INSTANCE_RESET_ALL, true, player);
      }
    } else {
      Player::ResetInstances(player->GetGUID(), INSTANCE_RESET_ALL, false);
      Player::ResetInstances(player->GetGUID(), INSTANCE_RESET_ALL, true);
    }
    // `Group::ResetInstances(INSTANCE_RESET_ALL, ...)` skips raids; explicitly
    // unbind all off-map dungeon/raid saves across all difficulties so
    // multi-wave raid benchmarks get a fresh instance on every wave.
    const ObjectGuid guid = player->GetGUID();
    const uint32_t current_map_id = player->GetMapId();
    for (uint8_t diff = 0; diff < MAX_DIFFICULTY; ++diff) {
      const auto difficulty = static_cast<Difficulty>(diff);
      const BoundInstancesMap& bound =
          sInstanceSaveMgr->PlayerGetBoundInstances(guid, difficulty);
      std::vector<uint32_t> to_unbind;
      for (const auto& [map_id, bind] : bound) {
        if (map_id != current_map_id) {
          to_unbind.push_back(map_id);
        }
      }
      for (uint32_t map_id : to_unbind) {
        sInstanceSaveMgr->PlayerUnbindInstance(guid, map_id, difficulty, true,
                                               player);
      }
    }
    handler->PSendSysMessage(
        "llmtest: resetbot {} alive={} passives={} hp={}/{} mana={}/{} "
        "inst={}{}",
        player->GetName(), player->IsAlive() ? 1 : 0, passive_count,
        player->GetHealth(), player->GetMaxHealth(),
        player->GetPower(POWER_MANA), player->GetMaxPower(POWER_MANA),
        player->GetInstanceId(), FormatToken(token));
    return true;
  }

  static bool HandleWhere(ChatHandler* handler,
                          std::string player_name,
                          Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    handler->PSendSysMessage("llmtest: where {} map={} instance={} alive={}{}",
                             player->GetName(), player->GetMapId(),
                             player->GetInstanceId(), player->IsAlive() ? 1 : 0,
                             FormatToken(token));
    return true;
  }

  static bool HandleClearTrash(ChatHandler* handler,
                               std::string player_name,
                               float radius,
                               Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    if (!player->GetMap() || !player->GetMap()->IsDungeon()) {
      handler->SendErrorMessage(
          "llmtest: cleartrash requires a dungeon or raid instance");
      return false;
    }
    if (!(radius > 0.0f && radius <= 300.0f)) {
      handler->SendErrorMessage(
          "llmtest: cleartrash radius must be in (0, 300]");
      return false;
    }
    struct HostileTrashCheck {
      Player* player;
      float range;
      bool operator()(Creature* c) const {
        return c && c->IsInWorld() && c->IsAlive() && !c->isWorldBoss() &&
               !c->IsDungeonBoss() && !c->IsPet() && !c->IsTotem() &&
               !c->IsTrigger() &&
               c->GetInstanceId() == player->GetInstanceId() &&
               player->IsHostileTo(c) && player->IsWithinDistInMap(c, range);
      }
    };
    std::list<Creature*> trash;
    HostileTrashCheck check{player, radius};
    Acore::CreatureListSearcher<HostileTrashCheck> searcher(player, trash,
                                                            check);
    Cell::VisitObjects(player, searcher, radius);
    uint32_t cleared = 0;
    for (Creature* c : trash) {
      if (c && c->IsInWorld()) {
        c->DespawnOrUnsummon();
        ++cleared;
      }
    }
    handler->PSendSysMessage("llmtest: cleartrash {} radius={} cleared={}{}",
                             player->GetName(), radius, cleared,
                             FormatToken(token));
    return true;
  }

  static bool HandleDifficulty(ChatHandler* handler,
                               std::string player_name,
                               std::string mode,
                               Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    std::string mode_lower = mode;
    std::transform(
        mode_lower.begin(), mode_lower.end(), mode_lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    Difficulty diff = DUNGEON_DIFFICULTY_NORMAL;
    if (mode_lower == "normal") {
      diff = DUNGEON_DIFFICULTY_NORMAL;
    } else if (mode_lower == "heroic") {
      diff = DUNGEON_DIFFICULTY_HEROIC;
    } else {
      handler->SendErrorMessage(
          "llmtest: difficulty must be normal or heroic (got {})", mode);
      return false;
    }
    player->SetDungeonDifficulty(diff);
    Group* group = player->GetGroup();
    if (group) {
      group->SetDungeonDifficulty(diff);
    } else {
      player->SendDungeonDifficulty(false);
    }
    handler->PSendSysMessage(
        "llmtest: difficulty {} mode={} group={}{}", player->GetName(),
        diff == DUNGEON_DIFFICULTY_HEROIC ? "heroic" : "normal", group ? 1 : 0,
        FormatToken(token));
    return true;
  }

  static bool HandleSetSkill(ChatHandler* handler,
                             std::string player_name,
                             uint16_t skill_id,
                             uint16_t value,
                             uint16_t max_value,
                             Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    const SkillLineEntry* skill_line = sSkillLineStore.LookupEntry(skill_id);
    if (!skill_line) {
      handler->SendErrorMessage("llmtest: setskill: invalid skill_id {}",
                                skill_id);
      return false;
    }
    if (value == 0 || max_value == 0 || value > max_value) {
      handler->SendErrorMessage(
          "llmtest: setskill: require 0 < value <= max (got value={} max={})",
          value, max_value);
      return false;
    }
    uint16_t step =
        player->HasSkill(skill_id) ? player->GetSkillStep(skill_id) : 0;
    if (const SkillRaceClassInfoEntry* rc_entry = GetSkillRaceClassInfo(
            skill_id, player->getRace(), player->getClass())) {
      if (const SkillTiersEntry* tier =
              sSkillTiersStore.LookupEntry(rc_entry->SkillTierID)) {
        uint16_t computed_step = 0;
        for (uint32_t i = 0; i < MAX_SKILL_STEP; ++i) {
          if (tier->Value[i] == 0) {
            continue;
          }
          computed_step = static_cast<uint16_t>(i + 1);
          if (tier->Value[i] >= max_value) {
            break;
          }
        }
        if (computed_step > 0) {
          step = computed_step;
        }
      }
    }
    player->SetSkill(skill_id, step, value, max_value);
    handler->PSendSysMessage(
        "llmtest: setskill {} skill={} step={} value={} max={}{}",
        player->GetName(), skill_id, step, value, max_value,
        FormatToken(token));
    return true;
  }

  static bool HandleLogout(ChatHandler* handler,
                           std::string player_name,
                           Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    const ObjectGuid guid = player->GetGUID();
    if (!RandomPlayerbotMgr::instance().GetPlayerBot(guid)) {
      handler->SendErrorMessage("{} is not a random playerbot",
                                player->GetName());
      return false;
    }
    const std::string name = player->GetName();
    RandomPlayerbotMgr::instance().LogoutPlayerBot(guid);
    handler->PSendSysMessage("llmtest: logged out {}{}", name,
                             FormatToken(token));
    return true;
  }

  static bool HandleKill(ChatHandler* handler,
                         std::string player_name,
                         Optional<std::string> token) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    if (player->IsAlive()) {
      // A real death (JustDied, then the corpse): it is what arms a
      // soulstone or ankh (PLAYER_SELF_RES_SPELL), as dying to a mob does.
      Unit::Kill(player, player, /*durabilityLoss=*/false);
    }
    handler->PSendSysMessage("llmtest: killed {}{}", player->GetName(),
                             FormatToken(token));
    return true;
  }

  static bool HandleLfgBoot(ChatHandler* handler,
                            std::string kicker_name,
                            std::string victim_name,
                            Optional<std::string> reason) {
    Player* kicker = FindOnlinePlayer(handler, std::move(kicker_name));
    Player* victim = FindOnlinePlayer(handler, std::move(victim_name));
    if (!kicker || !victim) {
      return false;
    }
    Group* group = kicker->GetGroup();
    if (!group || victim->GetGroup() != group) {
      handler->SendErrorMessage("llmtest: lfgboot requires same group");
      return false;
    }
    sLFGMgr->InitBoot(group->GetGUID(), kicker->GetGUID(), victim->GetGUID(),
                      reason ? *reason : "afk");
    handler->PSendSysMessage("llmtest: boot vote started on {} by {}",
                             victim->GetName(), kicker->GetName());
    return true;
  }

  // `llmtest spellmiss <caster> <spell id> <miss info>`: broadcasts a WoW
  // 3.3.5a SMSG_SPELL_GO miss packet from `caster` against the nearest living
  // Mottled Boar (or a temporary one if none is within 60 yd) to every player
  // in visibility range.
  static bool HandleSpellMiss(ChatHandler* handler,
                              std::string caster_name,
                              uint32_t spell_id,
                              uint32_t miss_info) {
    Player* caster = FindOnlinePlayer(handler, std::move(caster_name));
    if (!caster || miss_info > 255) {
      handler->SendErrorMessage("llmtest: spellmiss: bad caster or miss_info");
      return false;
    }
    if (!sSpellMgr->GetSpellInfo(spell_id)) {
      handler->SendErrorMessage("llmtest: spellmiss: unknown spell {}",
                                spell_id);
      return false;
    }
    Creature* victim = nullptr;
    std::list<Creature*> nearby;
    constexpr uint32_t kMottledBoar = 3098;
    caster->GetCreatureListWithEntryInGrid(nearby, kMottledBoar, 60.0f);
    for (Creature* c : nearby) {
      if (c && c->IsAlive() && !c->IsPlayer() && !c->IsTotem() &&
          (!victim || caster->GetDistance(c) < caster->GetDistance(victim))) {
        victim = c;
      }
    }
    if (!victim) {
      victim = caster->SummonCreature(
          kMottledBoar, caster->GetPositionX() + 3.0f, caster->GetPositionY(),
          caster->GetPositionZ(), 0.0f, TEMPSUMMON_TIMED_DESPAWN, 30000);
    }
    if (!victim) {
      handler->SendErrorMessage("llmtest: spellmiss: no creature nearby");
      return false;
    }
    WorldPacket data(SMSG_SPELL_GO, 64);
    data << caster->GetPackGUID();
    data << caster->GetPackGUID();
    data << static_cast<uint8>(0);  // cast_count
    data << static_cast<uint32>(spell_id);
    data << static_cast<uint32>(0);  // cast_flags (CAST_FLAG_NONE)
    data << static_cast<uint32>(getMSTime());
    data << static_cast<uint8>(0);          // hitCount
    data << static_cast<uint8>(1);          // missCount
    data << victim->GetGUID();              // raw uint64 target guid
    data << static_cast<uint8>(miss_info);  // SpellMissInfo
    data << static_cast<uint32>(0);         // SpellCastTargets flags
    caster->SendMessageToSet(&data, true);
    handler->PSendSysMessage("llmtest: {} missed spell {} on {} (info={})",
                             caster->GetName(), spell_id, victim->GetName(),
                             miss_info);
    return true;
  }
};

}  // namespace

void AddLlmBrainTestCommandScripts() {
  new LlmBrainTestCommandScript();
}

}  // namespace llm_brain
