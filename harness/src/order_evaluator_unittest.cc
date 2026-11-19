// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "order_evaluator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/time/time.h"
#include "grammar/orders.wlb.h"
#include "orders.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

Order MustParse(std::string_view text) {
  std::optional<Order> parsed = ParseOrder(text);
  EXPECT_TRUE(parsed.has_value()) << "failed to parse order: " << text;
  return parsed.value_or(Order{});
}

pb::Snapshot MakeBaseSnapshot() {
  pb::Snapshot s;
  s.base.self.unit.guid = "Player-1";
  s.base.self.unit.name = "Aelindra";
  s.base.self.unit.health_pct = 100.0f;
  s.base.self.health = 10000;
  s.base.self.health_max = 10000;
  s.base.self.power_type = pb::Self::MANA;
  s.base.self.power = 5000;
  s.base.self.power_max = 10000;
  s.base.where.position = {0.0f, 0.0f, 0.0f};
  return s;
}

pb::Unit MakeUnit(std::string guid,
                  std::string name,
                  float dist_yd = 10.0f,
                  pb::Vec3 pos = {}) {
  pb::Unit u;
  u.guid = std::move(guid);
  u.name = std::move(name);
  u.rel.dist_yd = dist_yd;
  u.rel.position = pos;
  return u;
}

pb::Aura MakeAura(std::string name,
                  uint32_t stacks = 0,
                  std::optional<float> remaining_s = std::nullopt) {
  pb::Aura a;
  a.name = std::move(name);
  a.stacks = stacks;
  a.remaining_s = remaining_s;
  return a;
}

pb::GroupPart MakeGroup(pb::Role role, bool main_tank = false) {
  pb::GroupPart g;
  g.role = role;
  g.main_tank = main_tank;
  return g;
}

pb::Cast MakeCast(std::string spell_name, std::string target = {}) {
  pb::Spell sp;
  sp.name = std::move(spell_name);
  pb::Cast c;
  c.spell = std::move(sp);
  c.target = std::move(target);
  return c;
}

base::TimeTicks T(double seconds) {
  return base::TimeTicks() + base::Seconds(seconds);
}

TEST(OrderEvaluatorTest, AlwaysAndPriority) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("Defile on me -> away"),
                  MustParse("always -> to behind boss")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 1);
  EXPECT_EQ(r.step_index, 0);
  EXPECT_TRUE(r.covered);
  EXPECT_FALSE(r.preempted);
  EXPECT_TRUE(r.it_guid.empty());
  EXPECT_TRUE(std::holds_alternative<grammar::ActTo>(r.act->value));

  // Higher-priority order becomes true and preempts the running `always` order.
  s.base.self.unit.auras.push_back(MakeAura("Defile"));
  r = eval.OnSnapshot(s, T(2));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.step_index, 0);
  EXPECT_TRUE(r.covered);
  EXPECT_TRUE(r.preempted);
  EXPECT_EQ(r.it_guid, "Player-1");
  EXPECT_TRUE(std::holds_alternative<grammar::ActAway>(r.act->value));
}

TEST(OrderEvaluatorTest, LowerPriorityDoesNotPreemptActiveOrder) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("Defile on me -> away"),
                  MustParse("always -> to behind boss")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  s.base.self.unit.auras.push_back(MakeAura("Defile"));
  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  EXPECT_EQ(r.order_index, 0);
  eval.set_pending_command_id(42);

  // Even if Defile drops while step 0 is still executing, lower-priority order
  // 1 does not preempt order 0 until step 0 finishes.
  s.base.self.unit.auras.clear();
  r = eval.OnSnapshot(s, T(2));
  EXPECT_FALSE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_TRUE(r.covered);
}

