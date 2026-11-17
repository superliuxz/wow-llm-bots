// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_
#define MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_

#include <string>
#include <string_view>

#include "options/options.h"

class GameObject;
class ObjectGuid;
class Player;

namespace llm_brain {

// The guid of a snapshot's "0x" + 16 hex digits; empty if malformed.
ObjectGuid ParseSnapshotGuid(std::string_view text);

// Carries out a chosen option with the bot's body: playerbots' own actions
// (attack, loot), with their own checks (line of sight, a valid target), as
// if the player had clicked the target. Returns what went wrong, phrased like
// the client's error line, or an empty string when the body took it on.
// World thread only.
// Gather and Use: herbs, veins and chests go through playerbots' loot
// strategy, which walks there itself; any other object is walked to first and
// used on arrival: the world script calls UseObjectNow() once the bot is in
// reach.
std::string ExecuteOption(Player* bot, const OptionTarget& target);

// Whether `go` is opened through the loot strategy (chest, herb, vein).
bool IsLootableObject(GameObject* go);

// Clicks `go` now through playerbots' "use" action. False when refused.
bool UseObjectNow(Player* bot, GameObject* go);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_
