// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace_scripts.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "Define.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "Spell.h"
#include "SpellInfo.h"
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
                                   Player* player) {
  if (!player) {
    return {};
  }
  if (std::binary_search(state.bot_names.begin(), state.bot_names.end(),
                         player->GetName())) {
    return "bot";
  }
  if (state.observe_humans && GET_PLAYERBOT_AI(player) == nullptr) {
    return "human";
  }
  return {};
}

void EmitTraceEvent(const TraceHookState* state,
                    Player* player,
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

class LlmBrainTracePlayerScript : public PlayerScript {
 public:
  explicit LlmBrainTracePlayerScript(TraceHookState* state)
      : PlayerScript(
            "LlmBrainTracePlayerScript",
            {PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_LOOT_ITEM,
             PLAYERHOOK_ON_BEFORE_LOOT_MONEY, PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
             PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST}),
        state_(state) {}

  void OnPlayerSpellCast(Player* player,
                         Spell* spell,
                         bool /*skipCheck*/) override {
    if (!player || !spell || !spell->GetSpellInfo()) {
      return;
    }
    const SpellInfo* info = spell->GetSpellInfo();
    const char* sname = info->SpellName[sWorld->GetDefaultDbcLocale()];
    if (!sname || sname[0] == '\0') {
      sname = info->SpellName[0];
    }
    EmitTraceEvent(state_, player, "cast_start", sname ? sname : "",
                   static_cast<int64_t>(info->Id));
  }

  void OnPlayerLootItem(Player* player,
                        Item* item,
                        uint32 count,
                        ObjectGuid /*lootguid*/) override {
    std::string name;
    if (item && item->GetTemplate()) {
      name = item->GetTemplate()->Name1;
    }
    EmitTraceEvent(state_, player, "loot", std::move(name),
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
