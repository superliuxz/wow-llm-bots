// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

TEST(BehaviorTraceTest, FormatsSampleAndEventWithJsonEscaping) {
  wlb::snapshot::Snapshot snap;
  snap.bot = "Loada";
  snap.time_ms = 1795064000000LL;
  snap.monotonic_us = 123456;
  snap.base.where.zone = "Durotar";
  snap.base.where.position = {-668.5f, -4282.25f, 41.0f};
  snap.base.self.in_combat = true;
  snap.base.self.health = 75;
  snap.base.self.health_max = 100;
  snap.base.self.power = 40;
  snap.base.self.power_max = 80;
  snap.base.self.casting.emplace().spell.emplace().name = "Smite";
  auto& tgt = snap.ext.target.emplace().target.emplace();
  tgt.name = "Mottled Boar";
  tgt.rel.dist_yd = 12.5f;
  tgt.health_pct = 80.0f;
  tgt.level = 2;
  tgt.activity.cast.emplace().spell.emplace().name = "Charge";
  tgt.reaction = wlb::snapshot::Unit::HOSTILE;
  tgt.targeting_me = true;

  const TraceSample s = TraceSampleFromSnapshot(
      snap, "bot", /*map_id=*/1, /*moving=*/true, /*in_dungeon=*/true);
  const std::string line = FormatTraceSample(s);
  EXPECT_NE(line.find("\"kind\":\"sample\""), std::string::npos);
  EXPECT_NE(line.find("\"role\":\"bot\""), std::string::npos);
  EXPECT_NE(line.find("\"name\":\"Loada\""), std::string::npos);
  EXPECT_NE(line.find("\"in_dungeon\":true"), std::string::npos);
  EXPECT_NE(line.find("\"casting\":\"Smite\""), std::string::npos);
  EXPECT_NE(line.find("\"target\":\"Mottled Boar\""), std::string::npos);
  EXPECT_NE(line.find("\"target_dist\":12.50"), std::string::npos);
  EXPECT_NE(line.find("\"target_hp_pct\":80.0"), std::string::npos);
  EXPECT_NE(line.find("\"target_level\":2"), std::string::npos);
  EXPECT_NE(line.find("\"target_casting\":\"Charge\""), std::string::npos);
  EXPECT_NE(line.find("\"target_targeting_me\":true"), std::string::npos);

  TraceEvent ev;
  ev.time_ms = 1795064002000LL;
  ev.monotonic_us = 323456;
  ev.role = "human";
  ev.name = "Hero";
  ev.event = "damage_taken";
  ev.detail = "Mottled \"Boar\"\nBoss";
  ev.value = 17;
  const std::string ev_line = FormatTraceEvent(ev);
  EXPECT_NE(ev_line.find("\"kind\":\"event\""), std::string::npos);
  EXPECT_NE(ev_line.find("\"event\":\"damage_taken\""), std::string::npos);
  EXPECT_NE(ev_line.find("Mottled \\\"Boar\\\"\\nBoss"), std::string::npos);
  EXPECT_NE(ev_line.find("\"value\":17"), std::string::npos);
}

TEST(BehaviorTraceTest, WritesUnifiedRoleDirectories) {
  const std::filesystem::path root =
      std::filesystem::path(testing::TempDir()) / "wlb_trace_writer_test";
  std::filesystem::remove_all(root);
  BehaviorTraceWriter writer(root.string());
  ASSERT_TRUE(writer.enabled());

  TraceSample s;
  s.time_ms = 1795064000000LL;  // 2026-11-19 UTC
  s.role = "human";
  s.name = "PlayerOne";
  ASSERT_TRUE(writer.WriteSample(s));
  writer.Flush();

  const std::filesystem::path expected =
      root / "human" / "PlayerOne" / "2026-11-19.jsonl";
  ASSERT_TRUE(std::filesystem::exists(expected));
  std::ifstream in(expected);
  std::string content;
  std::getline(in, content);
  EXPECT_NE(content.find("\"name\":\"PlayerOne\""), std::string::npos);
  std::filesystem::remove_all(root);
}

