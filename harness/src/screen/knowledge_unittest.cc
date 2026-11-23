// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/knowledge.h"

#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

constexpr const char* kSampleKnowledgeJson = R"json({
  "version": 1,
  "quests": {
    "7": {
      "title": "Kobold Camp Cleanup",
      "level": 2,
      "min_level": 1,
      "log": "Kill 8 Kobold Vermin.",
      "starter": "Marshal McBride",
      "ender": "Marshal McBride",
      "tip": "Kobold Vermin are north of the abbey."
    },
    "60": {
      "title": "Kobold Candles",
      "level": 7,
      "min_level": 4,
      "log": "Collect 8 Large Candles.",
      "ender": "William Pestle",
      "drops": ["Kobold Miner", "Kobold Geomancer"]
    }
  },
  "zones": {
    "Elwynn Forest": {
      "min_level": 1,
      "max_level": 10,
      "faction": "Alliance",
      "next_zones": ["Westfall (10-20)", "Loch Modan (10-20)"]
    }
  },
  "dungeons": {
    "Deadmines": {
      "level": "15-21",
      "summary": "Pull Defias patrols carefully."
    }
  }
})json";

TEST(WorldKnowledgeTest, OutleveledZoneAndActiveQuests) {
  std::string err;
  const std::optional<WorldKnowledge> wk =
      WorldKnowledge::ParseJson(kSampleKnowledgeJson, &err);
  ASSERT_TRUE(wk.has_value()) << err;
  EXPECT_EQ(wk->quest_count(), 2u);
  EXPECT_EQ(wk->zone_count(), 1u);
  EXPECT_EQ(wk->dungeon_count(), 1u);

  wlb::snapshot::Snapshot snap;
  snap.base.self.unit.level = 14;
  snap.base.where.zone = "Elwynn Forest";
  snap.base.where.in_instance = false;

  wlb::snapshot::Objective q_complete;
  q_complete.quest.emplace().quest_id = 7;
  q_complete.quest->complete = true;
  snap.ext.quest_tracker.push_back(q_complete);

  wlb::snapshot::Objective q_open;
  q_open.quest.emplace().quest_id = 60;
  q_open.quest->complete = false;
  snap.ext.quest_tracker.push_back(q_open);

  const std::string md = wk->SelectRelevant(snap, /*has_boss_notes=*/false);
  EXPECT_NE(md.find("Zone: Elwynn Forest (levels 1-10), Alliance"),
            std::string::npos);
  EXPECT_NE(md.find("Outleveled zone: you are level 14; next zones: Westfall "
                    "(10-20), Loch Modan (10-20)"),
            std::string::npos);
  EXPECT_NE(md.find("Kobold Candles"), std::string::npos);
  EXPECT_NE(md.find("Drops from: Kobold Miner, Kobold Geomancer."),
            std::string::npos);
  EXPECT_NE(md.find("Kobold Camp Cleanup"), std::string::npos);
  EXPECT_NE(md.find("turn in to Marshal McBride."), std::string::npos);
}

TEST(WorldKnowledgeTest, DungeonFallbackSuppressedWhenBossNotesExist) {
  const std::optional<WorldKnowledge> wk =
      WorldKnowledge::ParseJson(kSampleKnowledgeJson);
  ASSERT_TRUE(wk.has_value());

  wlb::snapshot::Snapshot snap;
  snap.base.where.in_instance = true;
  snap.base.where.zone = "Deadmines";

  const std::string without_notes =
      wk->SelectRelevant(snap, /*has_boss_notes=*/false);
  EXPECT_NE(without_notes.find("Instance: Deadmines (levels 15-21)"),
            std::string::npos);
  EXPECT_NE(without_notes.find("Pull Defias patrols carefully."),
            std::string::npos);

  const std::string with_notes =
      wk->SelectRelevant(snap, /*has_boss_notes=*/true);
  EXPECT_EQ(with_notes.find("Instance: Deadmines"), std::string::npos);
}

TEST(WorldKnowledgeTest, RejectsInvalidJson) {
  std::string err;
  EXPECT_FALSE(WorldKnowledge::ParseJson("{not json", &err).has_value());
  EXPECT_FALSE(err.empty());
}

}  // namespace
}  // namespace harness
