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
  // Where the bot was when it last made progress (moved 5 yd), and when.
  wlb::snapshot::Vec3 progress_pos;
  base::TimeTicks last_progress_at;
  // When the path was last worked out: at most once a second.
  base::TimeTicks last_path_at;
};

struct WalkStepResult {
  bool arrived = false;
  // Non-empty when the walk ends without arriving: "stuck" (no path, or the
  // bot moved less than 5 yd in 10 s; a detour that leads away for a while
  // still counts as moving), or "walk interrupted: ..." (combat, death, another
  // map), so the harness hears of it instead of waiting for its timeout.
  std::string error;
  base::TimeDelta path_time;
};

struct ExecuteResult {
  // What went wrong, phrased like the client's error line; empty when the body
  // took the choice on.
  std::string error;
  // Loot / Gather / Use / Talk / Explore / GoTo: true when finished at once
  // (already in reach or already at the destination).
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

// While a Rest step runs: starts eating or drinking when that one isn't going
// yet (a second item used in the same tick as the first is refused, so a bot
// would eat and never drink). Call at most about once a second.
void ContinueRest(Player* bot);

// Whether a Rest option is still in progress: alive, out of combat, below full
// HP or mana, and either holding a food/drink aura or sitting.
bool IsRestInProgress(Player* bot, bool* had_food_drink_aura);

// Whether the bot is close enough to loot `creature` now.
bool IsInLootRange(Player* bot, Creature* creature);

// Opens `creature`'s corpse, takes its money and auto-loots all items for `bot`
// through the server's loot opcodes (including grey items playerbots' own
// >80%-bag-space filter would leave behind). Returns why it failed ("Inventory
// is full", ...), or empty when looted.
std::string LootCorpseNow(Player* bot, Creature* creature);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_OPTION_EXECUTOR_H_
