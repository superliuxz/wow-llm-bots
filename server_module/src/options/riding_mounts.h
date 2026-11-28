// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure riding-training, mount-purchase, and mount/dismount selection and
// validation helpers (no AzerothCore headers, unit-tested directly).

#ifndef MOD_LLM_BRAIN_OPTIONS_RIDING_MOUNTS_H_
#define MOD_LLM_BRAIN_OPTIONS_RIDING_MOUNTS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

// WotLK 3.3.5a map IDs where flying mounts are allowed (Outland, Northrend).
inline constexpr uint32_t kMapOutland = 530;
inline constexpr uint32_t kMapNorthrend = 571;

// WotLK 3.3.5a riding skill line and riding spell IDs.
inline constexpr uint32_t kSkillRiding = 762;
inline constexpr uint32_t kSpellApprenticeRiding = 33388;   // Level 20, 75
inline constexpr uint32_t kSpellJourneymanRiding = 33391;   // Level 40, 150
inline constexpr uint32_t kSpellExpertRiding = 34090;       // Level 60, 225
inline constexpr uint32_t kSpellArtisanRiding = 34091;      // Level 70, 300
inline constexpr uint32_t kSpellColdWeatherFlying = 54197;  // Level 77

// Returns the canonical English name for a known WotLK riding spell ID, or an
// empty string if `spell_id` is not one of the five riding spells.
std::string_view RidingSpellName(uint32_t spell_id);

// Formats a copper price concisely for option labels (e.g. "4g", "3g 60s",
// "90s", "50c", "free").
std::string FormatCopperCost(uint32_t copper);

// Applies a reputation discount factor in [0, 1] to a base copper cost.
uint32_t DiscountedCopperCost(uint32_t base_copper, float discount);

struct RidingTrainingCandidate {
  uint32_t spell_id = 0;
  std::string name;
  uint32_t req_level = 0;
  uint32_t cost_copper = 0;
  bool can_teach = false;
};

struct RidingTrainingOption {
  uint32_t spell_id = 0;
  uint32_t cost_copper = 0;
  std::string label;
};

// Selects affordable, teachable riding spells in ascending level/spell order
// and formats option labels for `npc_name`.
std::vector<RidingTrainingOption> SelectAffordableRidingTraining(
    const std::vector<RidingTrainingCandidate>& candidates,
    uint32_t bot_money_copper,
    std::string_view npc_name);

// Pure validation before teaching a riding spell at a mount trainer.
// Returns empty string when valid, else a concise failure reason.
std::string ValidateRidingTrain(bool in_combat,
                                bool valid_trainer,
                                bool spell_found,
                                bool can_teach,
                                uint32_t cost_copper,
                                uint32_t bot_money_copper);

struct MountVendorCandidate {
  uint32_t item_id = 0;
  std::string name;
  uint32_t price_copper = 0;
  uint32_t speed_pct = 0;
  bool is_flying = false;
  bool already_known = false;
  bool in_bags = false;
  bool meets_requirements = false;
};

struct MountPurchaseOption {
  uint32_t item_id = 0;
  uint32_t price_copper = 0;
  uint32_t speed_pct = 0;
  bool is_flying = false;
  std::string label;
};

// Selects up to `max_picks` affordable, usable, unowned mounts that improve
// over the bot's best known ground/flying mount speed (at most one mount per
// `(is_flying, speed_pct)` tier, preferring the cheapest then lowest item ID).
std::vector<MountPurchaseOption> SelectMountPurchases(
    const std::vector<MountVendorCandidate>& candidates,
    uint32_t bot_money_copper,
    uint32_t best_known_ground_speed_pct,
    uint32_t best_known_flight_speed_pct,
    std::string_view npc_name,
    size_t max_picks = 2);

struct MountState {
  bool alive = true;
  bool in_combat = false;
  bool mounted = false;
  bool in_flight = false;
  bool in_vehicle = false;
  bool in_dungeon = false;
  bool outdoors = true;
  bool swimming = false;
};

bool CanOfferMount(const MountState& state);

// Pure validation before casting a mount spell. Returns empty string if valid.
std::string ValidateMountState(const MountState& state, bool has_spell);

struct KnownMountSpell {
  uint32_t spell_id = 0;
  std::string name;
  uint32_t ground_speed_pct = 0;
  uint32_t flight_speed_pct = 0;
  bool is_flying = false;
};

// Inputs for checking whether a bot can summon and fly a flying mount in its
// current area (mirrors WotLK 3.3.5a SpellInfo::CheckLocation +
// Player::canFlyInZone + Spell::CheckCast).
struct FlyableAreaInput {
  uint32_t map_id = 0;
  bool area_is_flyable = false;
  bool area_no_fly_zone = false;
  bool battlefield_blocks_fly = false;
  bool has_cold_weather_flying = false;
  bool outdoors = true;
  bool in_dungeon = false;
};

// Returns true when flying mounts are mechanically usable in the bot's current
// area (Outland outdoors, or Northrend outdoors with Cold Weather Flying,
// excluding no-fly zones such as Dalaran streets and active Wintergrasp).
bool CanFlyInArea(const FlyableAreaInput& in);

// Returns true when a bot can move via 3D flight splines rather than
// ground-only navmesh pathing (free-flying on a flying mount or flight form,
// not on a taxi flight path, not in a dungeon, and out of combat).
bool CanUseFlightMovement(bool can_fly_flag,
                          bool has_fly_aura,
                          bool in_flight_taxi,
                          bool in_dungeon,
                          bool in_combat);

// Computes the next 3D flight waypoint from `from` toward `to` with step size
// at most `max_step_yd`. When still further than `max_step_yd` horizontally,
// lifts altitude by `clearance_z_yd` above the higher endpoint (and optional
// `terrain_z` below the waypoint) so flying bots clear hills and ledges; when
// within one step of `to`, descends directly to `to`.
wlb::snapshot::Vec3 ComputeFlightStep(
    const wlb::snapshot::Vec3& from,
    const wlb::snapshot::Vec3& to,
    float max_step_yd,
    float clearance_z_yd = 12.0f,
    std::optional<float> terrain_z = std::nullopt);

// Picks the best known mount spell to offer. When `can_fly` is true, prefers
// the fastest flying mount (falling back to the fastest ground mount if no
// flying mount is known). When `can_fly` is false, never picks a flying-only
// mount and picks the fastest ground mount (tie-breaking by lowest `spell_id`).
std::optional<KnownMountSpell> PickBestMount(
    const std::vector<KnownMountSpell>& mounts,
    bool can_fly);

bool CanOfferDismount(bool alive,
                      bool mounted,
                      bool in_flight,
                      bool incapacitated);

// Pure validation before dismounting. Returns empty string if valid.
std::string ValidateDismountState(bool mounted,
                                  bool in_flight,
                                  bool incapacitated);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_RIDING_MOUNTS_H_
