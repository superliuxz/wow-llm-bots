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
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "Strategy.h"

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
        {"questcomplete", HandleQuestComplete, SEC_ADMINISTRATOR, Console::Yes},
        {"item", HandleItem, SEC_ADMINISTRATOR, Console::Yes},
        {"checkstrat", HandleCheckStrat, SEC_ADMINISTRATOR, Console::Yes},
        {"resetbot", HandleResetBot, SEC_ADMINISTRATOR, Console::Yes},
        {"where", HandleWhere, SEC_ADMINISTRATOR, Console::Yes},
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
    if (!group->isRaidGroup() && group->GetMembersCount() >= MAXGROUPSIZE) {
      group->ConvertToRaid();
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
    if (!player->TeleportTo(map_id, x, y, z, o, TELE_TO_GM_MODE)) {
      handler->SendErrorMessage("llmtest: TeleportTo map {} failed for {}",
                                map_id, player->GetName());
      return false;
    }
    handler->PSendSysMessage(
        "llmtest: teleported {} to map {} ({:.1f}, {:.1f}, {:.1f}) instance={}",
        player->GetName(), map_id, x, y, z, player->GetInstanceId());
    return true;
  }

  static bool HandleGear(ChatHandler* handler,
                         std::string player_name,
                         uint32_t level,
                         Optional<int32_t> spec_tab) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    PlayerbotFactory factory(player, level, ITEM_QUALITY_EPIC);
    factory.Randomize(false);
    if (spec_tab && *spec_tab >= 0 && *spec_tab <= 3) {
      PlayerbotFactory::InitTalentsBySpecNo(player, *spec_tab, true);
      factory.InitGlyphs(false);
      factory.InitEquipment(false);
      factory.InitAmmo();
      factory.InitReagents();
      if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player)) {
        ai->ResetStrategies();
      }
    }
    player->SetFullHealth();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    const char* role = PlayerbotAI::IsTank(player)
                           ? "tank"
                           : (PlayerbotAI::IsHeal(player) ? "heal" : "dps");
    handler->PSendSysMessage("llmtest: geared {} at level {} (role={}, hp={})",
                             player->GetName(), level, role,
                             player->GetMaxHealth());
    return true;
  }

  static bool HandleCheckStrat(ChatHandler* handler,
                               std::string player_name,
                               std::string strat_name) {
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
    const std::set<std::string> supported =
        ai->GetAiObjectContext()->GetSupportedStrategies();
    const bool exists = supported.count(strat_name) != 0;
    const bool in_combat = ai->HasStrategy(strat_name, BOT_STATE_COMBAT);
    const bool non_combat = ai->HasStrategy(strat_name, BOT_STATE_NON_COMBAT);
    handler->PSendSysMessage(
        "llmtest: strat {} on {} exists={} combat={} noncombat={}", strat_name,
        player->GetName(), exists ? 1 : 0, in_combat ? 1 : 0,
        non_combat ? 1 : 0);
    return exists;
  }

  static bool HandleQuestComplete(ChatHandler* handler,
                                  std::string player_name,
                                  uint32_t quest_id) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    player->SetRewardedQuest(quest_id);
    handler->PSendSysMessage("llmtest: marked quest {} rewarded on {}",
                             quest_id, player->GetName());
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

  static bool HandleResetBot(ChatHandler* handler, std::string player_name) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    if (!player->IsAlive()) {
      player->ResurrectPlayer(1.0f);
      player->SpawnCorpseBones();
    }
    player->CombatStopWithPets(true);
    // Remove ONLY non-passive auras (clears Sated/Exhaustion, Res Sickness,
    // buffs, debuffs) while preserving passive talent, racial, and item-equip
    // auras (mirroring Unit::RemoveArenaAuras).
    uint32_t passive_count = 0;
    auto& auras = player->GetOwnedAuras();
    for (auto itr = auras.begin(); itr != auras.end();) {
      Aura* aura = itr->second;
      const SpellInfo* info = aura ? aura->GetSpellInfo() : nullptr;
      if (aura && (aura->IsPassive() || (info && info->IsPassive()))) {
        ++passive_count;
        ++itr;
      } else {
        player->RemoveOwnedAura(itr);
      }
    }
    player->RemoveAllSpellCooldown();
    player->DurabilityRepairAll(false, 0, false);
    player->SetFullHealth();
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    player->SetPower(POWER_RAGE, 0);
    player->SetPower(POWER_ENERGY, player->GetMaxPower(POWER_ENERGY));
    player->SetPower(POWER_RUNIC_POWER, 0);
    if (Pet* pet = player->GetPet()) {
      if (!pet->IsAlive()) {
        pet->setDeathState(DeathState::Alive);
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
    handler->PSendSysMessage(
        "llmtest: resetbot {} alive={} passives={} hp={}/{} mana={}/{} inst={}",
        player->GetName(), player->IsAlive() ? 1 : 0, passive_count,
        player->GetHealth(), player->GetMaxHealth(),
        player->GetPower(POWER_MANA), player->GetMaxPower(POWER_MANA),
        player->GetInstanceId());
    return true;
  }

  static bool HandleWhere(ChatHandler* handler, std::string player_name) {
    Player* player = FindOnlinePlayer(handler, std::move(player_name));
    if (!player) {
      return false;
    }
    handler->PSendSysMessage(
        "llmtest: where {} map={} instance={} alive={}", player->GetName(),
        player->GetMapId(), player->GetInstanceId(), player->IsAlive() ? 1 : 0);
    return true;
  }
};

}  // namespace

void AddLlmBrainTestCommandScripts() {
  new LlmBrainTestCommandScript();
}

}  // namespace llm_brain
