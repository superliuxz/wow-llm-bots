// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "runtime/replay.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "gtest/gtest.h"
#include "llm/llm_client.h"
#include "runtime/file_util.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace fs = std::filesystem;
namespace pb = wlb::snapshot;

TEST(ReplayTest, RenderScreenGoldensMatchesCommittedScreens) {
  std::string report;
  const std::string dir = std::string(WLB_HARNESS_TESTDATA) + "/screens";
  EXPECT_EQ(RenderScreenGoldens(dir, /*rewrite=*/false, &report), 0) << report;
  EXPECT_NE(report.find("0 differ"), std::string::npos) << report;
}

TEST(ReplayTest, RunReplayCapacityAndLoadWithMockCaller) {
  const fs::path tmp = fs::temp_directory_path() / "wlb_replay_test";
  fs::remove_all(tmp);
  const fs::path bot_dir = tmp / "Loada";
  fs::create_directories(bot_dir);
  const fs::path rules_path = tmp / "RULES.md";
  ASSERT_TRUE(WriteFileAtomic(rules_path.string(), "Follow the reply form.\n"));

  for (int seq = 1; seq <= 6; ++seq) {
    pb::Snapshot snap;
    snap.bot = "Loada";
    snap.seq = static_cast<uint32_t>(seq);
    snap.time_ms = seq * 10;
    snap.base.self.unit.name = "Loada";
    snap.base.self.state = pb::Self::ALIVE;
    snap.base.self.health = 100;
    snap.base.self.health_max = 100;
    snap.base.where.zone = "Durotar";
    ASSERT_TRUE(
        WriteFileAtomic((bot_dir / (std::to_string(seq) + ".json")).string(),
                        pb::WriteJson(snap)));
  }

  const base::RepeatingCallback<LlmResult(const LlmRequest&)> mock_call =
      [](const LlmRequest&) {
        LlmResult r;
        r.reply.emplace().text = "do: 1\nexpect: none\nwatch: none\nlook: 60";
        r.first_line_ms = 10.0;
        r.round_trip_ms = 20.0;
        return r;
      };

  ReplayOptions cap;
  cap.snapshot_dir = tmp.string();
  cap.rules_path = rules_path.string();
  cap.mode = "capacity";
  cap.slots = 2;
  cap.plan_s = 0.01;
  cap.warmup_s = 0.0;
  cap.seconds = 0.05;
  std::string report;
  EXPECT_EQ(RunReplay(cap, mock_call, &report), 0) << report;
  EXPECT_NE(report.find("2 workers:"), std::string::npos) << report;

  ReplayOptions load = cap;
  load.mode = "load";
  load.bots = 2;
  load.seconds = 0.10;
  EXPECT_EQ(RunReplay(load, mock_call, &report), 0) << report;
  EXPECT_NE(report.find("wake->plan p50"), std::string::npos) << report;

  fs::remove_all(tmp);
}

}  // namespace
}  // namespace harness
