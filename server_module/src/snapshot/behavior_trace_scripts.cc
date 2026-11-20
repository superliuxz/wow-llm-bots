// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace_scripts.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "AllGameObjectScript.h"
#include "AllItemScript.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "GameObject.h"
#include "Group.h"
#include "GroupScript.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "UnitScript.h"
#include "World.h"
#include "WorldSession.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "snapshot/behavior_trace.h"

namespace llm_brain {
namespace {

// Resolves whether `player` should be traced and returns its role ("bot" or
// "human"), or empty if not traced.
// Checks `IsHeadless()` and `IsRandomBot()` in addition to `GET_PLAYERBOT_AI`
// because login/equip/zone hooks fire inside HandlePlayerLoginFromDB before
// Playerbots attaches the AI component to random bots.
std::string_view ResolveTracedRole(const TraceHookState& state,
                                   const Player* player) {
  if (!player) {
    return {};
  }
  if (std::binary_search(state.bot_names.begin(), state.bot_names.end(),
                         player->GetName())) {
    return "bot";
  }
  Player* mutable_player = const_cast<Player*>(player);
  const WorldSession* session = player->GetSession();
  if (state.observe_humans && session != nullptr && !session->IsHeadless() &&
      GET_PLAYERBOT_AI(mutable_player) == nullptr &&
      !sRandomPlayerbotMgr.IsRandomBot(mutable_player)) {
    return "human";
  }
  return {};
}

void EmitTraceEvent(const TraceHookState* state,
                    const Player* player,
                    std::string_view event_name,
                    std::string detail = {},
                    int64_t value = 0) {
  if (!state || !player) {
    return;
  }
  BehaviorTraceWriter* writer = state->writer.load(std::memory_order_acquire);
  if (!writer || !writer->enabled()) {
    return;
  }
  const std::string_view role = ResolveTracedRole(*state, player);
  if (role.empty()) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  TraceEvent ev;
  ev.time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  ev.monotonic_us = (now - base::TimeTicks()).InMicroseconds();
  ev.role = std::string(role);
  ev.name = player->GetName();
  ev.event = std::string(event_name);
  ev.detail = std::move(detail);
  ev.value = value;
  writer->WriteEvent(ev);
}

std::string SpellDisplayName(const SpellInfo* info) {
  if (!info) {
    return {};
  }
  const char* sname = info->SpellName[sWorld->GetDefaultDbcLocale()];
  if (!sname || sname[0] == '\0') {
    sname = info->SpellName[0];
  }
  return sname ? sname : "";
}

std::string ItemDisplayName(const Item* item) {
  if (!item || !item->GetTemplate()) {
    return {};
  }
  return item->GetTemplate()->Name1;
}

std::string AreaDisplayName(uint32 area_id) {
  const AreaTableEntry* area = sAreaTableStore.LookupEntry(area_id);
  if (!area) {
    return {};
  }
  const char* name = area->area_name[sWorld->GetDefaultDbcLocale()];
  if (!name || name[0] == '\0') {
    name = area->area_name[0];
  }
  return name ? name : "";
}

class LlmBrainTracePlayerScript : public PlayerScript {
 public:
  explicit LlmBrainTracePlayerScript(TraceHookState* state)
      : PlayerScript("LlmBrainTracePlayerScript",
                     {PLAYERHOOK_ON_SPELL_CAST,
                      PLAYERHOOK_ON_LOOT_ITEM,
                      PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
                      PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
                      PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
                      PLAYERHOOK_ON_QUEST_REWARD_ITEM,
                      PLAYERHOOK_ON_QUEST_ABANDON,
                      PLAYERHOOK_ON_CREATURE_KILL_CREDIT,
                      PLAYERHOOK_ON_PVP_KILL,
                      PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
                      PLAYERHOOK_ON_PLAYER_RELEASED_GHOST,
                      PLAYERHOOK_ON_PLAYER_RESURRECT,
                      PLAYERHOOK_ON_LEVEL_CHANGED,
                      PLAYERHOOK_ON_GIVE_EXP,
                      PLAYERHOOK_ON_MONEY_CHANGED,
                      PLAYERHOOK_ON_REPUTATION_CHANGE,
                      PLAYERHOOK_ON_LEARN_SPELL,
                      PLAYERHOOK_ON_PLAYER_LEARN_TALENTS,
                      PLAYERHOOK_ON_UPDATE_SKILL,
                      PLAYERHOOK_ON_EQUIP,
                      PLAYERHOOK_ON_UNEQUIP_ITEM,
                      PLAYERHOOK_ON_AFTER_MOVE_ITEM_FROM_INVENTORY,
                      PLAYERHOOK_ON_CREATE_ITEM,
                      PLAYERHOOK_ON_BEFORE_BUY_ITEM_FROM_VENDOR,
                      PLAYERHOOK_ON_BEFORE_DURABILITY_REPAIR,
                      PLAYERHOOK_ON_AFTER_TAKE_ITEM_FROM_MAIL,
                      PLAYERHOOK_ON_LEARN_TAXI_NODE,
                      PLAYERHOOK_ON_UPDATE_ZONE,
                      PLAYERHOOK_ON_UPDATE_AREA,
                      PLAYERHOOK_ON_MAP_CHANGED,
                      PLAYERHOOK_ON_LOGIN,
                      PLAYERHOOK_ON_LOGOUT,
                      PLAYERHOOK_ON_STORE_NEW_ITEM,
                      PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM,
                      PLAYERHOOK_ON_BEFORE_OPEN_ITEM,
                      PLAYERHOOK_ON_TEXT_EMOTE,
                      PLAYERHOOK_ON_DUEL_REQUEST,
                      PLAYERHOOK_ON_DUEL_END}),
        state_(state) {}

