// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_
#define MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "options/options.h"
#include "proto/snapshot.h"

class Creature;
class GameObject;
class ObjectGuid;
class Player;

namespace llm_brain {

// The guid of a snapshot's "0x" + 16 hex digits; empty if malformed.
ObjectGuid ParseSnapshotGuid(std::string_view text);

// State for an active multi-step mmap walk (Explore / Go to).
struct PendingWalk {
  uint32_t map_id = 0;
  wlb::snapshot::Vec3 dest;
  wlb::snapshot::Vec3 step_dest;
  bool has_step = false;
  float best_dist_yd = 0.0f;
  base::TimeTicks last_progress_at;
};

struct WalkStepResult {
  bool arrived = false;
  bool cancelled = false;
  // Non-empty ("stuck") when pathing fails or progress stalls (< 5 yd closer in
  // 10 s).
  std::string error;
  base::TimeDelta path_time;
};

struct ExecuteResult {
  // What went wrong, phrased like the client's error line; empty when the body
  // took the choice on.
  std::string error;
  // Gather / Use / Talk / Explore / GoTo: true when finished at once (already
  // in reach or already at the destination).
  bool used = false;
  // Optional info summary emitted when Talk finishes at once.
  std::string info;
  // Time spent in PathGenerator when starting an Explore or GoTo walk.
  base::TimeDelta path_time;
};

// Carries out a chosen option with the bot's body: playerbots' own actions
// (attack, loot, eat/drink, quest/trainer/vendor), with their own checks, as
// if the player had clicked the target. World thread only.
// For kExplore and kGoTo, initializes `*walk` when not already at the
// destination.
ExecuteResult ExecuteOption(Player* bot,
                            const OptionTarget& target,
                            PendingWalk* walk = nullptr);

// Advances an active Explore / GoTo walk in ~70 yd steps along the mmap path.
WalkStepResult TickLongWalk(Player* bot,
                            base::TimeTicks now,
                            PendingWalk* walk);

// Whether `go` is opened through the loot strategy (chest, herb, vein).
bool IsLootableObject(GameObject* go);

// Whether the bot can use `go` from where it stands: in reach for an ordinary
// object, within playerbots' loot distance for a lootable one (its loot
// strategy walks the rest).
bool IsInUseRange(Player* bot, GameObject* go);

// Clicks `go` now through playerbots' "use" action. Returns why it was
// refused (distance, height, bag space, casting, ...), empty when used.
std::string UseObjectNow(Player* bot, GameObject* go);

// Whether the bot is close enough to talk to `npc`.
bool IsInTalkRange(Player* bot, Creature* npc);

struct TalkResult {
  std::string error;
  std::string info;
};

// Carries out all available interactions on `npc` (turn in completed quests,
// accept available quests, learn affordable spells, sell grey items, repair,
// bind hearthstone).
TalkResult TalkToNpcNow(Player* bot, Creature* npc);

// Whether the bot currently has a food or drink regeneration aura.
bool HasFoodOrDrinkAura(Player* bot);

// Whether a Rest option is still in progress: alive, out of combat, below full
// HP or mana, and either holding a food/drink aura or sitting.
bool IsRestInProgress(Player* bot, bool* had_food_drink_aura);

// Whether the body is still looting the corpse `guid`: it is playerbots' loot
// target or the open loot window. False once the loot strategy is done with
// it, even if it left items it had no use for.
bool IsLootInProgress(Player* bot, ObjectGuid guid);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_
