// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/plan_runner.h"

#include <cstdint>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "screen/screen_renderer.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

pb::Snapshot MakeSnap(uint32_t seq, bool in_combat = false) {
  pb::Snapshot snap;
  snap.seq = seq;
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.self.in_combat = in_combat;
  snap.base.self.health = 60;
  snap.base.self.health_max = 60;
  return snap;
}

void AddBoar(pb::Snapshot* snap,
             const std::string& guid,
             bool dead = false,
             bool lootable = false) {
  pb::Unit& u = snap->base.nearby.units.emplace_back();
  u.guid = guid;
  u.name = "Mottled Boar";
  u.kind = pb::Unit::NPC;
  u.reaction = pb::Unit::NEUTRAL;
  u.dead = dead;
  u.lootable = lootable;
  u.rel.dist_yd = 6.0f;
}

void AddModuleOption(pb::Snapshot* snap,
                     pb::Option::Kind kind,
                     const std::string& text,
                     const std::string& guid = "",
                     const std::string& name = "") {
  pb::Option& opt = snap->options.emplace_back();
  opt.text = text;
  opt.guid = guid;
  opt.kind = kind;
  opt.name = name;
}

TEST(PlanRunnerTest, RepeatsReResolveAgainstFreshSnapshotsAndThenLoot) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);

  // Snapshot 10: two live boars (0x1 nearest, 0x2 second).
  pb::Snapshot s10 = MakeSnap(10);
  AddBoar(&s10, "0x1");
  AddBoar(&s10, "0x2");
  AddModuleOption(&s10, pb::Option::ATTACK, "Attack Mottled Boar", "0x1",
                  "Mottled Boar");
  AddModuleOption(&s10, pb::Option::KEEP, "Keep doing what I'm doing");
  const Screen sc10 = RenderScreen(s10);

  PlanRunner runner;
  // Plan: attack 2x Mottled Boar (`1x2`).
  const RunnerAction a1 = runner.Start({{1, 2}}, 10, sc10, t0);
  ASSERT_TRUE(a1.choose);
  EXPECT_EQ(a1.choose->seq, 10u);
  EXPECT_EQ(a1.choose->option, 1u);
  EXPECT_TRUE(a1.finished_reason.empty());

  // Snapshot 11: fighting 0x1 (still alive) -> nothing new sent yet.
  pb::Snapshot s11 = MakeSnap(11, /*in_combat=*/true);
  AddBoar(&s11, "0x1");
  AddBoar(&s11, "0x2");
  AddModuleOption(&s11, pb::Option::ATTACK, "Attack Mottled Boar", "0x1",
                  "Mottled Boar");
  AddModuleOption(&s11, pb::Option::KEEP, "Keep doing what I'm doing");
  const RunnerAction a2 =
      runner.OnSnapshot(s11, RenderScreen(s11), t0 + base::Seconds(2));
  EXPECT_FALSE(a2.choose);
  EXPECT_TRUE(a2.finished_reason.empty());

  // Snapshot 12: 0x1 died (now option 1 = Loot 0x1, option 2 = Attack 0x2!).
  pb::Snapshot s12 = MakeSnap(12, /*in_combat=*/false);
  AddBoar(&s12, "0x1", /*dead=*/true, /*lootable=*/true);
  AddBoar(&s12, "0x2");
  AddModuleOption(&s12, pb::Option::LOOT, "Loot Mottled Boar", "0x1",
                  "Mottled Boar");
  AddModuleOption(&s12, pb::Option::ATTACK, "Attack Mottled Boar", "0x2",
                  "Mottled Boar");
  AddModuleOption(&s12, pb::Option::KEEP, "Keep doing what I'm doing");
  const RunnerAction a3 =
      runner.OnSnapshot(s12, RenderScreen(s12), t0 + base::Seconds(5));
  // Repeat #2 re-resolves "Attack Mottled Boar" to option 2 of snapshot 12!
  ASSERT_TRUE(a3.choose);
  EXPECT_EQ(a3.choose->seq, 12u);
  EXPECT_EQ(a3.choose->option, 2u);

  // Snapshot 13: 0x2 also died -> 2x2 finished, wakes brain with "plan done".
  pb::Snapshot s13 = MakeSnap(13, /*in_combat=*/false);
  AddBoar(&s13, "0x1", /*dead=*/true, /*lootable=*/true);
  AddBoar(&s13, "0x2", /*dead=*/true, /*lootable=*/true);
  AddModuleOption(&s13, pb::Option::LOOT, "Loot Mottled Boar", "0x1",
                  "Mottled Boar");
  AddModuleOption(&s13, pb::Option::KEEP, "Keep doing what I'm doing");
  const RunnerAction a4 =
      runner.OnSnapshot(s13, RenderScreen(s13), t0 + base::Seconds(9));
  EXPECT_FALSE(a4.choose);
  EXPECT_EQ(a4.finished_reason, "plan done");
  EXPECT_FALSE(runner.active());
}

