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

// True when a leader's explicit "stop dps" command may pause offensive actions
// on a bot: only when grouped, only while the bot or party is in combat, and
// never on an active tank (tanks must keep building threat and taunting).
bool ShouldApplyStopDps(bool in_group,
                        bool is_tank,
                        bool bot_in_combat,
                        bool party_in_combat);

// Records an active "stop dps" hold. Refuses to latch when `in_combat` is
// false so an out-of-combat command can never break the next fight.
void RecordStopDps(StopDpsState& state, bool in_combat);

// Returns true (and clears `state`) once both bot and party are out of combat.
// Keeps `state` active throughout combat until combat ends or an explicit
// resume/reset occurs.
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

// Inputs for guarding against human-placed pre-pull Skull raid icons.
struct UnpulledSkullGuardInput {
  bool in_group = false;
  bool in_battleground = false;
  bool has_skull_mark = false;
  bool skull_in_combat_with_group = false;
  // True when a leader command ("pull skull", "kill skull") or prioritized
  // target explicitly ordered attacking this Skull-marked unit.
  bool explicit_target_override = false;
};

// True when the group's Skull-marked unit is not yet in combat with the group
// and has no explicit pull/kill override, so playerbots' Skull-as-attacker
// reflex must be suppressed until the mob is actually pulled.
bool ShouldSuppressUnpulledSkull(const UnpulledSkullGuardInput& input);

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

// Maximum duration a leader "wait"/"stop" stay hold lasts out of combat before
// automatically resuming follow so forgotten waits cannot stall a bot forever.
inline constexpr base::TimeDelta kLeaderWaitTimeout = base::Minutes(3);

// Tracks an active leader "wait"/"stop" stay hold so an out-of-combat wait
// releases automatically when combat begins (letting healers/DPS move to the
// tank/target), an explicit in-combat wait holds through that fight, and any
// out-of-combat wait expires after `kLeaderWaitTimeout` and resets cleanly on
// follow, group leave, map change, logout, or disconnect.
struct LeaderWaitState {
  bool active = false;
  bool issued_in_combat = false;
  base::TimeTicks started_at;

  bool operator==(const LeaderWaitState&) const = default;
};

void RecordLeaderWait(LeaderWaitState& state,
                      base::TimeTicks now,
                      bool issued_in_combat = false);

// Returns true (and clears `state`) when the caller should clear `policy.stay`:
//  - Immediately when combat starts for an out-of-combat wait
//    (`!state.issued_in_combat`), so healers/DPS aren't held out of range.
//  - After `kLeaderWaitTimeout` out of combat.
// Explicit in-combat waits (`state.issued_in_combat`) hold through that fight
// and downgrade once combat ends so they don't block the next pull.
// Clears `state` without returning true when the bot left the group or `stay`
// was already cleared externally.
bool ShouldExpireLeaderWait(LeaderWaitState& state,
                            bool in_group,
                            bool policy_stay,
                            bool bot_in_combat,
                            bool party_in_combat,
                            base::TimeTicks now);

// Unconditionally clears leader wait state (follow, map change, logout,
// disconnect). Returns true if a wait was active.
bool ClearLeaderWait(LeaderWaitState& state);

// Maximum distance (yd) from a party member for a friendly non-escort NPC
// being attacked by a hostile mob to count as a protected friendly NPC.
inline constexpr float kFriendlyNpcProtectMaxDistYd = 30.0f;

// Maximum distance (yd) from a party member for an active escort NPC being
// attacked to count as pulled for the party.
inline constexpr float kEscortProtectMaxDistYd = 60.0f;

// Maximum distance (yd) from an online human group member for an unpulled mob
// that satisfies one of the human's incomplete quests to be engageable for
// quest assistance ("help <human> with <quest>") in open-world questing.
inline constexpr float kHumanQuestMobAssistMaxDistYd = 25.0f;

// Facts describing whether a nearby attackable mob counts as pulled /
// engageable for a grouped bot.
struct MobPullInput {
  bool in_group = false;
  bool in_dungeon = false;
  // Mob is in combat with or targeting any group member (including this bot).
  bool fights_party_member = false;
  // Mob is in combat with or targeting any group member's pet, totem,
  // guardian, or minion.
  bool fights_party_pet_or_totem = false;
  // Mob is in combat with or targeting an active friendly escort NPC near the
  // party.
  bool fights_escort_npc = false;
  float escort_npc_dist_to_party_yd = 0.0f;
  // Mob is in combat with or targeting a friendly NPC near the party.
  bool fights_friendly_npc = false;
  float friendly_npc_dist_to_party_yd = 0.0f;
  // True when the mob is an active incomplete quest objective (kill credit or
  // quest item drop) for an online human member of the group on the same map,
  // and `human_quest_mob_dist_yd` is the mob's distance to that human.
  bool is_human_quest_mob = false;
  float human_quest_mob_dist_yd = 0.0f;
  // True when the human is AFK/offline or a healer-mana pull wait is active.
  // Active combat (party/pet/totem/escort/friendly NPC) still bypasses this so
  // bots defend allies; unpulled quest-mob assistance respects the pause.
  bool human_away_or_paused = false;
};

// Returns true when a nearby attackable mob counts as pulled / engageable for
// the group (not gated by `hold_pulls` and not tagged `(not pulled)`).
// Always returns true when solo (`!input.in_group`).
bool IsMobPulledForGroup(const MobPullInput& input);

// Inputs for deciding whether `OptionFacts::hold_pulls` should gate unpulled
// mobs for a bot in either a human-led or bot-led group.
struct PullGateInput {
  bool in_group = false;
  bool offline_human_pause = false;
  // An online human group member tanks (or is MT), is AFK, is on another map,
  // or is more than `kHumanBehindYd` behind.
  bool human_holds = false;
  bool healer_mana_hold = false;
  bool human_leads = false;
  bool bot_leads = false;
  // This bot is an effective tank (and not marked `not_tank`).
  bool bot_tanks = false;
  // Another online, alive, same-map group member is an effective tank.
  bool has_other_tank = false;
};

// Returns true when unpulled mobs should be withheld from Attack / assist
// options for this bot (applies consistently to human-led and bot-led groups).
bool ShouldHoldPulls(const PullGateInput& input);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_PULL_DISCIPLINE_H_