  void OnPlayerLogin(Player* player) override {
    EmitTraceEvent(state_, player, "login", "",
                   player ? static_cast<int64_t>(player->GetLevel()) : 0);
  }

  void OnPlayerLogout(Player* player) override {
    EmitTraceEvent(state_, player, "logout", "",
                   player ? static_cast<int64_t>(player->GetLevel()) : 0);
  }

  void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override {
    EmitTraceEvent(state_, player, "item_gain", ItemDisplayName(item),
                   static_cast<int64_t>(count));
  }

  void OnPlayerGroupRollRewardItem(Player* player,
                                   Item* item,
                                   uint32 count,
                                   RollVote vote_type,
                                   Roll* /*roll*/) override {
    EmitTraceEvent(state_, player, "loot_roll_win", ItemDisplayName(item),
                   static_cast<int64_t>(count * 100 + vote_type));
  }

  bool OnPlayerBeforeOpenItem(Player* player, Item* item) override {
    EmitTraceEvent(state_, player, "item_open", ItemDisplayName(item),
                   item ? static_cast<int64_t>(item->GetEntry()) : 0);
    return true;
  }

  void OnPlayerSpellCast(Player* player,
                         Spell* spell,
                         bool /*skipCheck*/) override {
    if (!player || !spell || !spell->GetSpellInfo()) {
      return;
    }
    const SpellInfo* info = spell->GetSpellInfo();
    EmitTraceEvent(state_, player, "cast_start", SpellDisplayName(info),
                   static_cast<int64_t>(info->Id));
  }

  void OnPlayerLootItem(Player* player,
                        Item* item,
                        uint32 count,
                        ObjectGuid /*lootguid*/) override {
    EmitTraceEvent(state_, player, "loot", ItemDisplayName(item),
                   static_cast<int64_t>(count));
  }

  void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override {
    const int64_t copper = loot ? static_cast<int64_t>(loot->gold) : 0;
    if (copper > 0) {
      EmitTraceEvent(state_, player, "loot", "money", copper);
    }
  }

  void OnPlayerQuestAccept(Player* player, Quest const* quest) override {
    if (!quest) {
      return;
    }
    EmitTraceEvent(state_, player, "quest_accept", quest->GetTitle(),
                   static_cast<int64_t>(quest->GetQuestId()));
  }

  void OnPlayerCompleteQuest(Player* player, Quest const* quest) override {
    if (!quest) {
      return;
    }
    EmitTraceEvent(state_, player, "quest_turnin", quest->GetTitle(),
                   static_cast<int64_t>(quest->GetQuestId()));
  }

  void OnPlayerQuestRewardItem(Player* player,
                               Item* item,
                               uint32 count) override {
    EmitTraceEvent(state_, player, "quest_reward", ItemDisplayName(item),
                   static_cast<int64_t>(count));
  }

