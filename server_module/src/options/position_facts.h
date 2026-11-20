// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTIONS_POSITION_FACTS_H_
#define MOD_LLM_BRAIN_OPTIONS_POSITION_FACTS_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "snapshot/snapshot.h"

// The parts of the Explore / Go to facts that need no game server
// (option_facts.h computes them from the world): unit-tested.
namespace llm_brain {

inline constexpr float kMinimapRangeYd = 150.0f;
// Explore spots and Go to places nearer or farther than this are not offered.
inline constexpr float kMinTravelDistYd = 25.0f;
inline constexpr float kMaxTravelDistYd = 2500.0f;

float Dist(const wlb::snapshot::Vec3& a, float x, float y, float z);
wlb::snapshot::Direction DirFrom(const wlb::snapshot::Vec3& from,
                                 float x,
                                 float y);

// Compass angles (8 directions) and probe distances (yd) used when falling
// back to walkable ray probing in dungeons/cities with no grind buckets.
std::span<const float> ExploreRayAngles();
std::span<const float> ExploreRayDistancesYd();

// Pure progress check for stepped long walks (Explore / GoTo). Detects both
// stationary stalls (no 5 yd move in 10 s) and ping-pong loops (moving without
// getting >= 3 yd closer to dest in 30 s). Arrives if within 10 yd, or within
// 12 yd when a ping-pong/no-closer timeout triggers near an unreachable hub
// centroid.
enum class WalkProgressVerdict { kContinue, kArrived, kStuck };
struct WalkProgressTracker {
  wlb::snapshot::Vec3 progress_pos;
  base::TimeTicks last_progress_at;
  float best_dist_to_dest = 0.0f;
  base::TimeTicks last_closer_at;
};
WalkProgressVerdict CheckWalkProgress(const wlb::snapshot::Vec3& pos,
                                      float dist_to_dest,
                                      base::TimeTicks now,
                                      WalkProgressTracker* tracker);

// Formats the outcome of TalkToNpcNow. When `done` is empty and a trainer
// refused (e.g. "not enough money for new spells" or "no new spells to
// learn"), returns that refusal as `error` so the harness backs off instead of
// treating it as a successful interaction and looping.
struct TalkOutcome {
  std::string error;
  std::string info;
};
TalkOutcome FormatTalkOutcome(std::string_view npc_name,
                              const std::vector<std::string>& done,
                              std::string_view trainer_refusal);

// Pure predicate for HasTakeableLoot: true only when the corpse is dead,
// flagged lootable, allowed for the bot, and actually has gold or at least one
// slot the bot may take now (not solely items locked in a group roll).
bool HasCorpseTakeableLoot(bool is_dead,
                           bool has_lootable_flag,
                           bool allowed_to_loot,
                           uint32_t gold,
                           size_t takeable_slot_count);

// Pure predicate for out-of-LOS kAttack fallback: walk toward valid living
// non-friendly targets when Playerbots' AttackAction refuses solely due to LOS.
bool ShouldApproachOutOfLosTarget(bool is_alive,
                                  bool is_friendly,
                                  bool is_valid_attack_target,
                                  bool in_los);

// Pure state machine for StepActiveOption(kAttack): closes range/LOS on idle
// attack targets, engages as soon as LOS clears while approaching, and fails
// cleanly after 8 s stuck instead of freezing until the harness timeout.
enum class AttackStepAction {
  kActive,
  kEngageWhileMoving,
  kRetryWalk,
  kRetryExecute,
  kStuck,
};
AttackStepAction CheckAttackStep(bool in_combat,
                                 bool casting,
                                 bool moving,
                                 float dist_yd,
                                 bool in_los,
                                 base::TimeTicks now,
                                 base::TimeTicks* idle_since);

// True when a living bot in combat still holds a stale selection/target that
// would stall Playerbots' combat engine from acquiring a live enemy:
//  - a dead non-friendly target (preserves dead friendly targets for combat
//    resurrection), or
//  - for a non-healer that isn't mid-cast, a friendly/self target left over
//    from pre-pull buffing or conjuring (e.g. Arcane Brilliance / Mana Gem /
//    Soulstone).
bool ShouldClearStaleCombatTarget(bool in_combat,
                                  bool bot_alive,
                                  bool has_target,
                                  bool target_alive,
                                  bool target_friendly = false,
                                  bool bot_is_healer = false,
                                  bool bot_casting = false);

// The Explore spots and Go to places seen from one position. Distances and
// directions are filled for the position they were computed at; RefreshFrom
// re-measures them from a nearby one (cheap), so a bot recomputes them only
// when it has moved or time has passed (SnapshotBuilder).
struct PositionFacts {
  std::vector<wlb::snapshot::OptionFacts::Spot> spots;
  std::vector<wlb::snapshot::OptionFacts::Place> places;
  // Hubs near enough to be on the minimap soon (TOWN / CAMP; `rel.position`
  // only: AddPositionFacts measures them).
  std::vector<wlb::snapshot::Place> minimap_hubs;
};

// Re-measures distance and direction from `pos`, dropping spots and places
// now nearer than the travel minimum or farther than the maximum.
void RefreshFrom(const wlb::snapshot::Vec3& pos, PositionFacts* facts);

// Adds the facts of `position` to `out` (spots, places) and the hubs within
// minimap range of `pos` to the minimap, then the nearest quest turn-in and
// objective from `quest_tracker` as places. The same code live
// (SnapshotBuilder) and offline (test/facts_dump.cc).
void AddPositionFacts(
    const PositionFacts& position,
    const wlb::snapshot::Vec3& pos,
    const std::vector<wlb::snapshot::Objective>& quest_tracker,
    wlb::snapshot::OptionFacts* out,
    wlb::snapshot::Minimap* minimap);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_POSITION_FACTS_H_
