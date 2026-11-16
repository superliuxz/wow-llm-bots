// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTION_EXECUTOR_H_
#define MOD_LLM_BRAIN_OPTION_EXECUTOR_H_

#include <string>

#include "options.h"

class Player;

namespace llm_brain {

// Carries out a chosen option with the bot's body: playerbots' own actions
// (attack, loot), with their own checks (line of sight, a valid target), as
// if the player had clicked the target. Returns what went wrong, phrased like
// the client's error line, or an empty string when the body took it on.
// World thread only.
std::string ExecuteOption(Player* bot, const OptionTarget& target);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTION_EXECUTOR_H_
