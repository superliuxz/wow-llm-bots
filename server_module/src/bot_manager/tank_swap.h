// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Tank-swap coordination between the LLM brain and Playerbots' "lose aggro"
// reflex (Milestone M1b):
//  - When the brain issues a TauntAction on a mob in a group, records a
//    per-target claim for that tank.
//  - BrainPolicyListener consults ShouldSuppressReflexTaunt() before allowing
//    Playerbots' reflex taunt actions so the previous tank does not taunt the
//    mob right back (taunt ping-pong / diminishing returns).
//  - Yields only when another living group tank holds aggro, has an active
//    taunt aura on the mob, or recently claimed it via a brain taunt. Solo
//    tanks and emergency rescues (non-tank victim, dead co-tank) are never
//    blocked.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_TANK_SWAP_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_TANK_SWAP_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace llm_brain {

// Grace window after a brain TauntAction during which other tanks in the same
// group suppress reflex taunts on that target while the taunt lands and threat
// stabilizes.
inline constexpr uint32_t kBrainTauntGraceMs = 8000;

// Maximum lifetime of a stored brain-taunt claim before automatic pruning.
inline constexpr uint32_t kMaxTankSwapClaimAgeMs = 60000;

struct TankSwapClaim {
  uint64_t target_guid = 0;
  uint64_t claiming_tank_guid = 0;
  uint64_t group_guid = 0;
  uint32_t map_id = 0;
  uint32_t instance_id = 0;
  uint32_t claimed_at_ms = 0;

  bool operator==(const TankSwapClaim&) const = default;
};

struct ReflexTauntCheck {
  bool in_group = false;
  uint64_t bot_guid = 0;
  uint64_t target_guid = 0;
  // True if the target's current victim is another living friendly tank in the
  // same group (main-tank flag, LFG tank role, or tank spec without not_tank).
  bool victim_is_other_living_tank = false;
  // True if the target currently has a SPELL_AURA_MOD_TAUNT aura cast by
  // another living tank in the same group (covers both bot and human tanks).
  bool target_taunted_by_other_tank = false;
  // Active brain-taunt claim on this target in the same group/map/instance.
  bool has_active_claim = false;
  uint64_t claiming_tank_guid = 0;
  bool claiming_tank_alive = false;
  bool claim_within_grace = false;
};

// True for playerbots' reflex taunt action names across Warrior, Paladin,
// Druid, and Death Knight tank strategies.
bool IsReflexTauntActionName(std::string_view action_name);

// True when a grouped tank bot's playerbots reflex taunt should be suppressed
// to avoid taunt ping-pong with another tank.
bool ShouldSuppressReflexTaunt(const ReflexTauntCheck& check);

// Thread-safe table of recent brain-initiated tank-swap claims.
class TankSwapTracker {
 public:
  static TankSwapTracker& Instance();

  // Records (or overwrites) a brain-taunt claim for `claim.target_guid`.
  // Ignored if `target_guid`, `claiming_tank_guid`, or `group_guid` is 0.
  void RecordClaim(const TankSwapClaim& claim);

  // Looks up a claim for `target_guid` matching `(group_guid, map_id,
  // instance_id)` that has not exceeded `kMaxTankSwapClaimAgeMs` at `now_ms`.
  std::optional<TankSwapClaim> FindClaim(uint64_t target_guid,
                                         uint64_t group_guid,
                                         uint32_t map_id,
                                         uint32_t instance_id,
                                         uint32_t now_ms) const;

  // True if `claim` is within `kBrainTauntGraceMs` of `now_ms`.
  static bool IsWithinGrace(const TankSwapClaim& claim, uint32_t now_ms);

  // Removes any claims owned by `bot_guid` (called on logout, map change,
  // disconnect, or leaving combat/group).
  void ClearForBot(uint64_t bot_guid);

  // Removes any claim on `target_guid` (e.g. target died or left combat).
  void ClearForTarget(uint64_t target_guid);

  // Removes all claims (server shutdown / test reset).
  void ClearAll();

  size_t size() const;

 private:
  mutable base::Lock lock_;
  std::vector<TankSwapClaim> claims_ GUARDED_BY(lock_);
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_TANK_SWAP_H_