TEST(OrderEvaluatorTest, ThenChainAdvancesOnDoneCommandId) {
  OrderEvaluator eval;
  eval.SetOrders(
      {MustParse("Defile on me -> away, then to behind boss, then wake")},
      T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  s.base.self.unit.auras.push_back(MakeAura("Defile"));

  // Step 0: away
  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.step_index, 0);
  EXPECT_TRUE(std::holds_alternative<grammar::ActAway>(r.act->value));
  EXPECT_TRUE(r.covered);
  EXPECT_TRUE(r.wake_reason.empty());
  eval.set_pending_command_id(101);

  // Still running on next snapshot.
  r = eval.OnSnapshot(s, T(2));
  EXPECT_FALSE(r.act.has_value());
  EXPECT_TRUE(r.covered);

  // Step 0 finishes -> step 1: to behind boss.
  s.done_command_id = 101;
  r = eval.OnSnapshot(s, T(3));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.step_index, 1);
  EXPECT_TRUE(std::holds_alternative<grammar::ActTo>(r.act->value));
  EXPECT_TRUE(r.covered);
  eval.set_pending_command_id(102);

  // Step 1 finishes -> step 2: wake (hands control to brain, not covered).
  s.done_command_id = 102;
  r = eval.OnSnapshot(s, T(4));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.step_index, 2);
  EXPECT_TRUE(std::holds_alternative<grammar::ActWake>(r.act->value));
  EXPECT_FALSE(r.covered);
  EXPECT_FALSE(r.wake_reason.empty());
  EXPECT_FALSE(eval.active());
}

TEST(OrderEvaluatorTest, OnStepDoneAndFailureAndTimeout) {
  OrderEvaluator eval(base::Seconds(5));
  eval.SetOrders({MustParse("Defile on me -> away, then stop casting")}, T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  s.base.self.unit.auras.push_back(MakeAura("Defile"));
  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  eval.set_pending_command_id(7);

  // Advance directly via OnStepDone.
  r = eval.OnStepDone(7, T(2));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.step_index, 1);
  EXPECT_TRUE(std::holds_alternative<grammar::ActStop>(r.act->value));
  eval.set_pending_command_id(8);

  // Timeout at 2 + 5 = 7 s.
  EXPECT_TRUE(eval.CheckTimeout(T(6.9)).wake_reason.empty());
  r = eval.CheckTimeout(T(7.0));
  EXPECT_FALSE(r.wake_reason.empty());
  EXPECT_FALSE(r.covered);
  EXPECT_FALSE(eval.active());

  // Re-trigger and test OnStepFailed.
  r = eval.OnSnapshot(s, T(8));
  ASSERT_TRUE(r.act.has_value());
  r = eval.OnStepFailed("no path");
  EXPECT_NE(r.wake_reason.find("no path"), std::string::npos);
  EXPECT_FALSE(r.covered);
  EXPECT_FALSE(eval.active());
}

TEST(OrderEvaluatorTest, AuraChecksStacksEndsAndAreaInsideMe) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("Frost Buffet stacks>3 on me -> use Ice Block"),
                  MustParse("Necrotic Plague ends<3s on tank -> use Cleanse"),
                  MustParse("Defile on me -> away")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  // Ground pool with inside_me = true satisfies `Defile on me`.
  pb::Area defile_area;
  defile_area.guid = "Area-1";
  defile_area.name = "Defile";
  defile_area.inside_me = true;
  s.base.nearby.areas.push_back(defile_area);

  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 2);
  EXPECT_EQ(r.it_guid, "Area-1");
  eval.SetOrders(eval.orders(), T(2));

  // Tank with Necrotic Plague ending in 2.5 s -> order 1.
  pb::Unit tank = MakeUnit("Player-2", "Balgor", 12.0f);
  tank.group = MakeGroup(pb::Role::TANK, /*main_tank=*/true);
  tank.auras.push_back(MakeAura("Necrotic Plague", 0, 2.5f));
  s.base.nearby.units.push_back(tank);
  r = eval.OnSnapshot(s, T(2));
  EXPECT_EQ(r.order_index, 1);
  EXPECT_EQ(r.it_guid, "Player-2");
  eval.SetOrders(eval.orders(), T(3));

  // Self with 4 stacks of Frost Buffet -> order 0.
  s.base.self.unit.auras.push_back(MakeAura("Frost Buffet", 4));
  r = eval.OnSnapshot(s, T(3));
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.it_guid, "Player-1");
}

