// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// AzerothCore-independent group loot roll policy, stat/spec fit evaluation,
// Disenchant eligibility selection, and human-deferral timer tracking.

#ifndef MOD_LLM_BRAIN_OPTIONS_LOOT_ROLL_POLICY_H_
#define MOD_LLM_BRAIN_OPTIONS_LOOT_ROLL_POLICY_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llm_brain {

// How long loot rolls are deferred while at least one connected human member
// in the roll hasn't voted yet (applies in both auto-popup and brain-driven
// roll modes). Strictly shorter than kPopupFallbackDeadlineMs (15 s) and WoW's
// 60 s group-loot timer so human Need rolls are respected without stalling or
// handing off to playerbots.
inline constexpr uint64_t kHumanLootRollDeferMs = 10000;

// Mirrors AzerothCore's ItemSubclassArmor values for body armor tiers.
inline constexpr uint32_t kArmorSubclassCloth = 1;
inline constexpr uint32_t kArmorSubclassLeather = 2;
inline constexpr uint32_t kArmorSubclassMail = 3;
inline constexpr uint32_t kArmorSubclassPlate = 4;

// Mirrors AzerothCore's ItemSubclassWeapon values used for class/role checks.
inline constexpr uint32_t kWeaponSubclassBow = 2;
inline constexpr uint32_t kWeaponSubclassGun = 3;
inline constexpr uint32_t kWeaponSubclassThrown = 16;
inline constexpr uint32_t kWeaponSubclassCrossbow = 18;
inline constexpr uint32_t kWeaponSubclassWand = 19;

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
  bool attack_power = false;         // Melee / general attack power.
  bool ranged_attack_power = false;  // Hunter-only ranged attack power.
  bool mp5 = false;                  // Mana regeneration per 5s.
  bool hit = false;                  // Hit rating (generic/melee/ranged/spell).
  bool crit = false;            // Crit rating (generic/melee/ranged/spell).
  bool haste = false;           // Haste rating.
  bool tank_avoidance = false;  // Defense, dodge, parry, block.
  bool melee_dps = false;       // Armor penetration, expertise.
};

// Numeric item attributes used to score gear upgrades and quest rewards for a
// bot's class, spec profile, and armor proficiency.
struct GearItemInfo {
  uint32_t item_level = 0;
  uint32_t armor = 0;
  float weapon_dps = 0.0f;
  bool is_armor = false;
  bool is_cloak = false;
  bool is_shield = false;
  bool is_weapon = false;
  uint32_t armor_subclass = 0;   // kArmorSubclass* (0 for non-body-armor).
  uint32_t weapon_subclass = 0;  // ItemSubclassWeapon.

  int32_t strength = 0;
  int32_t agility = 0;
  int32_t intellect = 0;
  int32_t spirit = 0;
  int32_t stamina = 0;
  int32_t spell_power = 0;
  int32_t attack_power = 0;
  int32_t ranged_attack_power = 0;
  int32_t mp5 = 0;
  int32_t hit = 0;
  int32_t crit = 0;
  int32_t haste = 0;
  int32_t defense = 0;
  int32_t dodge = 0;
  int32_t parry = 0;
  int32_t block = 0;
  int32_t expertise = 0;
  int32_t armor_pen = 0;
};

struct TradeBagItem {
  uint8_t bag = 0;
  uint8_t slot = 0;
  uint32_t item_id = 0;
  std::string name;
  uint32_t count = 1;
  uint8_t quality = 0;  // 0 = Poor (grey), 1+ = Common or higher.
  bool already_in_trade = false;
  bool can_be_traded = true;
  bool bound_not_with_trader = false;
};

struct TradeOfferOption {
  std::string guid;
  std::string label;
  bool operator==(const TradeOfferOption&) const = default;
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
  bool weapon_disallowed_for_spec = false;
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

// Whether a weapon subclass is off-role for `class_id` + `profile` (e.g.
// Bows/Guns/Crossbows are Hunter primary weapons, not Warrior/Rogue Need
// targets; Wands are caster-only; Thrown is non-Hunter physical).
bool IsWeaponDisallowedForSpec(LootStatProfile profile,
                               uint8_t class_id,
                               uint32_t weapon_subclass);

// Extracts boolean stat presence flags from numeric `GearItemInfo`.
ItemStatFlags StatFlagsFromGearInfo(const GearItemInfo& item);

// Classifies how an item's stats fit a bot's spec archetype.
StatFit EvaluateStatFit(LootStatProfile profile,
                        uint8_t class_id,
                        const ItemStatFlags& stats);

// Scores an equippable item for `profile` / `class_id` / `best_armor_subclass`,
// weighting primary and secondary stats, armor proficiency, and weapon fit
// instead of item level alone.
float ScoreGearItem(LootStatProfile profile,
                    uint8_t class_id,
                    uint32_t best_armor_subclass,
                    const GearItemInfo& item);

// Whether a loot roll should be deferred while waiting for an eligible
// connected human group member to vote first. Applies in both auto-popup and
// brain-driven roll modes.
bool ShouldDeferRollForHuman(bool human_pending_vote,
                             uint64_t roll_elapsed_ms,
                             uint64_t human_defer_ms = kHumanLootRollDeferMs);

// Decides an auto-popup loot roll vote, or std::nullopt when the bot should
// defer briefly while waiting for a human group member to vote.
std::optional<LootRollChoice> DecidePolicyRollVote(
    const LootRollPolicyInput& in);

// Selects tradeable bag items to offer in an open trade window (one option per
// distinct item_id, prioritizing non-grey items ahead of grey junk while
// preserving bag order within each tier). No 8-item cap.
std::vector<TradeOfferOption> SelectTradeItemOffers(
    bool has_free_trade_slot,
    const std::vector<TradeBagItem>& bag_items);

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
