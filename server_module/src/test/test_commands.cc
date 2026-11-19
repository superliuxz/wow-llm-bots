// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/test_commands.h"

#include <cstdint>
#include <string>

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "SharedDefines.h"

namespace llm_brain {
namespace {

using Acore::ChatCommands::ChatCommandTable;
using Acore::ChatCommands::Console;

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
    };
    static ChatCommandTable root_table = {
        {"llmtest", test_table},
    };
    return root_table;
  }

 private:
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
                          std::string member_name) {
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
    if (member->GetGroup() != group && !group->AddMember(member)) {
      handler->SendErrorMessage("llmtest: failed to add {} to group",
                                member->GetName());
      return false;
    }
    group->BroadcastGroupUpdate();
    handler->PSendSysMessage("llmtest: grouped {} (leader) and {}",
                             leader->GetName(), member->GetName());
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
    player->NearTeleportTo(x, y, z, o);
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
                         float o) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    if (!player->TeleportTo(map_id, x, y, z, o)) {
      handler->SendErrorMessage("llmtest: TeleportTo map {} failed for {}",
                                map_id, player->GetName());
      return false;
    }
    handler->PSendSysMessage(
        "llmtest: teleported {} to map {} ({:.1f}, {:.1f}, {:.1f})",
        player->GetName(), map_id, x, y, z);
    return true;
  }

  static bool HandleGear(ChatHandler* handler,
                         std::string player_name,
                         uint32_t level) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    PlayerbotFactory factory(player, level, ITEM_QUALITY_EPIC);
    factory.Randomize(false);
    player->SetFullHealth();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    handler->PSendSysMessage("llmtest: geared {} at level {} (hp={})",
                             player->GetName(), level, player->GetMaxHealth());
    return true;
  }
};

}  // namespace

void AddLlmBrainTestCommandScripts() {
  new LlmBrainTestCommandScript();
}

}  // namespace llm_brain