  void OnPlayerQuestAbandon(Player* player, uint32 quest_id) override {
    EmitTraceEvent(state_, player, "quest_abandon", "",
                   static_cast<int64_t>(quest_id));
  }

  void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override {
    if (!player || !killed) {
      return;
    }
    EmitTraceEvent(state_, player, "mob_kill", killed->GetName(),
                   static_cast<int64_t>(killed->GetLevel()));
  }

  void OnPlayerPVPKill(Player* killer, Player* killed) override {
    // Ignore environmental / self-inflicted deaths (fall, lava, drowning).
    if (!killer || !killed || killer == killed) {
      return;
    }
    EmitTraceEvent(state_, killer, "pvp_kill", "",
                   static_cast<int64_t>(killed->GetLevel()));
    EmitTraceEvent(state_, killed, "died_pvp", "",
                   static_cast<int64_t>(killer->GetLevel()));
  }

  void OnPlayerKilledByCreature(Creature* killer, Player* killed) override {
    if (!killed) {
      return;
    }
    EmitTraceEvent(state_, killed, "died", killer ? killer->GetName() : "",
                   killer ? static_cast<int64_t>(killer->GetLevel()) : 0);
  }

  void OnPlayerReleasedGhost(Player* player) override {
    EmitTraceEvent(state_, player, "release_spirit", "", 0);
  }

  void OnPlayerResurrect(Player* player,
                         float restore_percent,
                         bool& /*applySickness*/) override {
    EmitTraceEvent(state_, player, "resurrect", "",
                   static_cast<int64_t>(restore_percent));
  }

  void OnPlayerLevelChanged(Player* player, uint8 /*oldlevel*/) override {
    if (!player) {
      return;
    }
    EmitTraceEvent(state_, player, "level_up", "",
                   static_cast<int64_t>(player->GetLevel()));
  }

  void OnPlayerGiveXP(Player* player,
                      uint32& amount,
                      Unit* victim,
                      uint8 /*xpSource*/) override {
    if (!player || amount == 0) {
      return;
    }
    EmitTraceEvent(state_, player, "xp_gain", victim ? victim->GetName() : "",
                   static_cast<int64_t>(amount));
  }

  void OnPlayerMoneyChanged(Player* player, int32& amount) override {
    if (!player || amount == 0) {
      return;
    }
    EmitTraceEvent(state_, player, "money_changed", "",
                   static_cast<int64_t>(amount));
  }

  bool OnPlayerReputationChange(Player* player,
                                uint32 faction_id,
                                int32& standing,
                                bool /*incremental*/) override {
    EmitTraceEvent(state_, player, "rep_change", std::to_string(faction_id),
                   static_cast<int64_t>(standing));
    return true;
  }

  void OnPlayerLearnSpell(Player* player, uint32 spell_id) override {
    const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
    EmitTraceEvent(state_, player, "spell_learn", SpellDisplayName(info),
                   static_cast<int64_t>(spell_id));
  }

  void OnPlayerLearnTalents(Player* player,
                            uint32 talent_id,
                            uint32 talent_rank,
                            uint32 /*spellid*/) override {
    EmitTraceEvent(state_, player, "talent_learn", std::to_string(talent_id),
                   static_cast<int64_t>(talent_rank));
  }

  void OnPlayerUpdateSkill(Player* player,
                           uint32 skill_id,
                           uint32 /*value*/,
                           uint32 /*max*/,
                           uint32 /*step*/,
                           uint32 new_value) override {
    EmitTraceEvent(state_, player, "skill_up", std::to_string(skill_id),
                   static_cast<int64_t>(new_value));
  }

  void OnPlayerEquip(Player* player,
                     Item* it,
                     uint8 /*bag*/,
                     uint8 /*slot*/,
                     bool /*update*/) override {
    EmitTraceEvent(state_, player, "item_equip", ItemDisplayName(it),
                   it ? static_cast<int64_t>(it->GetEntry()) : 0);
  }

  void OnPlayerUnequip(Player* player, Item* it) override {
    EmitTraceEvent(state_, player, "item_unequip", ItemDisplayName(it),
                   it ? static_cast<int64_t>(it->GetEntry()) : 0);
  }