TEST(PlanRunnerTest, KeepStaysActiveUntilLookTimerWithoutFinishingEarly) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  pb::Snapshot s1 = MakeSnap(1);
  AddModuleOption(&s1, pb::Option::KEEP, "Keep doing what I'm doing");
  const Screen sc1 = RenderScreen(s1);

  PlanRunner runner;
  const RunnerAction a1 = runner.Start({{1, 1}}, 1, sc1, t0);
  ASSERT_TRUE(a1.choose);
  EXPECT_EQ(a1.choose->option, 1u);

  // Even 120 s later, `Keep` does not self-finish or time out on its own.
  const RunnerAction a2 = runner.OnSnapshot(s1, sc1, t0 + base::Seconds(120));
  EXPECT_FALSE(a2.choose);
  EXPECT_TRUE(a2.finished_reason.empty());
  EXPECT_TRUE(runner.active());
}

TEST(PlanRunnerTest, NeverSendsChooseWhenScreenLacksModuleOptions) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  pb::Snapshot offline = MakeSnap(1);
  AddBoar(&offline, "0x1");
  const Screen sc = RenderScreen(offline);
  ASSERT_FALSE(sc.has_module_options);

  PlanRunner runner;
  const RunnerAction a = runner.Start({{1, 1}}, 1, sc, t0);
  EXPECT_FALSE(a.choose);
  EXPECT_TRUE(runner.active());
}

TEST(PlanRunnerTest, StalledAttackTimesOutAfterSafetyLimit) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  pb::Snapshot s1 = MakeSnap(1, /*in_combat=*/true);
  AddBoar(&s1, "0x1");
  AddModuleOption(&s1, pb::Option::ATTACK, "Attack Mottled Boar", "0x1",
                  "Mottled Boar");
  AddModuleOption(&s1, pb::Option::KEEP, "Keep doing what I'm doing");
  const Screen sc1 = RenderScreen(s1);

  PlanRunner runner;
  ASSERT_TRUE(runner.Start({{1, 1}}, 1, sc1, t0).choose);
  EXPECT_FALSE(runner.CheckTimeout(t0 + base::Seconds(44)));
  EXPECT_EQ(runner.CheckTimeout(t0 + base::Seconds(45)), "step timed out");
  EXPECT_FALSE(runner.active());
}

TEST(PlanRunnerTest, ActiveAttackMakingProgressDoesNotTimeOutAt45s) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  pb::Snapshot s1 = MakeSnap(1, /*in_combat=*/true);
  AddBoar(&s1, "0x1");
  s1.base.nearby.units[0].health_pct = 100.0f;
  AddModuleOption(&s1, pb::Option::ATTACK, "Attack Mottled Boar", "0x1",
                  "Mottled Boar");
  AddModuleOption(&s1, pb::Option::KEEP, "Keep doing what I'm doing");

  PlanRunner runner;
  ASSERT_TRUE(runner.Start({{1, 1}}, 1, RenderScreen(s1), t0).choose);
  // Target HP drops every 20 s during a long boss fight: never times out.
  for (int step = 1; step <= 4; ++step) {
    pb::Snapshot sn = s1;
    sn.seq = static_cast<uint32_t>(step + 1);
    sn.base.nearby.units[0].health_pct = 100.0f - 10.0f * step;
    const RunnerAction a =
        runner.OnSnapshot(sn, RenderScreen(sn), t0 + base::Seconds(20 * step));
    EXPECT_TRUE(a.finished_reason.empty());
    EXPECT_TRUE(runner.active());
  }
}

