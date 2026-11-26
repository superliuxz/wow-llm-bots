// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Combat and soak event hooks for behavior traces (C++ cleanup #5):
// `spell_cast`, `damage_dealt`, `damage_taken`, `heal_done`, `mob_kill`,
// `died`, `xp_gain`, `quest_turnin`.

#include "snapshot/behavior_trace_scripts.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "Creature.h"
#include "Define.h"
#include "Group.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Unit.h"
#include "UnitScript.h"
#include "World.h"
#include "WorldSession.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "snapshot/behavior_trace.h"

namespace llm_brain {
namespace {

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

class LlmBrainTracePlayerScript : public PlayerScript {
 public:
  explicit LlmBrainTracePlayerScript(TraceHookState* state)
      : PlayerScript(
            "LlmBrainTracePlayerScript",
            {PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
             PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
             PLAYERHOOK_ON_GIVE_EXP}),
        state_(state) {}

  void OnPlayerSpellCast(Player* player,
                         Spell* spell,
                         bool /*skipCheck*/) override {
    if (!player || !spell || !spell->GetSpellInfo()) {
      return;
    }
    const SpellInfo* info = spell->GetSpellInfo();
    EmitTraceEvent(state_, player, "spell_cast", SpellDisplayName(info),
                   static_cast<int64_t>(info->Id));
  }

  void OnPlayerCompleteQuest(Player* player, Quest const* quest) override {
    if (!quest) {
      return;
    }
    EmitTraceEvent(state_, player, "quest_turnin", quest->GetTitle(),
                   static_cast<int64_t>(quest->GetQuestId()));
  }

  void OnPlayerPVPKill(Player* killer, Player* killed) override {
    if (!killed) {
      return;
    }
    if (!killer || killer == killed) {
      EmitTraceEvent(state_, killed, "died", "environment", 0);
      return;
    }
    EmitTraceEvent(state_, killed, "died", "",
                   static_cast<int64_t>(killer->GetLevel()));
  }

  void OnPlayerKilledByCreature(Creature* killer, Player* killed) override {
    if (!killed) {
      return;
    }
    EmitTraceEvent(state_, killed, "died", killer ? killer->GetName() : "",
                   killer ? static_cast<int64_t>(killer->GetLevel()) : 0);
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

 private:
  const base::raw_ptr<TraceHookState> state_;
};

class LlmBrainTraceUnitScript : public UnitScript {
 public:
  explicit LlmBrainTraceUnitScript(TraceHookState* state)
      : UnitScript(
            "LlmBrainTraceUnitScript",
            true,
            {UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_ON_UNIT_DEATH}),
        state_(state) {}

  void OnUnitDeath(Unit* unit, Unit* killer) override {
    if (!unit) {
      return;
    }
    Creature* killed = unit->ToCreature();
    if (!killed) {
      return;
    }
    Player* kp =
        killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    Group* group = kp ? kp->GetGroup() : killed->GetLootRecipientGroup();
    if (!kp && !group) {
      kp = killed->GetLootRecipient();
    }
    const std::string name = killed->GetName();
    const int64_t lvl = static_cast<int64_t>(killed->GetLevel());
    if (kp) {
      EmitTraceEvent(state_, kp, "killing_blow", name, lvl);
    }
    if (group) {
      for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
           itr = itr->next()) {
        Player* member = itr->GetSource();
        if (member && member->IsWithinDistInMap(killed, 100.0f)) {
          EmitTraceEvent(state_, member, "mob_kill", name, lvl);
        }
      }
      return;
    }
    if (kp) {
      EmitTraceEvent(state_, kp, "mob_kill", name, lvl);
    }
  }

  void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override {
    if (damage == 0) {
      return;
    }
    if (victim) {
      if (Player* vp = victim->ToPlayer()) {
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

  void OnHeal(Unit* healer, Unit* /*receiver*/, uint32& gain) override {
    if (gain == 0 || !healer) {
      return;
    }
    if (Player* hp = healer->GetCharmerOrOwnerPlayerOrPlayerItself()) {
      EmitTraceEvent(state_, hp, "heal_done", "", static_cast<int64_t>(gain));
    }
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
