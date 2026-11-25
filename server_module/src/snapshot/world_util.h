// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Small AzerothCore helpers shared by snapshot builders and option execution.
// Forward-declares core types so this header compiles without AzerothCore
// include paths (tools/wlb check includes). World thread only.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_WORLD_UTIL_H_
#define MOD_LLM_BRAIN_SNAPSHOT_WORLD_UTIL_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "snapshot/snapshot.h"

class Bag;
class Creature;
class Item;
class Player;
class Spell;
class SpellInfo;
class Unit;

namespace llm_brain {

// The area's name in the server's DBC locale, or "" if unknown.
std::string AreaName(uint32_t area_id);

// "0x00000000000001FC": the raw GUID, as the client's combat log prints it.
std::string GuidString(uint64_t raw_guid);

// The raid target icon set on `raw_guid` in `bot`'s group, if any.
wlb::snapshot::RaidMarker RaidMarkerOf(const Player* bot, uint64_t raw_guid);

// True for general-purpose bags (excludes profession bags, quivers, etc.).
bool IsGeneralBag(const Bag* bag);

struct BagSlotItem {
  Item* item = nullptr;
  uint8_t bag = 0;
  uint8_t slot = 0;
  bool general_bag = true;
};

// Every item in `bot`'s backpack and equipped bags.
std::vector<BagSlotItem> BagItems(Player* bot);

// The 7 crowd-control flags the client shows on the bot's own state, and their
// packed bitmask for SnapshotSchedule change detection.
wlb::snapshot::Self::Control ReadControlState(const Player* bot);
uint8_t ControlMask(const Player* bot);

// Casts and auras seen on the bot, its selected target, and its attackers at
// the last snapshot, as sorted (unit_raw_guid, spell_id) pairs.
struct ReactiveSnapshotState {
  std::vector<std::pair<uint64_t, uint32_t>> casts;
  std::vector<std::pair<uint64_t, uint32_t>> auras;
};

// Captures current casts and auras on `bot`, its target, and its attackers.
ReactiveSnapshotState CollectReactiveState(Player* bot);

// True if any cast started or any aura appeared on `bot`, its target, or an
// attacker that was not present in `last`.
bool HasNewCastOrAura(Player* bot, const ReactiveSnapshotState& last);

// What may need a step aside, within `radius_yd` of `bot`: casts with a cast
// bar and channels by units hostile to it, and spell areas (DynamicObject)
// hostile casters left on the ground, as sorted (raw_guid, spell_id) pairs.
// A new one makes a combat snapshot due at once (SnapshotSchedule).
inline constexpr float kHostileHazardRadiusYd = 40.0f;
std::vector<std::pair<uint64_t, uint32_t>> CollectHostileHazards(
    Player* bot,
    float radius_yd = kHostileHazardRadiusYd);

// Whether `now` (sorted, as CollectHostileHazards returns) has an entry that
// `seen` (sorted) lacks.
bool HasNewEntry(const std::vector<std::pair<uint64_t, uint32_t>>& now,
                 const std::vector<std::pair<uint64_t, uint32_t>>& seen);

// Localized spell name in the server's default DBC locale, or "" if unknown.
std::string_view SpellLocalizedName(const SpellInfo* info);

// Maximum effect radius (yards) across all effects of `info`.
float MaxSpellRadiusYd(const SpellInfo* info,
                       Unit* caster,
                       Spell* spell = nullptr);

// Visible ground/orb hazard carried by a hidden trigger creature (active cast,
// channel, or visual/periodic-trigger aura such as Flame Strike, Arcane Sphere,
// Coldflame, or Defile).
struct TriggerHazardInfo {
  std::string name;
  float radius_yd = 0.0f;
  std::optional<float> remaining_s;
  std::optional<float> lands_in_s;
  uint32_t spell_id = 0;
  bool is_preparing_cast = false;
};

// Inspects a living hidden trigger creature for a player-visible spell cast or
// visual aura, following periodic-trigger auras to their triggered spell so
// both name and effect radius match what players see and take damage from.
std::optional<TriggerHazardInfo> InspectTriggerHazard(Creature* trigger);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_WORLD_UTIL_H_