// Playerbots leaves items it has no use for, so the corpse stays lootable: the
// step ends when the module says the body finished its command.
TEST(PlanRunnerTest, LootEndsOnTheBodysDoneSignalWhileTheCorpseIsLootable) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  pb::Snapshot s1 = MakeSnap(1);
  AddBoar(&s1, "0x1", /*dead=*/true, /*lootable=*/true);
  AddModuleOption(&s1, pb::Option::LOOT, "Loot Mottled Boar", "0x1",
                  "Mottled Boar");
  AddModuleOption(&s1, pb::Option::KEEP, "Keep doing what I'm doing");

  PlanRunner runner;
  const RunnerAction a1 = runner.Start({{1, 1}}, 1, RenderScreen(s1), t0);
  ASSERT_TRUE(a1.choose);
  runner.set_last_command_id(7);

  // Still lootable, and the body hasn't finished: carry on.
  pb::Snapshot s2 = s1;
  s2.seq = 2;
  s2.done_command_id = 6;
  EXPECT_TRUE(runner.OnSnapshot(s2, RenderScreen(s2), t0 + base::Seconds(1))
                  .finished_reason.empty());
  EXPECT_TRUE(runner.active());

  // Finished (items left on it): the plan is done.
  pb::Snapshot s3 = s1;
  s3.seq = 3;
  s3.done_command_id = 7;
  const RunnerAction a3 =
      runner.OnSnapshot(s3, RenderScreen(s3), t0 + base::Seconds(2));
  EXPECT_TRUE(a3.step_done);
  EXPECT_EQ(a3.finished_reason, "plan done");
}

TEST(PlanRunnerTest, MultipleExploreAndGoToOptionsDoNotCollideAcrossSnapshots) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  pb::Snapshot s1 = MakeSnap(1);
  AddModuleOption(&s1, pb::Option::REST, "Rest (eat and drink)");
  AddModuleOption(&s1, pb::Option::EXPLORE,
                  "Explore NE, 300 yd (boars, level 1-2)", "spot:1:-500:-4100",
                  "boars, level 1-2");
  AddModuleOption(&s1, pb::Option::EXPLORE,
                  "Explore S, 220 yd (scorpids, level 3-5)",
                  "spot:1:-700:-4300", "scorpids, level 3-5");
  AddModuleOption(&s1, pb::Option::GO_TO, "Go to The Den (innkeeper), 150 yd W",
                  "place:the_den", "The Den");
  AddModuleOption(&s1, pb::Option::GO_TO,
                  "Go to Razor Hill (vendor, repair), 900 yd N",
                  "place:razor_hill", "Razor Hill");
  AddModuleOption(&s1, pb::Option::KEEP, "Keep doing what I'm doing");

  PlanRunner runner;
  // Plan: Rest (1), then Explore S (3), then Go to Razor Hill (5).
  const RunnerAction a1 =
      runner.Start({{1, 1}, {3, 1}, {5, 1}}, 1, RenderScreen(s1), t0);
  ASSERT_TRUE(a1.choose);
  EXPECT_EQ(a1.choose->option, 1u);
  runner.set_last_command_id(10);

  // Rest finishes on s2; distances and directions shifted while resting.
  pb::Snapshot s2 = MakeSnap(2);
  s2.done_command_id = 10;
  AddModuleOption(&s2, pb::Option::EXPLORE,
                  "Explore N, 290 yd (boars, level 1-2)", "spot:1:-500:-4100",
                  "boars, level 1-2");
  AddModuleOption(&s2, pb::Option::EXPLORE,
                  "Explore SE, 210 yd (scorpids, level 3-5)",
                  "spot:1:-700:-4300", "scorpids, level 3-5");
  AddModuleOption(&s2, pb::Option::GO_TO, "Go to The Den (innkeeper), 140 yd W",
                  "place:the_den", "The Den");
  AddModuleOption(&s2, pb::Option::GO_TO,
                  "Go to Razor Hill (vendor, repair), 890 yd N",
                  "place:razor_hill", "Razor Hill");
  AddModuleOption(&s2, pb::Option::KEEP, "Keep doing what I'm doing");
  const RunnerAction a2 =
      runner.OnSnapshot(s2, RenderScreen(s2), t0 + base::Seconds(15));
  ASSERT_TRUE(a2.choose);
  // Picks the second Explore option (scorpids), not the first (boars)!
  EXPECT_EQ(a2.choose->option, 2u);
  runner.set_last_command_id(11);

  // Explore finishes on s3; advances to Go to Razor Hill (option 4 on s3).
  pb::Snapshot s3 = s2;
  s3.seq = 3;
  s3.done_command_id = 11;
  const RunnerAction a3 =
      runner.OnSnapshot(s3, RenderScreen(s3), t0 + base::Seconds(60));
  ASSERT_TRUE(a3.choose);
  EXPECT_EQ(a3.choose->option, 4u);
}

}  // namespace
}  // namespace harness
