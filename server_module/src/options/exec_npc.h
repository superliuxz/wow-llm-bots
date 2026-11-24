// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// NPC action discovery and execution (quests, class trainers, vendors,
// flight masters, innkeepers).

#ifndef MOD_LLM_BRAIN_OPTIONS_EXEC_NPC_H_
#define MOD_LLM_BRAIN_OPTIONS_EXEC_NPC_H_

#include <string>
#include <vector>

#include "options/action_types.h"
#include "options/exec_shared.h"
#include "snapshot/snapshot.h"

class Creature;
class Player;

namespace llm_brain {

struct TalkResult {
  std::string error;
  std::string info;
};

// Approaches and faces `npc` once in talk range. Pure harness: never makes
// quest/reward/trainer/vendor decisions in C++.
TalkResult TalkToNpcNow(Player* bot, Creature* npc);

// Collects explicit per-quest, per-reward, class-trainer, vendor
// (sell-grey/repair & buy upkeep), flight-master, and innkeeper actions for
// friendly NPCs within talk range.
void CollectNpcActions(Player* bot,
                       const wlb::snapshot::Snapshot& snapshot,
                       bool has_food,
                       bool has_drink,
                       std::vector<wlb::snapshot::OptionFacts_NpcAction>* out);

ActionOutcome ExecuteTalkAction(Player* bot, const TalkAction& action);
ActionOutcome ExecuteNpcAction(Player* bot, const NpcAction& action);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_EXEC_NPC_H_
