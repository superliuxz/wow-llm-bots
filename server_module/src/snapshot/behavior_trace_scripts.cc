// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace_scripts.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "Creature.h"
#include "Define.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "UnitScript.h"
#include "World.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "snapshot/behavior_trace.h"

namespace llm_brain {
namespace {

// Resolves whether `player` should be traced and returns its role ("bot" or
// "human"), or empty if not traced.
std::string_view ResolveTracedRole(const TraceHookState& state,
                                   const Player* player) {
  if (!player) {
    return {};
  }
  if (std::binary_search(state.bot_names.begin(), state.bot_names.end(),
                         player->GetName())) {
    return "bot";
  }
  if (state.observe_humans &&
      GET_PLAYERBOT_AI(const_cast<Player*>(player)) == nullptr) {
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

class LlmBrainTracePlayerScript : public PlayerScript {
 public:
  explicit LlmBrainTracePlayerScript(TraceHookState* state)
      : PlayerScript(
            "LlmBrainTracePlayerScript",
            {PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_LOOT_ITEM,
             PLAYERHOOK_ON_BEFORE_LOOT_MONEY, PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
             PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
             PLAYERHOOK_ON_QUEST_REWARD_ITEM,
             PLAYERHOOK_ON_CREATURE_KILL_CREDIT,
             PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
             PLAYERHOOK_ON_PLAYER_RESURRECT, PLAYERHOOK_ON_LEVEL_CHANGED,
             PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_PLAYER_LEARN_TALENTS,
             PLAYERHOOK_ON_EQUIP, PLAYERHOOK_ON_UNEQUIP_ITEM,
             PLAYERHOOK_ON_AFTER_MOVE_ITEM_FROM_INVENTORY,
             PLAYERHOOK_ON_BEFORE_BUY_ITEM_FROM_VENDOR,
             PLAYERHOOK_ON_LEARN_TAXI_NODE}),
        state_(state) {}

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

  void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override {
    if (!player || !killed) {
      return;
    }
    EmitTraceEvent(state_, player, "mob_kill", killed->GetName(),
                   static_cast<int64_t>(killed->GetLevel()));
  }

  void OnPlayerKilledByCreature(Creature* killer, Player* killed) override {
    if (!killed) {
      return;
    }
    EmitTraceEvent(state_, killed, "died", killer ? killer->GetName() : "",
                   killer ? static_cast<int64_t>(killer->GetLevel()) : 0);
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

  void OnPlayerLearnTaxiNode(Player const* player, uint32 node_id) override {
    EmitTraceEvent(state_, player, "taxi_learn", "",
                   static_cast<int64_t>(node_id));
  }

 private:
  const base::raw_ptr<TraceHookState> state_;
};

class LlmBrainTraceUnitScript : public UnitScript {
 public:
  explicit LlmBrainTraceUnitScript(TraceHookState* state)
      : UnitScript("LlmBrainTraceUnitScript", true, {UNITHOOK_ON_DAMAGE}),
        state_(state) {}

  void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override {
    if (!victim || damage == 0) {
      return;
    }
    Player* player = victim->ToPlayer();
    if (!player) {
      return;
    }
    EmitTraceEvent(state_, player, "damage_taken",
                   attacker ? attacker->GetName() : "",
                   static_cast<int64_t>(damage));
  }

 private:
  const base::raw_ptr<TraceHookState> state_;
};

}  // namespace

void AddLlmBrainTraceScripts(TraceHookState* state) {
  new LlmBrainTracePlayerScript(state);
  new LlmBrainTraceUnitScript(state);
}

}  // namespace llm_brain
