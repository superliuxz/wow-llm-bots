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

struct ExecuteResult {
  // What went wrong, phrased like the client's error line; empty when the body
  // took the choice on.
  std::string error;
  // Gather / Use: the object was used at once (in reach). Otherwise the bot
  // is walking to it, and the world script uses it on arrival (IsInUseRange,
  // UseObjectNow).
  bool used = false;
};

// Carries out a chosen option with the bot's body: playerbots' own actions
// (attack, loot), with their own checks (line of sight, a valid target), as
// if the player had clicked the target. World thread only.
// Gather and Use: an object out of reach is walked to first (as a player
// would before clicking it); herbs, veins and chests then go through
// playerbots' loot strategy.
ExecuteResult ExecuteOption(Player* bot, const OptionTarget& target);

// Whether `go` is opened through the loot strategy (chest, herb, vein).
bool IsLootableObject(GameObject* go);

// Whether the bot can use `go` from where it stands: in reach for an ordinary
// object, within playerbots' loot distance for a lootable one (its loot
// strategy walks the rest).
bool IsInUseRange(Player* bot, GameObject* go);

// Clicks `go` now through playerbots' "use" action. Returns why it was
// refused (distance, height, bag space, casting, ...), empty when used.
std::string UseObjectNow(Player* bot, GameObject* go);

// Whether the body is still looting the corpse `guid`: it is playerbots' loot
// target or the open loot window. False once the loot strategy is done with
// it, even if it left items it had no use for.
bool IsLootInProgress(Player* bot, ObjectGuid guid);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_