TEST(OrderEvaluatorTest, CastAndCastOnAndCastInLast) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("Defile cast on me -> away"),
                  MustParse("Remorseless Winter cast -> away boss"),
                  MustParse("Infest cast in last 4s -> use Power Word Shield")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit boss = MakeUnit("Boss-1", "The Lich King", 8.0f);
  boss.boss = pb::BossPart{.phase = 1};
  boss.activity.state = pb::Activity::CASTING;
  boss.activity.cast = MakeCast("Infest", "Player-2");
  pb::Encounter enc;
  enc.boss = boss;
  s.ext.encounter = enc;

  // Infest is cast at t=1 -> matches `cast in last 4s`.
  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  EXPECT_EQ(r.order_index, 2);
  EXPECT_EQ(r.it_guid, "Boss-1");
  eval.set_pending_command_id(1);

  // Complete step, stop casting; at t=4 (3 s later) still within 4 s window.
  s.done_command_id = 1;
  s.ext.encounter->boss->activity = pb::Activity{};
  r = eval.OnSnapshot(s, T(4));
  EXPECT_EQ(r.order_index, 2);
  eval.set_pending_command_id(2);

  // At t=6 (5 s after cast), outside 4 s window -> idle.
  s.done_command_id = 2;
  r = eval.OnSnapshot(s, T(6));
  EXPECT_FALSE(r.act.has_value());
  EXPECT_FALSE(r.covered);

  // Boss casts Defile on me -> order 0.
  s.ext.encounter->boss->activity.state = pb::Activity::CASTING;
  s.ext.encounter->boss->activity.cast = MakeCast("Defile", "Player-1");
  r = eval.OnSnapshot(s, T(7));
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.it_guid, "Boss-1");

  // Boss casts Remorseless Winter (not targeted at me) -> order 1.
  eval.SetOrders(eval.orders(), T(8));
  s.ext.encounter->boss->activity.cast = MakeCast("Remorseless Winter");
  r = eval.OnSnapshot(s, T(8));
  EXPECT_EQ(r.order_index, 1);
}

TEST(OrderEvaluatorTest, AppearsAppearsInCountAndItNearestToIt) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("Spirit Bomb appears & me nearest to it -> to it"),
                  MustParse("Val'kyr Shadowguard count>1 -> wake"),
                  MustParse("Ice Sphere appears in last 3s -> target it")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  // Ally at (10, 0, 0), me at (0, 0, 0), Spirit Bomb at (3, 0, 0) -> me is
  // closer (3 yd vs 7 yd).
  pb::Unit ally = MakeUnit("Player-2", "Theron", 10.0f, {10.0f, 0.0f, 0.0f});
  ally.reaction = pb::Unit::FRIENDLY;
  pb::PlayerPart pp;
  pp.in_my_group = true;
  ally.player = pp;
  pb::Unit bomb = MakeUnit("Npc-10", "Spirit Bomb", 3.0f, {3.0f, 0.0f, 0.0f});
  s.base.nearby.units = {ally, bomb};

  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.it_guid, "Npc-10");
  eval.SetOrders(eval.orders(), T(2));

  // Move ally closer to Spirit Bomb (at (2, 0, 0), 1 yd away) -> order 0 fails!
  s.base.nearby.units[0].rel.position = {2.0f, 0.0f, 0.0f};
  r = eval.OnSnapshot(s, T(2));
  EXPECT_FALSE(r.act.has_value());

  // Two Val'kyrs appear (one dead shouldn't count, two alive should).
  pb::Unit v1 = MakeUnit("Npc-20", "Val'kyr Shadowguard", 15.0f);
  pb::Unit v2 = MakeUnit("Npc-21", "Val'kyr Shadowguard", 20.0f);
  v2.dead = true;
  s.base.nearby.units.push_back(v1);
  s.base.nearby.units.push_back(v2);
  r = eval.OnSnapshot(s, T(3));
  EXPECT_FALSE(r.act.has_value());

  s.base.nearby.units.back().dead = false;
  r = eval.OnSnapshot(s, T(4));
  EXPECT_EQ(r.order_index, 1);
  EXPECT_FALSE(r.wake_reason.empty());

  // Ice Sphere appears at t=5 -> matches `appears in last 3s` at t=5,
  // but NOT at t=9 even while the same Ice Sphere guid is still alive.
  s.base.nearby.units.clear();
  pb::Unit sphere = MakeUnit("Npc-30", "Ice Sphere", 12.0f);
  s.base.nearby.units.push_back(sphere);
  r = eval.OnSnapshot(s, T(5));
  EXPECT_EQ(r.order_index, 2);
  EXPECT_EQ(r.it_guid, "Npc-30");
  eval.set_pending_command_id(5);

  s.done_command_id = 5;
  r = eval.OnSnapshot(s, T(9));
  EXPECT_FALSE(r.act.has_value());

  // A second Ice Sphere (new guid) spawns at t=10 -> triggers again!
  s.base.nearby.units.push_back(MakeUnit("Npc-31", "Ice Sphere", 14.0f));
  r = eval.OnSnapshot(s, T(10));
  EXPECT_EQ(r.order_index, 2);
  EXPECT_EQ(r.it_guid, "Npc-31");
}