TEST(BehaviorTraceTest, FormatsDirectProductionTraceSampleFields) {
  TraceSample s;
  s.time_ms = 1795064000000LL;
  s.monotonic_us = 999999;
  s.role = "bot";
  s.name = "Loada";
  s.map_id = 602;
  s.zone = "Halls of Lightning";
  s.in_dungeon = true;
  s.x = 1186.5f;
  s.y = 56.0f;
  s.z = 60.8f;
  s.moving = true;
  s.in_combat = true;
  s.level = 80;
  s.hp_pct = 92.5f;
  s.mana_pct = 81.0f;
  s.target = "Loken";
  s.target_dist = 24.5f;
  s.target_x = 1186.5f;
  s.target_y = 33.8f;
  s.target_hp_pct = 63.0f;
  s.target_level = 82;
  s.target_casting = "Lightning Nova";
  s.target_cast_radius = 20.0f;
  const std::string line = FormatTraceSample(s);
  EXPECT_NE(line.find("\"target\":\"Loken\""), std::string::npos);
  EXPECT_NE(line.find("\"target_dist\":24.5"), std::string::npos);
  EXPECT_NE(line.find("\"target_x\":1186.5"), std::string::npos);
  EXPECT_NE(line.find("\"target_cast_radius\":20.0"), std::string::npos);
  EXPECT_NE(line.find("\"target_casting\":\"Lightning Nova\""),
            std::string::npos);
  EXPECT_NE(line.find("\"in_dungeon\":true"), std::string::npos);
}

TEST(BehaviorTraceTest, RotatesFileWhenExceedingMaxBytes) {
  const std::filesystem::path root =
      std::filesystem::path(testing::TempDir()) / "wlb_trace_rotate_test";
  std::filesystem::remove_all(root);
  BehaviorTraceWriter writer(root.string(), /*max_file_bytes=*/50);
  TraceSample s;
  s.time_ms = 1795064000000LL;
  s.role = "bot";
  s.name = "Loada";
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(writer.WriteSample(s));
    writer.Flush();
  }

  const std::filesystem::path active =
      root / "bot" / "Loada" / "2026-11-19.jsonl";
  const std::filesystem::path rot1 =
      root / "bot" / "Loada" / "2026-11-19.1.jsonl";
  const std::filesystem::path rot2 =
      root / "bot" / "Loada" / "2026-11-19.2.jsonl";
  const std::filesystem::path rot3 =
      root / "bot" / "Loada" / "2026-11-19.3.jsonl";
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_TRUE(std::filesystem::exists(rot1));
  EXPECT_TRUE(std::filesystem::exists(rot2));
  EXPECT_TRUE(std::filesystem::exists(rot3));
  std::filesystem::remove_all(root);
}

TEST(BehaviorTraceTest, RotationKeepsAtMostConfiguredArchives) {
  const std::filesystem::path root =
      std::filesystem::path(testing::TempDir()) / "wlb_trace_rotate_cap_test";
  std::filesystem::remove_all(root);
  const std::filesystem::path dir = root / "bot" / "Loada";
  const auto archive = [&](int idx) {
    return dir / ("2026-11-19." + std::to_string(idx) + ".jsonl");
  };
  TraceSample s;
  s.time_ms = 1795064000000LL;
  s.role = "bot";
  s.name = "Loada";
  {
    BehaviorTraceWriter writer(root.string(), /*max_file_bytes=*/50);
    for (int i = 0; i < 6; ++i) {
      ASSERT_TRUE(writer.WriteSample(s));
      writer.Flush();
    }
  }
  ASSERT_TRUE(std::filesystem::exists(archive(5)));
  {
    BehaviorTraceWriter writer(root.string(), /*max_file_bytes=*/50,
                               /*max_rotated_files=*/2);
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(writer.WriteSample(s));
      writer.Flush();
    }
  }
  EXPECT_TRUE(std::filesystem::exists(dir / "2026-11-19.jsonl"));
  EXPECT_TRUE(std::filesystem::exists(archive(1)));
  EXPECT_TRUE(std::filesystem::exists(archive(2)));
  for (int idx = 3; idx <= 8; ++idx) {
    EXPECT_FALSE(std::filesystem::exists(archive(idx))) << idx;
  }
  {
    BehaviorTraceWriter writer(root.string(), /*max_file_bytes=*/50,
                               /*max_rotated_files=*/0);
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(writer.WriteSample(s));
      writer.Flush();
    }
  }
  EXPECT_TRUE(std::filesystem::exists(dir / "2026-11-19.jsonl"));
  EXPECT_FALSE(std::filesystem::exists(archive(1)));
  EXPECT_FALSE(std::filesystem::exists(archive(2)));
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace llm_brain