  void OnPlayerAfterMoveItemFromInventory(Player* player,
                                          Item* it,
                                          uint8 /*bag*/,
                                          uint8 /*slot*/,
                                          bool /*update*/) override {
    EmitTraceEvent(state_, player, "item_remove", ItemDisplayName(it),
                   it ? static_cast<int64_t>(it->GetCount()) : 0);
  }

  void OnPlayerCreateItem(Player* player, Item* item, uint32 count) override {
    EmitTraceEvent(state_, player, "item_craft", ItemDisplayName(item),
                   static_cast<int64_t>(count));
  }

  void OnPlayerBeforeBuyItemFromVendor(Player* player,
                                       ObjectGuid /*vendorguid*/,
                                       uint32 /*vendorslot*/,
                                       uint32& item_id,
                                       uint8 count,
                                       uint8 /*bag*/,
                                       uint8 /*slot*/) override {
    const ItemTemplate* proto = sObjectMgr->GetItemTemplate(item_id);
    EmitTraceEvent(state_, player, "vendor_buy", proto ? proto->Name1 : "",
                   static_cast<int64_t>(count));
  }

  void OnPlayerBeforeDurabilityRepair(Player* player,
                                      ObjectGuid /*npcGUID*/,
                                      ObjectGuid /*itemGUID*/,
                                      float& /*discountMod*/,
                                      uint8 /*guildBank*/) override {
    EmitTraceEvent(state_, player, "vendor_repair", "", 0);
  }

  void OnPlayerAfterTakeItemFromMail(Player* player,
                                     Item* item,
                                     uint32 count) override {
    EmitTraceEvent(state_, player, "mail_take_item", ItemDisplayName(item),
                   static_cast<int64_t>(count));
  }

  void OnPlayerLearnTaxiNode(Player const* player, uint32 node_id) override {
    EmitTraceEvent(state_, player, "taxi_learn", "",
                   static_cast<int64_t>(node_id));
  }

  void OnPlayerUpdateZone(Player* player,
                          uint32 new_zone,
                          uint32 /*new_area*/) override {
    EmitTraceEvent(state_, player, "zone_change", AreaDisplayName(new_zone),
                   static_cast<int64_t>(new_zone));
  }

  void OnPlayerUpdateArea(Player* player,
                          uint32 /*old_area*/,
                          uint32 new_area) override {
    EmitTraceEvent(state_, player, "area_change", AreaDisplayName(new_area),
                   static_cast<int64_t>(new_area));
  }

  void OnPlayerMapChanged(Player* player) override {
    if (!player) {
      return;
    }
    EmitTraceEvent(state_, player, "map_change", "",
                   static_cast<int64_t>(player->GetMapId()));
  }

  void OnPlayerTextEmote(Player* player,
                         uint32 text_emote,
                         uint32 /*emoteNum*/,
                         ObjectGuid /*guid*/) override {
    EmitTraceEvent(state_, player, "emote", "",
                   static_cast<int64_t>(text_emote));
  }

  void OnPlayerDuelRequest(Player* /*target*/, Player* challenger) override {
    if (challenger) {
      EmitTraceEvent(state_, challenger, "duel_request", "", 0);
    }
  }

  void OnPlayerDuelEnd(Player* winner,
                       Player* loser,
                       DuelCompleteType /*type*/) override {
    if (winner) {
      EmitTraceEvent(state_, winner, "duel_win", "", 0);
    }
    if (loser) {
      EmitTraceEvent(state_, loser, "duel_lose", "", 0);
    }
  }

 private:
  const base::raw_ptr<TraceHookState> state_;
};

class LlmBrainTraceUnitScript : public UnitScript {
 public:
  explicit LlmBrainTraceUnitScript(TraceHookState* state)
      : UnitScript("LlmBrainTraceUnitScript",
                   true,
                   {UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL}),
        state_(state) {}