TEST(OrderEvaluatorTest, SpatialWithinBeyondTargetsAndWhoRoles) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("anyone within 8 -> away nearest ally 10"),
                  MustParse("offtank beyond 20 of tank -> wake"),
                  MustParse("boss targets healer -> use Taunt")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit mt = MakeUnit("Player-2", "MainTank", 15.0f, {15.0f, 0.0f, 0.0f});
  mt.group = MakeGroup(pb::Role::TANK, /*main_tank=*/true);
  pb::Unit ot = MakeUnit("Player-3", "OffTank", 15.0f, {20.0f, 0.0f, 0.0f});
  ot.group = MakeGroup(pb::Role::TANK);
  pb::Unit healer = MakeUnit("Player-4", "Healer", 18.0f, {0.0f, 18.0f, 0.0f});
  healer.group = MakeGroup(pb::Role::HEALER);
  pb::Unit boss = MakeUnit("Boss-1", "Patchwerk", 15.0f, {15.0f, 2.0f, 0.0f});
  boss.boss = pb::BossPart{.phase = 1};
  boss.target = "Player-2";
  s.base.nearby.units = {mt, ot, healer, boss};

  // Nobody within 8 yd of me (`me` must not match `anyone within 8` against
  // itself!), offtank is 5 yd from tank, boss targets tank -> idle.
  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  EXPECT_FALSE(r.act.has_value());

  // Boss targets healer -> order 2.
  s.base.nearby.units[3].target = "Player-4";
  r = eval.OnSnapshot(s, T(2));
  EXPECT_EQ(r.order_index, 2);
  eval.SetOrders(eval.orders(), T(3));

  // Offtank moves 25 yd from tank -> order 1.
  s.base.nearby.units[1].rel.position = {40.0f, 0.0f, 0.0f};
  r = eval.OnSnapshot(s, T(3));
  EXPECT_EQ(r.order_index, 1);

  // Someone steps within 6 yd of me -> order 0.
  s.base.nearby.units[2].rel.dist_yd = 6.0f;
  s.base.nearby.units[2].rel.position = {0.0f, 6.0f, 0.0f};
  r = eval.OnSnapshot(s, T(4));
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.it_guid, "Player-4");
}

