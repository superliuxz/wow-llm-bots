// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

TEST(BehaviorTraceTest, FormatsSampleAndChatWithJsonEscaping) {
  wlb::snapshot::Snapshot snap;
  snap.bot = "Loada";
  snap.time_ms = 1795064000000LL;
  snap.monotonic_us = 123456;
  snap.base.where.zone = "Durotar";
  snap.base.where.position = {-668.5f, -4282.25f, 41.0f};
  snap.base.where.facing = 1.571f;
  snap.base.self.in_combat = true;
  snap.base.self.health = 75;
  snap.base.self.health_max = 100;
  snap.base.self.power = 40;
  snap.base.self.power_max = 80;
  snap.base.self.casting.emplace().spell.emplace().name = "Smite";
  snap.ext.target.emplace().target.emplace().name = "Mottled Boar";
  snap.ext.combat.emplace().attackers.emplace_back().name = "Mottled Boar";

  const TraceSample s = TraceSampleFromSnapshot(
      snap, "bot", /*map_id=*/1, /*moving=*/true, /*jumping=*/false,
      /*in_town=*/false, /*in_group=*/true, /*in_dungeon=*/true);
  const std::string line = FormatTraceSample(s);
  EXPECT_NE(line.find("\"kind\":\"sample\""), std::string::npos);
  EXPECT_NE(line.find("\"role\":\"bot\""), std::string::npos);
  EXPECT_NE(line.find("\"name\":\"Loada\""), std::string::npos);
  EXPECT_NE(line.find("\"in_group\":true"), std::string::npos);
  EXPECT_NE(line.find("\"in_dungeon\":true"), std::string::npos);
  EXPECT_NE(line.find("\"casting\":\"Smite\""), std::string::npos);
  EXPECT_NE(line.find("\"target\":\"Mottled Boar\""), std::string::npos);
  EXPECT_NE(line.find("\"attackers\":1"), std::string::npos);

  TraceEvent ev;
  ev.time_ms = 1795064002000LL;
  ev.monotonic_us = 323456;
  ev.role = "human";
  ev.name = "Hero";
  ev.event = "damage_taken";
  ev.detail = "Mottled Boar";
  ev.value = 17;
  const std::string ev_line = FormatTraceEvent(ev);
  EXPECT_NE(ev_line.find("\"kind\":\"event\""), std::string::npos);
  EXPECT_NE(ev_line.find("\"event\":\"damage_taken\""), std::string::npos);
  EXPECT_NE(ev_line.find("\"detail\":\"Mottled Boar\""), std::string::npos);
  EXPECT_NE(ev_line.find("\"value\":17"), std::string::npos);

  TraceChat c;
  c.time_ms = 1795064001000LL;
  c.monotonic_us = 223456;
  c.role = "human";
  c.name = "Hero";
  c.direction = "out";
  c.channel = "whisper";
  c.peer = "Loada";
  c.text = "hey \"friend\"\nready?";
  const std::string chat_line = FormatTraceChat(c);
  EXPECT_NE(chat_line.find("\"kind\":\"chat\""), std::string::npos);
  EXPECT_NE(chat_line.find("\"role\":\"human\""), std::string::npos);
  EXPECT_NE(chat_line.find("hey \\\"friend\\\"\\nready?"), std::string::npos);
  EXPECT_NE(chat_line.find("\"text_len\":19"), std::string::npos);

  TraceChat redacted;
  redacted.role = "human";
  redacted.name = "Hero";
  redacted.direction = "out";
  redacted.channel = "whisper";
  redacted.peer = "Friend";
  redacted.text_len = 42;
  const std::string red_line = FormatTraceChat(redacted);
  EXPECT_NE(red_line.find("\"text\":\"\""), std::string::npos);
  EXPECT_NE(red_line.find("\"text_len\":42"), std::string::npos);
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

TEST(BehaviorTraceTest, RotatesFileWhenExceedingMaxBytes) {
  const std::filesystem::path root =
      std::filesystem::path(testing::TempDir()) / "wlb_trace_rotate_test";
  std::filesystem::remove_all(root);
  // Tiny cap so the second batch triggers rotation.
  BehaviorTraceWriter writer(root.string(), /*max_file_bytes=*/50);
  TraceSample s;
  s.time_ms = 1795064000000LL;
  s.role = "bot";
  s.name = "Loada";
  ASSERT_TRUE(writer.WriteSample(s));
  writer.Flush();
  ASSERT_TRUE(writer.WriteSample(s));
  writer.Flush();

  const std::filesystem::path active =
      root / "bot" / "Loada" / "2026-11-19.jsonl";
  const std::filesystem::path rotated =
      root / "bot" / "Loada" / "2026-11-19.1.jsonl";
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_TRUE(std::filesystem::exists(rotated));
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace llm_brain
