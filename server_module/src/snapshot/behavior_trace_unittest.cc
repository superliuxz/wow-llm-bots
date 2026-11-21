// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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
  auto& tgt = snap.ext.target.emplace().target.emplace();
  tgt.name = "Mottled Boar";
  tgt.rel.dist_yd = 12.5f;
  tgt.health_pct = 80.0f;
  tgt.level = 2;
  tgt.activity.cast.emplace().spell.emplace().name = "Charge";
  tgt.activity.state = wlb::snapshot::Activity::MOVING;
  tgt.reaction = wlb::snapshot::Unit::HOSTILE;
  tgt.targeting_me = true;
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
  EXPECT_NE(line.find("\"target_dist\":12.50"), std::string::npos);
  EXPECT_NE(line.find("\"target_hp_pct\":80.0"), std::string::npos);
  EXPECT_NE(line.find("\"target_level\":2"), std::string::npos);
  EXPECT_NE(line.find("\"target_casting\":\"Charge\""), std::string::npos);
  EXPECT_NE(line.find("\"target_moving\":true"), std::string::npos);
  EXPECT_NE(line.find("\"target_hostile\":true"), std::string::npos);
  EXPECT_NE(line.find("\"target_targeting_me\":true"), std::string::npos);
  EXPECT_NE(line.find("\"target_in_combat\":true"), std::string::npos);
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

TEST(BehaviorTraceTest, FormatsDirectProductionTraceSampleFields) {
  TraceSample s;
  s.time_ms = 1795064000000LL;
  s.monotonic_us = 999999;
  s.role = "bot";
  s.name = "Loada";
  s.map_id = 602;
  s.zone = "Halls of Lightning";
  s.in_group = true;
  s.in_dungeon = true;
  s.x = 1186.5f;
  s.y = 56.0f;
  s.z = 60.8f;
  s.moving = true;
  s.in_combat = true;
  s.level = 80;
  s.hp_pct = 92.5f;
  s.mana_pct = 81.0f;
  s.bag_free_slots = 14;
  s.target = "Loken";
  s.target_dist = 24.5f;
  s.target_x = 1186.5f;
  s.target_y = 33.8f;
  s.target_hp_pct = 63.0f;
  s.target_level = 82;
  s.target_casting = "Lightning Nova";
  s.target_cast_radius = 20.0f;
  s.target_hostile = true;
  s.target_in_combat = true;
  const std::string line = FormatTraceSample(s);
  EXPECT_NE(line.find("\"target\":\"Loken\""), std::string::npos);
  EXPECT_NE(line.find("\"target_dist\":24.5"), std::string::npos);
  EXPECT_NE(line.find("\"target_x\":1186.5"), std::string::npos);
  EXPECT_NE(line.find("\"target_cast_radius\":20.0"), std::string::npos);
  EXPECT_NE(line.find("\"target_casting\":\"Lightning Nova\""),
            std::string::npos);
  EXPECT_NE(line.find("\"bag_free_slots\":14"), std::string::npos);
  EXPECT_NE(line.find("\"in_dungeon\":true"), std::string::npos);
}

