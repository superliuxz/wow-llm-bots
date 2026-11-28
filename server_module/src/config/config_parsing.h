// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CONFIG_CONFIG_PARSING_H_
#define MOD_LLM_BRAIN_CONFIG_CONFIG_PARSING_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llm_brain {

// Splits "a, b,,c " into {"a", "b", "c"} (trims spaces and tabs, drops empty
// entries).
std::vector<std::string> SplitList(const std::string& csv);

// Parses "Name=file,Name2=/abs/file" into (name, path) pairs. Relative files
// are joined to `dir` when it is set. Malformed entries are skipped.
std::vector<std::pair<std::string, std::string>> ParseFixtures(
    const std::string& csv,
    const std::string& dir);

// What must hold while the strategy changes `changes` ("-follow,+grind,+new
// rpg") are in effect: every strategy added present, every one removed absent
// (the last change to a name wins). If any no longer holds, playerbots has
// reset the strategies since we applied them.
struct StrategyCheck {
  std::string name;
  bool present = true;
  bool operator==(const StrategyCheck&) const = default;
};
std::vector<StrategyCheck> StrategyChecks(const std::string& changes);

// Adjusts `LlmBrain.NonCombatStrategies` for brain ownership, group, and
// dungeon play without touching stripped test configs (where neither `+grind`
// nor `+new rpg` is enabled):
//  - While `brain_led` is true (harness connected and LLM healthy), the brain
//    owns quests, grinding, travel, vendors, and flight paths: `grind` and
//    `new rpg` are stripped (`-grind,-new rpg`) for solo bots, all-bot group
//    leaders, and human-group members alike so playerbots never accepts, turns
//    in, or abandons quests, picks rewards, flies, or teleports when stuck
//    alongside the brain. Grouped non-leader bots still keep `+follow`.
//  - While `brain_led` is false (harness disconnected or LLM unhealthy),
//    playerbots fallback applies: grouped non-leader bots keep
//    `+follow,-grind,-new rpg`, a bot leading a human keeps `-grind,-new rpg`,
//    dungeon leaders/solo bots strip `+new rpg`, and solo/all-bot-leader
//    open-world bots keep `base_non_combat`.
std::string AdjustNonCombatStrategies(const std::string& base_non_combat,
                                      bool in_group,
                                      bool is_leader,
                                      bool in_dungeon,
                                      bool group_has_human,
                                      bool brain_led);

// Returns the playerbots scripted raid strategy name registered in
// `RaidStrategyContext` / `PlayerbotAI::ApplyInstanceStrategies` for `map_id`
// (e.g. 533 -> "naxx", 603 -> "ulduar", 631 -> "icc"), or empty for non-raid
// maps (open world, 5-man dungeons, battlegrounds).
std::string_view ScriptedRaidStrategyForMap(uint32_t map_id);

// Coordinates playerbots' scripted raid strategies with brain ownership:
//  - On a non-raid map (`ScriptedRaidStrategyForMap(map_id)` empty), returns
//    `base_strategies` unchanged.
//  - Keeps playerbots' raid strategy (`+<name>`) enabled as the body layer
//    (preserving vehicles, harpoons, cannons, Malygos drakes, and boss
//    positioning) whenever `apply_instance_strategies` is true and
//    `base_strategies` did not explicitly disable `-<name>`.
//  - While `brain_led` is true, strips only the autonomous open-world
//    sub-behaviors that clash with the brain (`grind`, `new rpg`) if present,
//    never blanket-stripping the raid strategy itself.
std::string AdjustRaidStrategies(const std::string& base_strategies,
                                 uint32_t map_id,
                                 bool brain_led,
                                 bool apply_instance_strategies = true);

// Adjusts `LlmBrain.CombatStrategies` when a bot is in a group:
//  - Preserves `boost` as configured (`+boost` stays `+boost`; `-boost` stays
//    `-boost`): playerbots' `BoostTrigger`/`GenericBoostTrigger` already gates
//    cooldowns (Bloodlust/Heroism, Icy Veins, Arcane Power, Rapid Fire,
//    Shadowfiend, racials, trinkets) on `balance <= 50` or PvP targets, so
//    grouped bots use them on bosses and heavy pulls without blowing them on
//    routine trash.
//  - Enables `threat` (`+threat`) for DPS, but keeps `-threat` for healers:
//    playerbots marks every `CastHealingSpellAction` as
//    `ActionThreatType::Aoe`, causing `ThreatMultiplier` to suppress all heals
//    once AoE threat reaches 50%.
std::string GroupedCombatStrategies(const std::string& combat, bool is_healer);

// Playerbots reflexes held back in group play (they body-pull):
//  - `block_flee`: its `flee` step back from a mob that gets close runs a
//    ranged bot into unpulled packs, so it is blocked in a group in a dungeon
//    or with a human in it. Solo bots (and all-bot groups outdoors) keep it.
//  - `pet_defensive`: a grouped bot's pet on aggressive is set to defensive,
//    so it never runs off to attack a mob nobody pulled.
struct GroupReflexLimits {
  bool block_flee = false;
  bool pet_defensive = false;

  bool operator==(const GroupReflexLimits&) const = default;
};
GroupReflexLimits GroupReflexLimitsFor(bool in_group,
                                       bool in_dungeon,
                                       bool group_has_human);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CONFIG_CONFIG_PARSING_H_
