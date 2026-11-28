// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// NPC action discovery and execution (quests, class trainers, vendors,
// flight masters, innkeepers).

#ifndef MOD_LLM_BRAIN_OPTIONS_EXEC_NPC_H_
#define MOD_LLM_BRAIN_OPTIONS_EXEC_NPC_H_

#include <cstdint>
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

// Approaches, faces, and opens gossip (when `npc` has UNIT_NPC_FLAG_GOSSIP)
// once in talk range. Pure harness: never makes quest/reward/trainer/vendor
// or dialogue decisions in C++.
TalkResult TalkToNpcNow(Player* bot, Creature* npc);

// True when `npc` has selectable gossip/dialogue options for `bot` (an already
// open gossip menu, DB dialogue options whose conditions pass, or a scripted
// gossip NPC). Read-only: never mutates `PlayerTalkClass`.
bool NpcHasDialogue(Player* bot, Creature* npc);

// Collects explicit per-quest, per-reward, class-trainer, vendor
// (sell-grey/repair & buy upkeep), flight-master, innkeeper, and open gossip
// dialogue actions for friendly NPCs within talk range.
void CollectNpcActions(Player* bot,
                       const wlb::snapshot::Snapshot& snapshot,
                       uint32_t food_count,
                       uint32_t drink_count,
                       std::vector<wlb::snapshot::OptionFacts_NpcAction>* out);

// Returns the nearest living friendly repair/vendor NPC (`!for_train`) or
// valid class trainer for `bot` (`for_train`) within `max_dist_yd`, or nullptr.
Creature* FindNearestServiceNpc(Player* bot,
                                bool for_train,
                                float max_dist_yd = 150.0f);

ActionOutcome ExecuteTalkAction(Player* bot, const TalkAction& action);
ActionOutcome ExecuteNpcAction(Player* bot, const NpcAction& action);
ActionOutcome ExecuteObjectQuestAction(Player* bot,
                                       const ObjectQuestAction& action);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_EXEC_NPC_H_