TEST(BehaviorTraceTest, RotatesFileWhenExceedingMaxBytes) {
  const std::filesystem::path root =
      std::filesystem::path(testing::TempDir()) / "wlb_trace_rotate_test";
  std::filesystem::remove_all(root);
  // Tiny cap so every subsequent batch triggers rotation (default archive
  // count, kDefaultMaxRotatedFiles).
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
    // Start with 5 archives under the default cap...
    BehaviorTraceWriter writer(root.string(), /*max_file_bytes=*/50);
    for (int i = 0; i < 6; ++i) {
      ASSERT_TRUE(writer.WriteSample(s));
      writer.Flush();
    }
  }
  ASSERT_TRUE(std::filesystem::exists(archive(5)));
  {
    // ...then lower the cap to 2: the next rotation keeps .1 and .2 only.
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
    // 0 archives: the day file is truncated in place, never renamed.
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

TEST(BehaviorTraceTest, MeasuresActualSampleHzIncludingTickRounding) {
  const std::filesystem::path root =
      std::filesystem::path(testing::TempDir()) / "wlb_trace_hz_test";
  std::filesystem::remove_all(root);
  BehaviorTraceWriter writer(root.string());
  ASSERT_TRUE(writer.enabled());

  TraceSample s;
  s.role = "bot";
  s.name = "Loada";
  const int64_t t0 = 1795064000000LL;
  // Simulate TraceSampleIntervalMs = 285 rounded to 50 ms world ticks -> 300 ms
  // (3.33 Hz < 3.5 Hz parity floor), plus a 0 ms combat-edge duplicate and a
  // 10000 ms gap that must both be ignored (matching trace_parity.py).
  for (int i = 0; i <= 12; ++i) {
    s.time_ms = t0 + static_cast<int64_t>(i) * 300;
    ASSERT_TRUE(writer.WriteSample(s));
    if (i == 4) {
      ASSERT_TRUE(writer.WriteSample(s));  // dt == 0 duplicate
    }
  }
  s.time_ms = t0 + 12LL * 300 + 10000;
  ASSERT_TRUE(writer.WriteSample(s));  // > 5000 ms gap ignored

  // Insufficient samples preserves the window instead of clearing it.
  EXPECT_FALSE(
      writer.TakeMeasuredSampleHz("bot", /*min_intervals=*/20).has_value());
  const std::optional<double> rounded_hz =
      writer.TakeMeasuredSampleHz("bot", /*min_intervals=*/10);
  ASSERT_TRUE(rounded_hz.has_value());
  EXPECT_DOUBLE_EQ(*rounded_hz, 3.33);
  EXPECT_FALSE(
      writer.TakeMeasuredSampleHz("bot", /*min_intervals=*/1).has_value());
  EXPECT_FALSE(
      writer.TakeMeasuredSampleHz("human", /*min_intervals=*/1).has_value());

  // 200 ms effective cadence on bots vs 300 ms on a human must stay isolated
  // per role so 10 fast bots cannot mask a slow human trace.
  const int64_t t1 = s.time_ms + 1000;
  for (int b = 0; b < 10; ++b) {
    s.role = "bot";
    s.name = "Bot" + std::to_string(b);
    for (int i = 0; i <= 12; ++i) {
      s.time_ms = t1 + static_cast<int64_t>(i) * 200;
      ASSERT_TRUE(writer.WriteSample(s));
    }
  }
  s.role = "human";
  s.name = "Hero";
  for (int i = 0; i <= 12; ++i) {
    s.time_ms = t1 + static_cast<int64_t>(i) * 300;
    ASSERT_TRUE(writer.WriteSample(s));
  }
  const std::optional<double> bot_hz =
      writer.TakeMeasuredSampleHz("bot", /*min_intervals=*/10);
  const std::optional<double> human_hz =
      writer.TakeMeasuredSampleHz("human", /*min_intervals=*/10);
  ASSERT_TRUE(bot_hz.has_value());
  ASSERT_TRUE(human_hz.has_value());
  EXPECT_DOUBLE_EQ(*bot_hz, 5.0);
  EXPECT_DOUBLE_EQ(*human_hz, 3.33);

  writer.Flush();
  std::filesystem::remove_all(root);
}

TEST(BehaviorTraceTest, EvictsIdleTrackedCharactersAndReportsCapOverflow) {
  const std::filesystem::path root =
      std::filesystem::path(testing::TempDir()) / "wlb_trace_evict_test";
  std::filesystem::remove_all(root);
  BehaviorTraceWriter writer(root.string(),
                             BehaviorTraceWriter::kDefaultMaxFileBytes,
                             BehaviorTraceWriter::kDefaultMaxRotatedFiles,
                             /*max_tracked_chars_per_role=*/4);
  ASSERT_TRUE(writer.enabled());

  TraceSample s;
  const int64_t t0 = 1795064000000LL;
  // Pass 1 eviction: after draining batch 0, advancing >5 s (kMaxSampleGapMs)
  // lets batch 1 replace those empty slots without reaching
  // kStaleTrackedCharMs.
  for (int b = 0; b < 4; ++b) {
    s.role = "bot";
    s.name = "DrainBot" + std::to_string(b);
    for (int i = 0; i <= 10; ++i) {
      s.time_ms = t0 + static_cast<int64_t>(i) * 200;
      ASSERT_TRUE(writer.WriteSample(s));
    }
  }
  const std::optional<double> batch0_hz =
      writer.TakeMeasuredSampleHz("bot", /*min_intervals=*/10);
  ASSERT_TRUE(batch0_hz.has_value());
  EXPECT_DOUBLE_EQ(*batch0_hz, 5.0);

  // 20 sequential batches (80 distinct bots > cap=4) spaced 10 s / 70 s apart.
  int64_t cur_t = t0 + 10000;
  for (int batch = 0; batch < 20; ++batch) {
    for (int b = 0; b < 4; ++b) {
      s.role = "bot";
      s.name = "SeqBot_" + std::to_string(batch) + "_" + std::to_string(b);
      for (int i = 0; i <= 10; ++i) {
        s.time_ms = cur_t + static_cast<int64_t>(i) * 200;
        ASSERT_TRUE(writer.WriteSample(s));
      }
    }
    cur_t += 70000;  // > kStaleTrackedCharMs so undrained batches also evict.
  }

  // Humans track in an independent per-role map and are unaffected by bot
  // turnover.
  s.role = "human";
  s.name = "Hero";
  for (int i = 0; i <= 10; ++i) {
    s.time_ms = cur_t + static_cast<int64_t>(i) * 300;
    ASSERT_TRUE(writer.WriteSample(s));
  }

  EXPECT_EQ(writer.TakeUntrackedSampleChars(), 0U);
  const std::optional<double> bot_hz =
      writer.TakeMeasuredSampleHz("bot", /*min_intervals=*/10);
  const std::optional<double> human_hz =
      writer.TakeMeasuredSampleHz("human", /*min_intervals=*/10);
  ASSERT_TRUE(bot_hz.has_value());
  ASSERT_TRUE(human_hz.has_value());
  EXPECT_DOUBLE_EQ(*bot_hz, 5.0);
  EXPECT_DOUBLE_EQ(*human_hz, 3.33);

  // Now advance past 60 s so previous empty entries evict, then exceed the
  // concurrent active cap (=4) at a single timestamp: must report overflow.
  const int64_t active_t = cur_t + 70000;
  for (int b = 0; b < 5; ++b) {
    s.role = "bot";
    s.name = "ActiveBot" + std::to_string(b);
    s.time_ms = active_t;
    ASSERT_TRUE(writer.WriteSample(s));
  }
  EXPECT_GT(writer.TakeUntrackedSampleChars(), 0U);
  EXPECT_EQ(writer.TakeUntrackedSampleChars(), 0U);

  writer.Flush();
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace llm_brain
