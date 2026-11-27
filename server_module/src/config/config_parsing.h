// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CONFIG_CONFIG_PARSING_H_
#define MOD_LLM_BRAIN_CONFIG_CONFIG_PARSING_H_

#include <string>
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

// Adjusts `LlmBrain.NonCombatStrategies` for group and dungeon play without
// touching stripped test configs (where `+grind` is not enabled):
//  - Grouped non-leader bots keep `+follow,-grind,-new rpg` so the party runs
//    together naturally instead of splitting or hearthing via `+new rpg`.
//  - A bot leading a group with a human in it also gets `-grind,-new rpg`
//    (no `+follow`): it must not wander off from or pull ahead of the human.
//  - Dungeon leaders/solo bots strip `+new rpg` (`-new rpg`) so Playerbots'
//    open-world RPG travel does not Hearthstone them out of the instance.
std::string AdjustNonCombatStrategies(const std::string& base_non_combat,
                                      bool in_group,
                                      bool is_leader,
                                      bool in_dungeon,
                                      bool group_has_human);

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