TEST(OrderEvaluatorTest, ResourcesStatesSightBehindTargetHasAndTimers) {
  OrderEvaluator eval;
  eval.SetOrders(
      {MustParse("me hp<30 -> use Healthstone"),
       MustParse("mana<20 -> use Evocation"),
       MustParse("mana_points<500 -> use Mana Potion"),
       MustParse("boss is marked skull & not me behind boss -> to behind boss"),
       MustParse("my target is focus & boss out of sight -> to boss"),
       MustParse("me has Wyrmrest Lance & me in vehicle -> use Thrust"),
       MustParse("phase 2 & after 5s & every 10s -> use Bloodlust")},
      T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit boss = MakeUnit("Boss-1", "Malygos", 20.0f);
  boss.boss = pb::BossPart{.phase = 2};
  boss.raid_marker = pb::RaidMarker::SKULL;
  boss.rel.behind_it = true;
  boss.rel.in_los = true;
  pb::Encounter enc;
  enc.boss = boss;
  s.ext.encounter = enc;
  pb::Target tgt;
  tgt.target = boss;
  tgt.focus = boss;
  s.ext.target = tgt;

  // At t=4, nothing matches (`after 5s` not met yet).
  EXPECT_FALSE(eval.OnSnapshot(s, T(4)).act.has_value());

  // At t=10, `phase 2 & after 5s & every 10s` matches order 6!
  OrderEvalResult r = eval.OnSnapshot(s, T(10));
  EXPECT_EQ(r.order_index, 6);
  eval.set_pending_command_id(10);
  s.done_command_id = 10;

  // At t=15 (only 5 s since `every 10s` last fired), order 6 does not fire.
  EXPECT_FALSE(eval.OnSnapshot(s, T(15)).act.has_value());

  // At t=20 (10 s since last fire), order 6 fires again.
  r = eval.OnSnapshot(s, T(20));
  EXPECT_EQ(r.order_index, 6);
  eval.set_pending_command_id(11);
  s.done_command_id = 11;

  // `me has Wyrmrest Lance & me in vehicle` -> order 5.
  pb::Item lance;
  lance.name = "Wyrmrest Lance";
  pb::Character ch;
  ch.equipped.push_back(lance);
  s.ext.character = ch;
  s.ext.vehicle = MakeUnit("Veh-1", "Red Drake", 0.0f);
  r = eval.OnSnapshot(s, T(21));
  EXPECT_EQ(r.order_index, 5);
  eval.SetOrders(eval.orders(), T(22));

  // `my target is focus & boss out of sight` -> order 4.
  s.ext.encounter->boss->rel.in_los = false;
  r = eval.OnSnapshot(s, T(22));
  EXPECT_EQ(r.order_index, 4);
  eval.SetOrders(eval.orders(), T(23));

  // `boss is marked skull & not me behind boss` -> order 3.
  s.ext.encounter->boss->rel.behind_it = false;
  r = eval.OnSnapshot(s, T(23));
  EXPECT_EQ(r.order_index, 3);
  eval.SetOrders(eval.orders(), T(24));

  // `mana_points<500` -> order 2 (400/1000 = 40%, so `mana<20` is false).
  s.base.self.power = 400;
  s.base.self.power_max = 1000;
  r = eval.OnSnapshot(s, T(24));
  EXPECT_EQ(r.order_index, 2);
  eval.SetOrders(eval.orders(), T(25));

  // `mana<20` -> order 1.
  s.base.self.power = 150;
  r = eval.OnSnapshot(s, T(25));
  EXPECT_EQ(r.order_index, 1);
  eval.SetOrders(eval.orders(), T(26));

  // `me hp<30` -> order 0.
  s.base.self.unit.health_pct = 25.0f;
  r = eval.OnSnapshot(s, T(26));
  EXPECT_EQ(r.order_index, 0);
}

TEST(OrderEvaluatorTest, MissingSnapshotStatesSafelyReturnFalse) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("boss is flying -> stop attacking"),
                  MustParse("boss is grounded -> target boss"),
                  MustParse("boss is untargetable -> stop attacking"),
                  MustParse("boss is submerged -> stop attacking")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit boss = MakeUnit("Boss-1", "Razorscale", 25.0f);
  boss.boss = pb::BossPart{.phase = 1};
  pb::Encounter enc;
  enc.boss = boss;
  s.ext.encounter = enc;

  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  EXPECT_FALSE(r.act.has_value());
  EXPECT_FALSE(r.covered);
}

TEST(OrderEvaluatorTest, MountedSwimmingInSightAndAllyWith) {
  OrderEvaluator eval;
  eval.SetOrders(
      {MustParse("me mounted & me swimming -> leave vehicle"),
       MustParse("ally with Necrotic Plague within 10 -> away ally with "
                 "Necrotic Plague 15"),
       MustParse("marker skull in sight -> target marker skull")},
      T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit marked = MakeUnit("Npc-1", "Shambling Horror", 25.0f);
  marked.raid_marker = pb::RaidMarker::SKULL;
  marked.rel.in_los = true;
  s.base.nearby.units.push_back(marked);

  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  EXPECT_EQ(r.order_index, 2);
  EXPECT_EQ(r.it_guid, "Npc-1");
  eval.SetOrders(eval.orders(), T(2));

  pb::Unit plagued = MakeUnit("Player-5", "Sickly", 6.0f);
  plagued.reaction = pb::Unit::FRIENDLY;
  plagued.auras.push_back(MakeAura("Necrotic Plague"));
  s.base.nearby.units.push_back(plagued);
  r = eval.OnSnapshot(s, T(2));
  EXPECT_EQ(r.order_index, 1);
  EXPECT_EQ(r.it_guid, "Player-5");
  eval.SetOrders(eval.orders(), T(3));

  s.base.self.mounted = true;
  s.base.self.swimming = true;
  r = eval.OnSnapshot(s, T(3));
  EXPECT_EQ(r.order_index, 0);
}

}  // namespace
}  // namespace harness
