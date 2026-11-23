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
  // When `has_boss_notes` is true, returns empty so combat-critical boss notes
  // are never diluted or crowded out by open-world/quest lore.
  std::string SelectRelevant(const wlb::snapshot::Snapshot& snapshot,
                             bool has_boss_notes = false) const;

 private:
  std::flat_map<uint32_t, QuestInfo> quests_;
  std::flat_map<std::string, ZoneInfo> zones_;
  std::flat_map<std::string, DungeonInfo> dungeons_;
  std::flat_map<std::string, BossInfo> bosses_;
};

}  // namespace harness

#endif  // HARNESS_SCREEN_KNOWLEDGE_H_
