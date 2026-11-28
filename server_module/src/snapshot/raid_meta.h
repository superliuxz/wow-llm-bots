// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure, AzerothCore-independent raid metadata helpers (M11):
// - Instance difficulty / size / lockout formatting (`ext.instance`).
// - DBM boss cast/interval timer tracking (`ext.encounter->dbm`) and boss
//   warning formatting (`Event::dbm_warning`).
// - Dispel type mapping and missing raid buff detection.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_RAID_META_H_
#define MOD_LLM_BRAIN_SNAPSHOT_RAID_META_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

// AzerothCore `Difficulty` enum values (DBCEnums.h), mirrored here so pure
// unit tests need no AzerothCore headers.
inline constexpr uint8_t kAcoreDungeonNormal = 0;
inline constexpr uint8_t kAcoreDungeonHeroic = 1;
inline constexpr uint8_t kAcoreRaid10Normal = 0;
inline constexpr uint8_t kAcoreRaid25Normal = 1;
inline constexpr uint8_t kAcoreRaid10Heroic = 2;
inline constexpr uint8_t kAcoreRaid25Heroic = 3;

// AzerothCore `DispelType` enum values (SharedDefines.h).
inline constexpr uint32_t kAcoreDispelMagic = 1;
inline constexpr uint32_t kAcoreDispelCurse = 2;
inline constexpr uint32_t kAcoreDispelDisease = 3;
inline constexpr uint32_t kAcoreDispelPoison = 4;

struct InstanceMetaInput {
  bool is_dungeon_or_raid = false;
  bool is_raid = false;
  uint8_t spawn_mode = 0;
  uint32_t max_players = 0;
  std::string map_name;
  bool perm_bound = false;
  int64_t reset_time_epoch_s = 0;
  int64_t now_epoch_s = 0;
};

// Populates `ext.instance` when inside a dungeon or raid; returns std::nullopt
// outdoors so `ext.instance` stays empty outside instances.
std::optional<wlb::snapshot::Instance> BuildInstanceMeta(
    const InstanceMetaInput& in);

// Maps AzerothCore `SpellInfo::Dispel` to the snapshot `DispelType` enum.
wlb::snapshot::DispelType MapAcoreDispelType(uint32_t acore_dispel);

// Given the buff spell names the bot knows (from its spellbook/action bars)
// and the active positive aura names on each living in-range group member
// (including self), returns up to 4 raid/party buff names the bot can cast
// that at least one member is missing.
std::vector<std::string> ComputeMissingRaidBuffs(
    const std::vector<std::string>& my_known_spells,
    const std::vector<std::vector<std::string>>& member_positive_auras);

// Formats a boss emote or boss whisper into a `DbmWarningEvent`, stripping
// WoW `%s` sender placeholders and trimming whitespace. Returns std::nullopt
// if the resulting warning text is empty.
std::optional<wlb::snapshot::DbmWarningEvent> BuildDbmWarningFromBossChat(
    std::string_view sender,
    std::string_view text);

// Tracks active boss casts and learns repeating boss ability intervals during
// a combat encounter to populate `ext.encounter->dbm`. One per bot (owned by
// `SnapshotBuilder`); resets automatically on encounter end, boss change, or
// explicit `Reset()` (logout / map change / disconnect).
class BossTimerTracker {
 public:
  struct ActiveCast {
    std::string spell_name;
    float remaining_s = 0.0f;
  };

  void Update(uint64_t boss_guid,
              bool boss_in_combat,
              std::optional<ActiveCast> active_cast,
              base::TimeTicks now);
  void Reset();

  std::vector<wlb::snapshot::Encounter_Timer> GetTimers(
      base::TimeTicks now) const;

 private:
  struct CastHistory {
    std::string spell_name;
    base::TimeTicks last_seen_at;
    std::optional<base::TimeDelta> observed_interval;
  };

  struct ActiveTimer {
    std::string name;
    base::TimeTicks expires_at;
  };

  uint64_t boss_guid_ = 0;
  std::optional<ActiveTimer> current_cast_;
  std::vector<CastHistory> history_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_RAID_META_H_