  void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override {
    if (damage == 0) {
      return;
    }
    if (victim) {
      if (Player* vp = victim->ToPlayer()) {
        // Don't leak another player's name; log NPC names or "environment".
        std::string src;
        if (!attacker || attacker == victim) {
          src = "environment";
        } else if (!attacker->IsPlayer()) {
          src = attacker->GetName();
        }
        EmitTraceEvent(state_, vp, "damage_taken", std::move(src),
                       static_cast<int64_t>(damage));
      }
    }
    // Credit pet/totem/guardian damage to the owning player, and never count
    // self/environmental damage (e.g. fall/lava) as damage dealt.
    if (attacker && victim && attacker != victim) {
      if (Player* ap = attacker->GetCharmerOrOwnerPlayerOrPlayerItself()) {
        if (ap != victim) {
          std::string tgt = victim->IsPlayer() ? "" : victim->GetName();
          EmitTraceEvent(state_, ap, "damage_dealt", std::move(tgt),
                         static_cast<int64_t>(damage));
        }
      }
    }
  }

  void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override {
    if (gain == 0 || !healer) {
      return;
    }
    // Credit healing totems/guardians to the owning player; omit ally names.
    if (Player* hp = healer->GetCharmerOrOwnerPlayerOrPlayerItself()) {
      EmitTraceEvent(state_, hp, "heal_done", "", static_cast<int64_t>(gain));
    }
  }

 private:
  const base::raw_ptr<TraceHookState> state_;
};

class LlmBrainTraceGroupScript : public GroupScript {
 public:
  explicit LlmBrainTraceGroupScript(TraceHookState* state)
      : GroupScript("LlmBrainTraceGroupScript",
                    {GROUPHOOK_ON_ADD_MEMBER, GROUPHOOK_ON_INVITE_MEMBER,
                     GROUPHOOK_ON_REMOVE_MEMBER}),
        state_(state) {}

  void OnAddMember(Group* group, ObjectGuid guid) override {
    if (Player* p = ObjectAccessor::FindPlayer(guid)) {
      EmitTraceEvent(
          state_, p, "group_join", "",
          group ? static_cast<int64_t>(group->GetMembersCount()) : 0);
    }
  }

  void OnInviteMember(Group* /*group*/, ObjectGuid guid) override {
    if (Player* p = ObjectAccessor::FindPlayer(guid)) {
      EmitTraceEvent(state_, p, "group_invited", "", 0);
    }
  }

  void OnRemoveMember(Group* /*group*/,
                      ObjectGuid guid,
                      RemoveMethod method,
                      ObjectGuid /*kicker*/,
                      char const* /*reason*/) override {
    if (Player* p = ObjectAccessor::FindPlayer(guid)) {
      EmitTraceEvent(state_, p, "group_leave", "",
                     static_cast<int64_t>(method));
    }
  }

 private:
  const base::raw_ptr<TraceHookState> state_;
};

class LlmBrainTraceGameObjectScript : public AllGameObjectScript {
 public:
  explicit LlmBrainTraceGameObjectScript(TraceHookState* state)
      : AllGameObjectScript("LlmBrainTraceGameObjectScript"), state_(state) {}

  void OnGameObjectLootStateChanged(GameObject* go,
                                    uint32 state,
                                    Unit* unit) override {
    if (!go || !unit) {
      return;
    }
    if (Player* p = unit->ToPlayer()) {
      EmitTraceEvent(state_, p, "go_interact", go->GetName(),
                     static_cast<int64_t>(state));
    }
  }

 private:
  const base::raw_ptr<TraceHookState> state_;
};

class LlmBrainTraceItemScript : public AllItemScript {
 public:
  explicit LlmBrainTraceItemScript(TraceHookState* state)
      : AllItemScript("LlmBrainTraceItemScript"), state_(state) {}

  bool CanItemUse(Player* player,
                  Item* item,
                  SpellCastTargets const& /*targets*/) override {
    EmitTraceEvent(state_, player, "item_use", ItemDisplayName(item),
                   item ? static_cast<int64_t>(item->GetEntry()) : 0);
    return false;  // Do not override normal item use.
  }

 private:
  const base::raw_ptr<TraceHookState> state_;
};

}  // namespace

void AddLlmBrainTraceScripts(TraceHookState* state) {
  new LlmBrainTracePlayerScript(state);
  new LlmBrainTraceUnitScript(state);
  new LlmBrainTraceGroupScript(state);
  new LlmBrainTraceGameObjectScript(state);
  new LlmBrainTraceItemScript(state);
}

}  // namespace llm_brain
