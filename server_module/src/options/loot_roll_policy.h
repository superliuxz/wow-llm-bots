// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// AzerothCore-independent group loot roll policy, stat/spec fit evaluation,
// Disenchant eligibility selection, and human-deferral timer tracking.

#ifndef MOD_LLM_BRAIN_OPTIONS_LOOT_ROLL_POLICY_H_
#define MOD_LLM_BRAIN_OPTIONS_LOOT_ROLL_POLICY_H_

#include <cstdint>
#include <optional>
#include <vector>

namespace llm_brain {

// How long auto-popup defers a loot roll while at least one connected human
// member in the roll hasn't voted yet. Strictly shorter than
// kPopupFallbackDeadlineMs (15 s) and WoW's 60 s group-loot timer so human
// Need rolls are respected without stalling or handing off to playerbots.
inline constexpr uint64_t kHumanLootRollDeferMs = 10000;

// Mirrors AzerothCore's RollVoteMask bits (LootMgr.h).
inline constexpr uint8_t kRollMaskPass = 0x01;
inline constexpr uint8_t kRollMaskNeed = 0x02;
inline constexpr uint8_t kRollMaskGreed = 0x04;
inline constexpr uint8_t kRollMaskDisenchant = 0x08;

// Mirrors AzerothCore's CMSG_LOOT_ROLL vote values.
enum class LootRollChoice : uint8_t {
  kPass = 0,
  kNeed = 1,
  kGreed = 2,
  kDisenchant = 3,
};

// WotLK 3.3.5a mechanical stat archetypes for spec-appropriate gear checks.
enum class LootStatProfile : uint8_t {
  kSpellCaster,
  kMeleeStrength,
  kTankStrength,
  kPhysicalAgility,
  kTankAgility,
};

// Positive stat presence on an item template.
struct ItemStatFlags {
  bool strength = false;
  bool agility = false;
  bool intellect = false;
  bool spirit = false;
  bool stamina = false;
  bool spell_power = false;
  bool attack_power = false;    // Includes ranged attack power.
  bool mp5 = false;             // Mana regeneration per 5s.
  bool tank_avoidance = false;  // Defense, dodge, parry, block rating/value.
  bool melee_dps = false;       // Armor penetration, expertise.
};

enum class StatFit : uint8_t {
  kMatch,     // Has at least one primary stat for this spec profile.
  kNeutral,   // No profile-specific primary or off-spec stats (e.g. Stamina).
  kMismatch,  // Has off-spec stats and no matching primary stat.
};

struct RollKey {
  uint64_t item_guid = 0;
  uint8_t item_slot = 0;
  bool operator==(const RollKey&) const = default;
};

struct PendingRollSeen {
  uint64_t item_guid = 0;
  uint8_t item_slot = 0;
  uint64_t first_seen_ms = 0;
  bool operator==(const PendingRollSeen&) const = default;
};

struct LootRollPolicyInput {
  uint8_t roll_vote_mask = 0;
  bool usable = false;
  bool bind_on_equip = false;
  bool armor_type_matches = false;
  bool shield_disallowed_for_spec = false;
  bool has_upgrade_slot = false;
  StatFit stat_fit = StatFit::kNeutral;

  bool human_already_needed = false;
  bool human_pending_vote = false;
  uint64_t roll_elapsed_ms = 0;
  uint64_t human_defer_ms = kHumanLootRollDeferMs;
};

// Maps a WotLK 3.3.5a class + spec tab + role flags to its stat archetype.
LootStatProfile LootStatProfileForSpec(uint8_t class_id,
                                       uint8_t spec_tab,
                                       bool has_talents,
                                       bool is_tank,
                                       bool is_healer);

// Whether a shield is mechanically off-spec for `profile` (e.g. Arms/Fury
// Warrior, Retribution Paladin, Enhancement Shaman).
bool IsShieldDisallowedForProfile(LootStatProfile profile);

// Classifies how an item's stats fit a bot's spec archetype.
StatFit EvaluateStatFit(LootStatProfile profile,
                        uint8_t class_id,
                        const ItemStatFlags& stats);

// Decides an auto-popup loot roll vote, or std::nullopt when the bot should
// defer briefly while waiting for a human group member to vote.
std::optional<LootRollChoice> DecidePolicyRollVote(
    const LootRollPolicyInput& in);

// Human-readable verb prefix for `choice` ("Need", "Greed", "Disenchant",
// "Pass").
const char* LootRollChoiceVerb(LootRollChoice choice);

// Removes entries from `seen` whose (item_guid, item_slot) is not in
// `active_rolls`.
void PrunePendingRolls(std::vector<PendingRollSeen>* seen,
                       const std::vector<RollKey>& active_rolls);

// Records `now_ms` if `key` is not yet in `seen` and returns `now_ms -
// first_seen_ms` (0 if `now_ms < first_seen_ms`).
uint64_t RecordAndGetRollElapsedMs(std::vector<PendingRollSeen>* seen,
                                   RollKey key,
                                   uint64_t now_ms);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_LOOT_ROLL_POLICY_H_
