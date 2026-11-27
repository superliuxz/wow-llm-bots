// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pull & threat discipline helpers (Milestone 2b):
//  - Lifecycle state machine for leader-command `rti` overrides so a mark set
//    by "kill/pull <mark>" resets after combat ends (and on map change or
//    disconnect) instead of sticking onto later packs.
//  - WotLK 3.3.5a pet taunt spell classification and group suppression policy
//    so hunter/warlock pets don't autocast taunts over a non-pet tank.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_PULL_DISCIPLINE_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_PULL_DISCIPLINE_H_

#include <cstdint>
#include <string_view>
#include <vector>

#include "base/time/time.h"

namespace llm_brain {

// Playerbots' default `rti` value (`RtiValue::RtiValue`).
inline constexpr std::string_view kDefaultRti = "skull";

// Tracks an active `rti` override applied by a leader chat command.
struct RtiOverrideState {
  bool active = false;
  uint64_t target_guid = 0;
  bool saw_combat = false;

  bool operator==(const RtiOverrideState&) const = default;
};

struct RtiOverrideTickInput {
  bool bot_in_combat = false;
  bool party_in_combat = false;
  bool target_alive_and_in_world = false;
};

// Records that a leader command set `rti` on `target_guid`.
void RecordRtiOverride(RtiOverrideState& state,
                       uint64_t target_guid,
                       bool in_combat);

// Evaluates whether an active `rti` override should reset on this tick.
// Returns true (and clears `state`) when combat ends after the override was
// engaged, or when the marked target dies/disappears while out of combat.
// Keeps the override active throughout combat and while approaching an
// unengaged living target.
bool ShouldResetRtiOverride(RtiOverrideState& state,
                            const RtiOverrideTickInput& input);

// Unconditionally clears an active override (map change, disconnect, logout).
// Returns true if an override was active.
bool ClearRtiOverride(RtiOverrideState& state);

// True for WotLK 3.3.5a pet taunt spells: all ranks of Hunter Growl,
// Warlock Voidwalker Torment & Suffering, and Warlock Felguard Anguish.
bool IsPetTauntSpell(uint32_t spell_id);

// True when a bot's pet taunt autocast should be suppressed: only when
// grouped with a non-pet tank (human tank or tank-role group member).
// Solo pets and groups without a non-pet tank keep pet taunt autocast on.
bool ShouldSuppressPetTaunt(bool in_group, bool has_non_pet_tank);

// Tracks an active "stop dps" leader command so offensive attacks stay paused
// until explicitly resumed, combat ends, or the bot changes maps/disconnects.
struct StopDpsState {
  bool active = false;
  bool saw_combat = false;

  bool operator==(const StopDpsState&) const = default;
};

void RecordStopDps(StopDpsState& state, bool in_combat);

// Returns true (and clears `state`) once combat ends after `stop dps` was
// active in combat. Keeps `state` active throughout combat and during a
// pre-combat hold until combat ends or an explicit resume/reset occurs.
bool ShouldResetStopDps(StopDpsState& state,
                        bool bot_in_combat,
                        bool party_in_combat);

// Unconditionally clears an active `stop dps` state (explicit resume, attack
// command, map change, disconnect, logout). Returns true if it was active.
bool ClearStopDps(StopDpsState& state);

// True for playerbots' offensive/attack action names that must be blocked
// while `stop dps` is active (leaving movement, healing, dispels, and buffs
// unaffected).
bool IsStopDpsBlockedActionName(std::string_view action_name);

// Maximum range (yd) for offering "Mark <mob> with Skull".
inline constexpr float kSkullMarkMaxDistYd = 40.0f;

// Inputs describing a nearby unit evaluated for the tank/leader "Mark with
// Skull" option.
struct SkullMarkCandidate {
  bool alive = false;
  bool is_npc = false;
  bool hostile = false;
  bool has_guid = false;
  float dist_yd = 0.0f;
  bool in_combat_with_group = false;
  bool is_crowd_controlled = false;
  bool has_raid_marker = false;
};

// True when a nearby unit is a safe candidate for "Mark <mob> with Skull".
// Playerbots' AttackersValue treats any Skull-marked unit as an active
// attacker, so marking an unpulled mob chain-pulls its pack, and marking a
// CC'd or already-marked mob breaks crowd control or overwrites CC marks.
bool IsEligibleSkullMarkTarget(const SkullMarkCandidate& candidate);

// True when a Warlock bot's playerbots `cc` combat strategy (which casts Fear
// on the moon CC target, running it into adjacent packs) should be suppressed:
// whenever the Warlock is grouped or in a dungeon/raid outside battlegrounds.
bool ShouldSuppressWarlockCcStrategy(bool is_warlock,
                                     bool in_group,
                                     bool in_dungeon,
                                     bool in_battleground);

// Strategy delta for Warlock `cc`: `",-cc"` when suppressed, `",+cc"` to
// restore it for solo open-world Warlocks, and `""` for other classes or in
// battlegrounds.
std::string_view WarlockCcStrategyDelta(bool is_warlock,
                                        bool in_group,
                                        bool in_dungeon,
                                        bool in_battleground);

// Belt-and-suspenders guard for BrainPolicyListener: blocks playerbots'
// CastCrowdControlSpellAction("fear") (Warlock `fear on cc`) when grouped or
// in a dungeon outside battlegrounds, without affecting Banish or other
// classes' CC spells.
bool ShouldBlockFearCcAction(bool is_cc_spell_action,
                             std::string_view spell_name,
                             bool in_group,
                             bool in_dungeon,
                             bool in_battleground);

// Mechanical healer-mana pull gate thresholds and anti-stall safety timeouts.
inline constexpr float kHealerManaWaitMaxDistYd = 100.0f;
inline constexpr float kHealerPullWaitStartManaPct = 35.0f;
inline constexpr float kHealerPullWaitReadyManaPct = 75.0f;
inline constexpr base::TimeDelta kHealerNoDrinkTimeout = base::Seconds(12);
inline constexpr base::TimeDelta kHealerMaxWaitTimeout = base::Seconds(35);

struct HealerManaMember {
  bool is_healer = false;
  bool online = true;
  bool alive = true;
  bool same_map = true;
  bool uses_mana = true;
  float dist_yd = 0.0f;
  float mana_pct = 100.0f;
  bool drinking = false;
};

struct HealerManaWaitState {
  bool waiting = false;
  bool timed_out = false;
  bool saw_drinking = false;
  base::TimeTicks wait_started;
  base::TimeTicks last_drinking;

  bool operator==(const HealerManaWaitState&) const = default;
};

// Returns true when out-of-combat pulls should wait for a nearby living party
// healer who is low on mana (< 35%) or actively drinking (< 75%). Uses
// hysteresis up to 75% mana and bounded timeouts (12 s if not drinking, 35 s
// max) so a healer without water or an AFK/bugged drink never causes an
// infinite stall. Always returns false when solo or in combat.
bool EvaluateHealerManaPullWait(HealerManaWaitState& state,
                                bool in_group,
                                bool in_combat,
                                const std::vector<HealerManaMember>& members,
                                base::TimeTicks now);

// Unconditionally clears healer-mana wait state (group leave, map change,
// logout, disconnect). Returns true if any state was active.
bool ClearHealerManaWait(HealerManaWaitState& state);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_PULL_DISCIPLINE_H_
