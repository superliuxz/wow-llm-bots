// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Compact, read-only world knowledge index (exported from `atlas.sqlite` +
// `wiki_335a.sqlite` by `tools/knowledge/export_knowledge.py`). Selects a small
// Markdown guide tailored to the bot's current zone, level, active quests, and
// instance for injection into `BuildPlanRequest`.

#ifndef HARNESS_SCREEN_KNOWLEDGE_H_
#define HARNESS_SCREEN_KNOWLEDGE_H_

#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "snapshot/snapshot.h"

namespace harness {

class WorldKnowledge {
 public:
  struct QuestInfo {
    std::string title;
    uint32_t level = 0;
    std::string log;
    std::string ender;
    std::vector<std::string> drops;
    std::string tip;
  };

  struct ZoneInfo {
    uint32_t min_level = 0;
    uint32_t max_level = 0;
    std::string faction;
    std::vector<std::string> next_zones;
  };

  struct DungeonInfo {
    std::string level;
    std::string summary;
  };

  struct BossInfo {
    std::string abilities;
    std::string strategy;
  };

  static std::optional<WorldKnowledge> LoadFile(std::string_view path,
                                                std::string* error = nullptr);
  static std::optional<WorldKnowledge> ParseJson(std::string_view json_text,
                                                 std::string* error = nullptr);

  bool empty() const {
    return quests_.empty() && zones_.empty() && dungeons_.empty() &&
           bosses_.empty();
  }
  size_t quest_count() const { return quests_.size(); }
  size_t zone_count() const { return zones_.size(); }
  size_t dungeon_count() const { return dungeons_.size(); }
  size_t boss_count() const { return bosses_.size(); }

  // Returns a compact Markdown snippet (<= 1,200 bytes) relevant to `snapshot`.
  // When `has_boss_notes` is true, open-world zone, generic instance overview,
  // and quest lore are suppressed so combat-critical boss notes are never
  // diluted; wiki boss entries are still emitted as a per-boss fallback for any
  // active/nearby boss not covered by a `## <Boss>` heading in `boss_notes`
  // (when `boss_notes` is empty, all wiki output is suppressed).
  std::string SelectRelevant(const wlb::snapshot::Snapshot& snapshot,
                             bool has_boss_notes = false,
                             std::string_view boss_notes = {}) const;

 private:
  std::flat_map<uint32_t, QuestInfo> quests_;
  std::flat_map<std::string, ZoneInfo> zones_;
  std::flat_map<std::string, DungeonInfo> dungeons_;
  std::flat_map<std::string, BossInfo, std::less<>> bosses_;
};

}  // namespace harness

#endif  // HARNESS_SCREEN_KNOWLEDGE_H_
