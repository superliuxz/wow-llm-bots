// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Unified validate + execute dispatcher for `BotAction` (covering both chosen
// options and standing-order acts; C++ cleanup #1). Area implementations live
// in `exec_shared`, `exec_npc`, `exec_items`, `exec_social`, `exec_movement`,
// and `act_executor`.

#ifndef MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_
#define MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_

#include <cstdint>
#include <string>

#include "base/time/time.h"
#include "options/act_executor.h"
#include "options/action_types.h"
#include "options/exec_movement.h"
#include "options/options.h"
#include "options/position_facts.h"

class Player;

namespace llm_brain {

struct ActionRunContext {
  PendingWalk* walk = nullptr;
  const DungeonNavConfig* dungeon_nav = nullptr;
  ActiveAct* out_active_act = nullptr;
  base::TimeTicks now;
};

struct ExecuteResult {
  // What went wrong, phrased like the client's error line; empty on success.
  std::string error;
  // True when finished at once (no multi-tick tracking needed).
  bool used = false;
  // Optional info summary emitted when the action finishes immediately.
  std::string info;
  // Time spent in PathGenerator when starting an Explore or GoTo walk.
  base::TimeDelta path_time;
  // Non-zero when a bag item or mount started a cast / queued into SpellQueue.
  uint32_t pending_spell_id = 0;
};

// Pre-validates `action` before mutating bot state or cancelling any active
// dodge/movement act. Returns an empty string if valid, or the error text.
std::string ValidateAction(Player* bot, const BotAction& action);

// Single validate + execute path for all bot actions (both `Command::Choose`
// and `Command::Act`). World thread only.
ExecuteResult ExecuteAction(Player* bot,
                            const BotAction& action,
                            ActionRunContext* ctx = nullptr);

// Convenience wrapper that executes `target.action` (chosen option).
ExecuteResult ExecuteOption(Player* bot,
                            const OptionTarget& target,
                            PendingWalk* walk = nullptr,
                            const DungeonNavConfig& dungeon_nav = {});

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_
