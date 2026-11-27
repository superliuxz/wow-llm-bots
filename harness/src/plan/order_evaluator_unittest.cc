// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/order_evaluator.h"

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
#include "plan/orders.h"
#include "plan/triggers.h"
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
  // `always` orders never suppress default wakes.
  EXPECT_FALSE(r.covered);
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
  s.done_act_command_id = 101;
  r = eval.OnSnapshot(s, T(3));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.step_index, 1);
  EXPECT_TRUE(std::holds_alternative<grammar::ActTo>(r.act->value));
  EXPECT_TRUE(r.covered);
  eval.set_pending_command_id(102);

  // Step 1 finishes -> step 2: wake (hands control to brain, not covered).
  s.done_act_command_id = 102;
  r = eval.OnSnapshot(s, T(4));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.step_index, 2);
  EXPECT_TRUE(std::holds_alternative<grammar::ActWake>(r.act->value));
  EXPECT_FALSE(r.covered);
  EXPECT_FALSE(r.wake_reason.empty());
  EXPECT_FALSE(eval.active());

  // Latching: while Defile is STILL on me, the order does NOT fire again.
  r = eval.OnSnapshot(s, T(5));
  EXPECT_FALSE(r.act.has_value());
  EXPECT_TRUE(r.wake_reason.empty());

  // Defile clears -> latch re-arms; re-applying Defile fires step 0 again.
  s.base.self.unit.auras.clear();
  EXPECT_FALSE(eval.OnSnapshot(s, T(6)).act.has_value());
  s.base.self.unit.auras.push_back(MakeAura("Defile"));
  r = eval.OnSnapshot(s, T(7));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  EXPECT_EQ(r.step_index, 0);
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

  // Timeout at 2 + 5 = 7 s (instant/stop step uses 5 s timeout).
  EXPECT_TRUE(eval.CheckTimeout(T(6.9)).wake_reason.empty());
  r = eval.CheckTimeout(T(7.0));
  EXPECT_FALSE(r.wake_reason.empty());
  EXPECT_TRUE(r.timed_out);
  EXPECT_FALSE(r.covered);
  EXPECT_FALSE(eval.active());

  // Clear Defile to re-arm latch, re-trigger, and test OnStepFailed.
  s.base.self.unit.auras.clear();
  EXPECT_FALSE(eval.OnSnapshot(s, T(7.5)).act.has_value());
  s.base.self.unit.auras.push_back(MakeAura("Defile"));
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

  // Complete step, stop casting; because the order is latched while the 4 s
  // window still holds, it does not re-fire until the window expires and a new
  // cast starts.
  s.done_act_command_id = 1;
  s.ext.encounter->boss->activity = pb::Activity{};
  r = eval.OnSnapshot(s, T(4));
  EXPECT_FALSE(r.act.has_value());

  // At t=6 (5 s after cast), outside 4 s window -> latch re-arms.
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

  s.done_act_command_id = 5;
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
  s.done_act_command_id = 10;

  // At t=15 (only 5 s since `every 10s` last fired), order 6 does not fire.
  EXPECT_FALSE(eval.OnSnapshot(s, T(15)).act.has_value());

  // At t=20 (10 s since last fire), order 6 fires again.
  r = eval.OnSnapshot(s, T(20));
  EXPECT_EQ(r.order_index, 6);
  eval.set_pending_command_id(11);
  s.done_act_command_id = 11;

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

TEST(OrderEvaluatorTest, UnitStatesFlyingGroundedUntargetableSubmerged) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("boss is flying -> stop attacking"),
                  MustParse("boss is untargetable -> stop attacking"),
                  MustParse("boss is submerged -> stop attacking"),
                  MustParse("boss is grounded -> target boss")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit boss = MakeUnit("Boss-1", "Razorscale", 25.0f);
  boss.boss = pb::BossPart{.phase = 1};
  pb::Encounter enc;
  enc.boss = boss;
  s.ext.encounter = enc;

  // Grounded by default (neither FLYING nor SUBMERGED).
  OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 3);
  eval.SetOrders(eval.orders(), T(2));

  // Flying -> order 0.
  s.ext.encounter->boss->visual_state = {pb::Unit::FLYING};
  r = eval.OnSnapshot(s, T(2));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 0);
  eval.SetOrders(eval.orders(), T(3));

  // Untargetable -> order 1.
  s.ext.encounter->boss->visual_state = {pb::Unit::UNTARGETABLE};
  r = eval.OnSnapshot(s, T(3));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 1);
  eval.SetOrders(eval.orders(), T(4));

  // Submerged -> order 2 (and not grounded).
  s.ext.encounter->boss->visual_state = {pb::Unit::SUBMERGED};
  r = eval.OnSnapshot(s, T(4));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.order_index, 2);
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

TEST(OrderEvaluatorTest, OrderActToCommandAndWireRoundTrip) {
  auto to_wire = [](std::string_view order_text, std::string_view it_guid = "",
                    std::string_view it_name = "") {
    Order o = MustParse(order_text);
    return OrderActToCommand(o.steps.front(), it_guid, it_name);
  };

  // `to behind boss`
  auto cmd_act = to_wire("always -> to behind boss");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::TO);
  EXPECT_EQ(cmd_act->place, pb::Command::Act::BEHIND);
  EXPECT_EQ(cmd_act->thing, pb::Command::Act::BOSS);

  // `to 8 yd from tank`
  cmd_act = to_wire("always -> to 8 yd from tank");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::TO);
  EXPECT_EQ(cmd_act->place, pb::Command::Act::YD_FROM);
  EXPECT_EQ(cmd_act->thing, pb::Command::Act::TANK);
  EXPECT_FLOAT_EQ(cmd_act->yd, 8.0f);

  // `away nearest ally 10`
  cmd_act = to_wire("always -> away nearest ally 10");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::AWAY);
  EXPECT_EQ(cmd_act->thing, pb::Command::Act::NEAREST_ALLY);
  EXPECT_FLOAT_EQ(cmd_act->yd, 10.0f);

  // `target it` carries `it_guid` and `it_name` across BEVE round-trip.
  cmd_act = to_wire("always -> target it", "Npc-99", "Val'kyr");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::TARGET);
  EXPECT_EQ(cmd_act->thing, pb::Command::Act::IT);
  EXPECT_EQ(cmd_act->it_guid, "Npc-99");
  EXPECT_EQ(cmd_act->it_name, "Val'kyr");

  pb::Command wire_cmd;
  wire_cmd.version = pb::kWireVersion;
  wire_cmd.bot = "Aelindra";
  wire_cmd.command_id = 42;
  wire_cmd.act = *cmd_act;
  const std::string bytes = pb::WriteBeve(wire_cmd);
  pb::Command decoded;
  ASSERT_TRUE(pb::ReadBeve(bytes, &decoded));
  EXPECT_EQ(decoded, wire_cmd);

  // `use Cleanse on tank`
  cmd_act = to_wire("always -> use Cleanse on tank");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::USE);
  EXPECT_EQ(cmd_act->name, "Cleanse");
  EXPECT_EQ(cmd_act->thing, pb::Command::Act::TANK);

  // `stop casting` & `wake`
  cmd_act = to_wire("always -> stop casting");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::STOP);
  EXPECT_EQ(cmd_act->stop, pb::Command::Act::CASTING);

  cmd_act = to_wire("always -> wake");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::WAKE);

  // Remaining #5d verbs all convert cleanly.
  cmd_act = to_wire("always -> kite boss");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::KITE);
  EXPECT_EQ(cmd_act->thing, pb::Command::Act::BOSS);

  cmd_act = to_wire("always -> mark boss skull");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::MARK);
  EXPECT_EQ(cmd_act->marker, pb::RaidMarker::SKULL);

  cmd_act = to_wire("always -> pet passive");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::PET);
  EXPECT_EQ(cmd_act->pet_cmd, pb::Command::Act::PASSIVE);

  cmd_act = to_wire("always -> pet attack boss");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::PET_ATTACK);

  // `ping` is the spot of the party's minimap ping (never a unit: no name,
  // no GUID even when one is resolved), and `pet dismiss` its own command;
  // both survive the BEVE round trip.
  cmd_act = to_wire("always -> to ping");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::TO);
  EXPECT_EQ(cmd_act->place, pb::Command::Act::AT);
  EXPECT_EQ(cmd_act->thing, pb::Command::Act::PING);
  EXPECT_TRUE(cmd_act->name.empty());
  cmd_act = to_wire("always -> use Flare on ping");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::USE);
  EXPECT_EQ(cmd_act->name, "Flare");
  EXPECT_EQ(cmd_act->thing, pb::Command::Act::PING);
  EXPECT_TRUE(cmd_act->target_name.empty());
  EXPECT_EQ(to_wire("always -> face ping")->thing, pb::Command::Act::PING);
  cmd_act = to_wire("always -> pet dismiss");
  ASSERT_TRUE(cmd_act.has_value());
  EXPECT_EQ(cmd_act->verb, pb::Command::Act::PET);
  EXPECT_EQ(cmd_act->pet_cmd, pb::Command::Act::DISMISS);
  EXPECT_EQ(to_wire("always -> use Flare")->thing,
            pb::Command::Act::THING_UNSPECIFIED);
  EXPECT_EQ(to_wire("always -> pet defensive")->pet_cmd,
            pb::Command::Act::DEFENSIVE);
  for (const char* text :
       {"always -> use Flare on ping", "always -> pet dismiss"}) {
    pb::Command c;
    c.version = pb::kWireVersion;
    c.bot = "Aelindra";
    c.command_id = 43;
    c.act = to_wire(text);
    pb::Command back;
    ASSERT_TRUE(pb::ReadBeve(pb::WriteBeve(c), &back)) << text;
    EXPECT_EQ(back, c) << text;
  }

  EXPECT_EQ(to_wire("always -> avoid boss")->verb, pb::Command::Act::AVOID);
  EXPECT_EQ(to_wire("always -> face boss")->verb, pb::Command::Act::FACE);
  EXPECT_EQ(to_wire("always -> face away boss")->verb,
            pb::Command::Act::FACE_AWAY);
  EXPECT_EQ(to_wire("always -> jump")->verb, pb::Command::Act::JUMP);
  EXPECT_EQ(to_wire("always -> cancel Ice Block")->verb,
            pb::Command::Act::CANCEL);
  EXPECT_EQ(to_wire("always -> interact Portal")->verb,
            pb::Command::Act::INTERACT);
  EXPECT_EQ(to_wire("always -> board Drake")->verb, pb::Command::Act::BOARD);
  EXPECT_EQ(to_wire("always -> leave vehicle")->verb, pb::Command::Act::LEAVE);
  EXPECT_EQ(to_wire("always -> equip Wyrmrest Lance")->verb,
            pb::Command::Act::EQUIP);
  {
    const std::optional<pb::Command::Act> gold =
        to_wire("always -> trade gold 5g 25s");
    ASSERT_TRUE(gold.has_value());
    EXPECT_EQ(gold->verb, pb::Command::Act::TRADE_GOLD);
    EXPECT_EQ(gold->copper, 52500u);
    pb::Command c;
    c.version = pb::kWireVersion;
    c.bot = "Aelindra";
    c.command_id = 44;
    c.act = *gold;
    pb::Command back;
    ASSERT_TRUE(pb::ReadBeve(pb::WriteBeve(c), &back));
    EXPECT_EQ(back, c);
  }
  // Overflow above MAX_MONEY_AMOUNT does not produce a wire command.
  EXPECT_FALSE(to_wire("always -> trade gold 214748g 99s 99c").has_value());
}

TEST(OrderEvaluatorTest, AnyoneExcludesHostileEnemies) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("anyone hp<30 -> use Flash Heal on it")}, T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit boar = MakeUnit("Npc-9", "Mottled Boar", 5.0f);
  boar.reaction = pb::Unit::HOSTILE;
  boar.health_pct = 10.0f;
  s.base.nearby.units.push_back(boar);

  // Dying hostile boar must NOT trigger `anyone hp<30`.
  EXPECT_FALSE(eval.OnSnapshot(s, T(1)).act.has_value());

  // Low-HP group member DOES trigger it.
  pb::Unit ally = MakeUnit("Player-2", "Balgor", 12.0f);
  ally.kind = pb::Unit::PLAYER;
  ally.reaction = pb::Unit::FRIENDLY;
  ally.health_pct = 20.0f;
  s.base.nearby.units.push_back(ally);
  OrderEvalResult r = eval.OnSnapshot(s, T(2));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.it_guid, "Player-2");
}

TEST(OrderEvaluatorTest, FirstSnapshotDoesNotFalseFireAppearsInLast) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("Ice Sphere appears in last 5s -> target it")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  // Already present on the very first snapshot -> seeded, does NOT fire.
  s.base.nearby.units.push_back(MakeUnit("Npc-1", "Ice Sphere", 15.0f));
  EXPECT_FALSE(eval.OnSnapshot(s, T(1)).act.has_value());

  // A new Ice Sphere spawning on snapshot 2 DOES fire.
  s.base.nearby.units.push_back(MakeUnit("Npc-2", "Ice Sphere", 12.0f));
  OrderEvalResult r = eval.OnSnapshot(s, T(2));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.it_guid, "Npc-2");
}

TEST(OrderEvaluatorTest, KiteNeverTimesOutAndMoveUsesLongTimeout) {
  OrderEvaluator eval(base::Seconds(5));
  eval.SetOrders({MustParse("boss within 10 -> kite boss")}, T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit boss = MakeUnit("Boss-1", "Gluth", 8.0f);
  boss.boss = pb::BossPart{.phase = 1};
  s.ext.encounter.emplace().boss = boss;

  ASSERT_TRUE(eval.OnSnapshot(s, T(1)).act.has_value());
  // Kiting is continuous: still active after 60 s.
  EXPECT_TRUE(eval.CheckTimeout(T(61)).wake_reason.empty());
  EXPECT_TRUE(eval.active());

  // `to` uses kLongOrderMoveTimeout (15 s), not 5 s.
  eval.SetOrders({MustParse("boss within 10 -> to max range of boss")}, T(70));
  ASSERT_TRUE(eval.OnSnapshot(s, T(71)).act.has_value());
  EXPECT_TRUE(eval.CheckTimeout(T(80)).wake_reason.empty());
  OrderEvalResult to = eval.CheckTimeout(T(86));
  EXPECT_FALSE(to.wake_reason.empty());
  EXPECT_TRUE(to.timed_out);
}

// A kite ends when its `when` stops holding, with a stop and no wake.
TEST(OrderEvaluatorTest, KiteEndsWhenItsConditionClears) {
  OrderEvaluator eval(base::Seconds(5));
  eval.SetOrders({MustParse("boss within 10 -> kite boss")}, T(0));
  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit boss = MakeUnit("Boss-1", "Gluth", 8.0f);
  boss.boss = pb::BossPart{.phase = 1};
  s.ext.encounter.emplace().boss = boss;
  ASSERT_TRUE(eval.OnSnapshot(s, T(1)).act.has_value());
  EXPECT_TRUE(eval.OnSnapshot(s, T(2)).covered);  // Still kiting.
  s.ext.encounter->boss->rel.dist_yd = 25.0f;
  const OrderEvalResult r = eval.OnSnapshot(s, T(3));
  EXPECT_TRUE(r.kite_ended);
  EXPECT_TRUE(r.wake_reason.empty());
  EXPECT_FALSE(eval.active());
}

TEST(OrderEvaluatorTest, OrderCoversWakeByConditionAndSubject) {
  Order away_boss = MustParse("boss within 10 -> away");
  EXPECT_FALSE(OrderCoversWake(away_boss, kWakeHealth));
  EXPECT_FALSE(OrderCoversWake(away_boss, kWakeDied));
  EXPECT_FALSE(OrderCoversWake(away_boss, kWakeLostControl));
  EXPECT_FALSE(OrderCoversWake(away_boss, kWakeAttacked));

  Order hp_order = MustParse("me hp<30 -> use Healthstone");
  EXPECT_TRUE(OrderCoversWake(hp_order, kWakeHealth));
  EXPECT_FALSE(OrderCoversWake(hp_order, kWakeMana));
  EXPECT_FALSE(OrderCoversWake(hp_order, kWakeDied));
  // A merged wake is covered only if every part is.
  EXPECT_FALSE(OrderCoversWake(hp_order, kWakeHealth | kWakeAttacked));

  // Someone else's health doesn't cover my own `hp<N` watch.
  EXPECT_FALSE(OrderCoversWake(
      MustParse("tank hp<30 -> use Flash Heal on tank"), kWakeHealth));
  // Being attacked is covered by an order about what targets me only.
  EXPECT_TRUE(OrderCoversWake(MustParse("boss targets me -> face away boss"),
                              kWakeAttacked | kWakeEnteredCombat));
  EXPECT_FALSE(OrderCoversWake(
      MustParse("Valkyr appears -> target nearest Valkyr"), kWakeAttacked));

  Order always_order = MustParse("always -> to behind boss");
  EXPECT_FALSE(OrderCoversWake(always_order, kWakeHealth));
  EXPECT_FALSE(OrderCoversWake(always_order, kWakeEnteredCombat));

  // `X targets me` hides attacks by X only, never another attacker.
  Order boar_order = MustParse("Mottled Boar targets me -> away");
  EXPECT_TRUE(OrderCoversWake(boar_order, kWakeAttacked, {"Mottled Boar"}));
  EXPECT_FALSE(OrderCoversWake(boar_order, kWakeAttacked, {"Defias Pillager"}));
  EXPECT_FALSE(OrderCoversWake(boar_order, kWakeAttacked,
                               {"Mottled Boar", "Defias Pillager"}));
  EXPECT_FALSE(OrderCoversWake(boar_order, kWakeAttacked, {}));
}

TEST(OrderEvaluatorTest, SetOrdersPreservesLatchesForUnchangedOrders) {
  OrderEvaluator eval(base::Seconds(5));
  eval.SetOrders({MustParse("me hp<30 -> wake")}, T(0));
  pb::Snapshot s = MakeBaseSnapshot();
  s.base.self.unit.health_pct = 20.0f;

  const OrderEvalResult r1 = eval.OnSnapshot(s, T(1));
  EXPECT_FALSE(r1.wake_reason.empty());

  // Re-installing the same order while hp<30 still holds must NOT re-fire.
  eval.SetOrders({MustParse("me hp<30 -> wake")}, T(2));
  const OrderEvalResult r2 = eval.OnSnapshot(s, T(3));
  EXPECT_TRUE(r2.wake_reason.empty());
  EXPECT_FALSE(r2.act.has_value());

  // Once hp recovers above 30, the latch re-arms and fires on the next drop.
  s.base.self.unit.health_pct = 80.0f;
  EXPECT_TRUE(eval.OnSnapshot(s, T(4)).wake_reason.empty());
  s.base.self.unit.health_pct = 20.0f;
  EXPECT_FALSE(eval.OnSnapshot(s, T(5)).wake_reason.empty());
}

TEST(OrderEvaluatorTest, ReplanPreservesAfterAndFirstEveryClocks) {
  OrderEvaluator eval(base::Seconds(5));
  const std::vector<Order> orders = {
      MustParse("after 5s -> use Bloodlust"),
      MustParse("every 6s -> use Shield Block"),
  };
  eval.SetOrders(orders, T(0));
  pb::Snapshot s = MakeBaseSnapshot();

  // Replan every 1.5 s: `after 5s` and first `every 6s` must still fire!
  EXPECT_FALSE(eval.OnSnapshot(s, T(1)).act.has_value());
  eval.SetOrders(orders, T(2));
  EXPECT_FALSE(eval.OnSnapshot(s, T(3)).act.has_value());
  eval.SetOrders(orders, T(4));
  const OrderEvalResult r5 = eval.OnSnapshot(s, T(5));
  ASSERT_TRUE(r5.act.has_value());
  EXPECT_EQ(r5.order_index, 0);
  eval.set_pending_command_id(1);
  eval.OnStepDone(1, T(5));
  const OrderEvalResult r6 = eval.OnSnapshot(s, T(6));
  ASSERT_TRUE(r6.act.has_value());
  EXPECT_EQ(r6.order_index, 1);
}

TEST(OrderEvaluatorTest, MeleeContactDistZeroMatchesWithinAndSendsTargetGuid) {
  OrderEvaluator eval(base::Seconds(5));
  eval.SetOrders({MustParse("boss within 2 -> away boss 25")}, T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  // Boss is in melee contact: edge-to-edge dist_yd is 0.0 even though its
  // centre position is 8 yd away.
  pb::Unit boss = MakeUnit("Boss-42", "Loken", 0.0f, {8.0f, 0.0f, 0.0f});
  boss.boss = pb::BossPart{.phase = 1};
  s.ext.encounter.emplace().boss = boss;

  const OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.target_guid, "Boss-42");
  const std::optional<pb::Command::Act> wire =
      OrderActToCommand(*r.act, r.it_guid, r.it_name, r.target_guid);
  ASSERT_TRUE(wire.has_value());
  EXPECT_EQ(wire->thing, pb::Command::Act::BOSS);
  EXPECT_EQ(wire->it_guid, "Boss-42");
}

TEST(OrderEvaluatorTest, NamedActPrefersMatchedOrNearestUnit) {
  OrderEvaluator eval(base::Seconds(5));
  eval.SetOrders({MustParse("Mottled Boar targets me -> kite Mottled Boar")},
                 T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  s.base.self.unit.guid = "Player-1";
  // Far idle boar listed first; charging boar at 3 yd listed second.
  pb::Unit far_boar = MakeUnit("Boar-far", "Mottled Boar", 30.0f);
  pb::Unit near_boar = MakeUnit("Boar-near", "Mottled Boar", 3.0f);
  near_boar.target = "Player-1";
  s.base.nearby.units = {far_boar, near_boar};

  const OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  EXPECT_EQ(r.target_guid, "Boar-near");
}

TEST(OrderEvaluatorTest, InAreaMatchesAreaAndCastRadiusAndRearms) {
  OrderEvaluator eval(base::Seconds(5));
  eval.SetOrders({MustParse("in Flame Strike -> out of it, then back")}, T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  s.base.self.unit.rel.position = {10.0f, 0.0f, 0.0f};
  pb::Area fs;
  fs.guid = "trigger:0xF13000605A000001";
  fs.name = "Flame Strike";
  fs.radius_yd = 8.0f;
  fs.inside_me = true;
  fs.hostile = true;
  fs.rel.emplace().dist_yd = 2.0f;
  s.base.nearby.areas.push_back(fs);

  const OrderEvalResult r1 = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r1.act.has_value());
  EXPECT_EQ(r1.it_guid, "trigger:0xF13000605A000001");
  const std::optional<pb::Command::Act> wire1 =
      OrderActToCommand(*r1.act, r1.it_guid, r1.it_name, r1.target_guid);
  ASSERT_TRUE(wire1.has_value());
  EXPECT_EQ(wire1->verb, pb::Command::Act::OUT_OF);
  EXPECT_EQ(wire1->thing, pb::Command::Act::IT);
  eval.set_pending_command_id(10);

  // Finishing step 0 advances to `then back`.
  s.done_act_command_id = 10;
  const OrderEvalResult r2 = eval.OnSnapshot(s, T(2));
  ASSERT_TRUE(r2.act.has_value());
  const std::optional<pb::Command::Act> wire2 =
      OrderActToCommand(*r2.act, r2.it_guid, r2.it_name, r2.target_guid);
  ASSERT_TRUE(wire2.has_value());
  EXPECT_EQ(wire2->verb, pb::Command::Act::BACK);
  eval.set_pending_command_id(11);

  // Stepping out clears the condition and re-arms the latch.
  s.done_act_command_id = 11;
  s.base.nearby.areas[0].inside_me = false;
  s.base.nearby.areas[0].rel->dist_yd = 12.0f;
  EXPECT_FALSE(eval.OnSnapshot(s, T(3)).act.has_value());

  // Second Flame Strike under the bot fires again with the new GUID.
  pb::Area fs2 = fs;
  fs2.guid = "trigger:0xF13000605A000002";
  s.base.nearby.areas.push_back(fs2);
  const OrderEvalResult r3 = eval.OnSnapshot(s, T(4));
  ASSERT_TRUE(r3.act.has_value());
  EXPECT_EQ(r3.it_guid, "trigger:0xF13000605A000002");
}

TEST(OrderEvaluatorTest, HandFormedRaidRolesMatchFromNearbyWithoutInlineGroup) {
  // Real `SnapshotBuilder` puts `Unit::group` on `ext.party->members` while
  // `base.nearby.units` omits `Unit::group`. In a hand-formed 10/25-man raid
  // (no Main Tank flags), every bot — including the offtank itself — must
  // resolve the same `tank`, `offtank`, and nearest living `healer`.
  auto build_raid = [](bool self_is_ot) {
    pb::Snapshot s = MakeBaseSnapshot();
    s.ext.raid.emplace();
    pb::Party& party = s.ext.party.emplace();

    pb::Unit t1_nearby =
        MakeUnit("0x0000000000000010", "TankOne", 20.0f, {20.0f, 0.0f, 0.0f});
    pb::Unit t1_party = t1_nearby;
    t1_party.group = MakeGroup(pb::Role::TANK, /*main_tank=*/false);
    t1_party.group->subgroup = 1;

    pb::Unit t2_nearby =
        MakeUnit("0x0000000000000005", "TankTwo", 4.0f, {4.0f, 0.0f, 0.0f});
    pb::Unit t2_party = t2_nearby;
    t2_party.group = MakeGroup(pb::Role::TANK, /*main_tank=*/false);
    t2_party.group->subgroup = 2;

    pb::Unit heal_dead_nearby =
        MakeUnit("0x0000000000000030", "DeadHealer", 3.0f, {3.0f, 0.0f, 0.0f});
    heal_dead_nearby.dead = true;
    pb::Unit heal_dead_party = heal_dead_nearby;
    heal_dead_party.group = MakeGroup(pb::Role::HEALER);
    heal_dead_party.group->subgroup = 1;

    pb::Unit heal_alive_nearby = MakeUnit("0x0000000000000031", "LiveHealer",
                                          12.0f, {0.0f, 12.0f, 0.0f});
    pb::Unit heal_alive_party = heal_alive_nearby;
    heal_alive_party.group = MakeGroup(pb::Role::HEALER);
    heal_alive_party.group->subgroup = 3;

    if (self_is_ot) {
      // Self is TankTwo (subgroup 2, lower guid than TankOne in subgroup 1).
      s.base.self.unit.guid = t2_nearby.guid;
      s.base.self.unit.name = t2_nearby.name;
      s.base.self.unit.group = t2_party.group;
      s.base.nearby.units = {t1_nearby, heal_dead_nearby, heal_alive_nearby};
      party.members = {t1_party, heal_dead_party, heal_alive_party};
    } else {
      // Self is a DPS in subgroup 4; TankTwo is closer in `nearby.units` than
      // TankOne, so distance order alone would swap tank and offtank.
      s.base.self.unit.group = MakeGroup(pb::Role::DPS);
      s.base.self.unit.group->subgroup = 4;
      s.base.nearby.units = {t2_nearby, t1_nearby, heal_dead_nearby,
                             heal_alive_nearby};
      party.members = {t2_party, t1_party, heal_dead_party, heal_alive_party};
    }
    return s;
  };

  const Act to_tank = MustParse("always -> to tank").steps.front();
  const Act to_offtank = MustParse("always -> to offtank").steps.front();
  const Act to_healer = MustParse("always -> to healer").steps.front();

  for (bool self_is_ot : {false, true}) {
    const pb::Snapshot s = build_raid(self_is_ot);
    // TankOne (subgroup 1) is always `tank`, TankTwo (subgroup 2) is always
    // `offtank`, and LiveHealer beats closer DeadHealer.
    EXPECT_EQ(ResolvePlanActTargetGuid(s, to_tank, T(1)), "0x0000000000000010");
    EXPECT_EQ(ResolvePlanActTargetGuid(s, to_offtank, T(1)),
              "0x0000000000000005");
    EXPECT_EQ(ResolvePlanActTargetGuid(s, to_healer, T(1)),
              "0x0000000000000031");
  }
}

TEST(OrderEvaluatorTest, LfgRaidMainTankFlagsPreserveRaidOrder) {
  pb::Snapshot s = MakeBaseSnapshot();
  s.base.self.unit.group = MakeGroup(pb::Role::DPS);
  s.base.self.unit.group->subgroup = 4;

  pb::Raid& raid = s.ext.raid.emplace();
  // Raid leader marked Subgroup-3 tank first, Subgroup-1 tank second.
  raid.main_tanks = {"MtSub3", "MtSub1"};

  pb::Unit mt3 =
      MakeUnit("0x0000000000000090", "MtSub3", 18.0f, {18.0f, 0.0f, 0.0f});
  pb::Unit mt1 =
      MakeUnit("0x0000000000000010", "MtSub1", 8.0f, {8.0f, 0.0f, 0.0f});
  s.base.nearby.units = {mt1, mt3};

  pb::Party& party = s.ext.party.emplace();
  pb::Unit mt3_p = mt3;
  mt3_p.group = MakeGroup(pb::Role::TANK, /*main_tank=*/true);
  mt3_p.group->subgroup = 3;
  pb::Unit mt1_p = mt1;
  mt1_p.group = MakeGroup(pb::Role::TANK, /*main_tank=*/true);
  mt1_p.group->subgroup = 1;
  party.members = {mt3_p, mt1_p};

  const Act to_tank = MustParse("always -> to tank").steps.front();
  const Act to_offtank = MustParse("always -> to offtank").steps.front();
  EXPECT_EQ(ResolvePlanActTargetGuid(s, to_tank, T(1)), "0x0000000000000090");
  EXPECT_EQ(ResolvePlanActTargetGuid(s, to_offtank, T(1)),
            "0x0000000000000010");
}

TEST(OrderEvaluatorTest, AwayGroupCoversWholeRaidAndNeverLeaksSelfItGuid) {
  OrderEvaluator eval;
  eval.SetOrders({MustParse("Living Bomb on me -> away group 15")}, T(0));

  pb::Snapshot s = MakeBaseSnapshot();
  s.base.self.unit.group = MakeGroup(pb::Role::DPS);
  s.base.self.unit.group->subgroup = 1;
  s.base.self.unit.auras.push_back(MakeAura("Living Bomb"));
  s.ext.raid.emplace();

  // Stranger is 1 yd away (not in raid), dead raider is 2 yd away, subgroup 4
  // raider is 4 yd away, own subgroup 1 raider is 25 yd away.
  pb::Unit stranger =
      MakeUnit("0x0000000000000099", "Stranger", 1.0f, {1.0f, 0.0f, 0.0f});
  stranger.reaction = pb::Unit::FRIENDLY;
  stranger.kind = pb::Unit::PLAYER;

  pb::Unit dead_raider =
      MakeUnit("0x0000000000000020", "DeadRaider", 2.0f, {2.0f, 0.0f, 0.0f});
  dead_raider.dead = true;

  pb::Unit sub4_raider =
      MakeUnit("0x0000000000000040", "Sub4Raider", 4.0f, {4.0f, 0.0f, 0.0f});
  pb::Unit sub1_raider =
      MakeUnit("0x0000000000000011", "Sub1Raider", 25.0f, {25.0f, 0.0f, 0.0f});

  s.base.nearby.units = {stranger, dead_raider, sub4_raider, sub1_raider};
  pb::Party& party = s.ext.party.emplace();
  pb::Unit dead_p = dead_raider;
  dead_p.group = MakeGroup(pb::Role::DPS);
  dead_p.group->subgroup = 1;
  pb::Unit sub4_p = sub4_raider;
  sub4_p.group = MakeGroup(pb::Role::DPS);
  sub4_p.group->subgroup = 4;
  pb::Unit sub1_p = sub1_raider;
  sub1_p.group = MakeGroup(pb::Role::DPS);
  sub1_p.group->subgroup = 1;
  party.members = {dead_p, sub4_p, sub1_p};

  const OrderEvalResult r = eval.OnSnapshot(s, T(1));
  ASSERT_TRUE(r.act.has_value());
  // Whole-raid `away group` picks the living subgroup 4 raider (4 yd), not the
  // far subgroup 1 ally, dead raider, or stranger.
  EXPECT_EQ(r.target_guid, "0x0000000000000040");
  const std::optional<pb::Command::Act> wire =
      OrderActToCommand(*r.act, r.it_guid, r.it_name, r.target_guid);
  ASSERT_TRUE(wire.has_value());
  EXPECT_EQ(wire->verb, pb::Command::Act::AWAY);
  EXPECT_EQ(wire->thing, pb::Command::Act::GROUP);
  EXPECT_EQ(wire->it_guid, "0x0000000000000040");

  // Adversarial counter-test: when `target_guid` is empty (e.g. `then` chain
  // advancing via `OnStepDone` or solo), `OrderActToCommand` must NOT leak
  // `self`'s `it_guid` from `Living Bomb on me` into a `GROUP` act.
  const std::optional<pb::Command::Act> wire_no_target =
      OrderActToCommand(*r.act, r.it_guid, r.it_name, /*target_guid=*/"");
  ASSERT_TRUE(wire_no_target.has_value());
  EXPECT_EQ(wire_no_target->thing, pb::Command::Act::GROUP);
  EXPECT_TRUE(wire_no_target->it_guid.empty());
}

TEST(OrderEvaluatorTest, SoloBotDoesNotResolveGroupOrRoleTargets) {
  pb::Snapshot s = MakeBaseSnapshot();
  pb::Unit stranger = MakeUnit("0x0000000000000099", "FriendlyStranger", 3.0f,
                               {3.0f, 0.0f, 0.0f});
  stranger.reaction = pb::Unit::FRIENDLY;
  stranger.kind = pb::Unit::PLAYER;
  s.base.nearby.units = {stranger};

  EXPECT_TRUE(ResolvePlanActTargetGuid(
                  s, MustParse("always -> to tank").steps.front(), T(1))
                  .empty());
  EXPECT_TRUE(ResolvePlanActTargetGuid(
                  s, MustParse("always -> to offtank").steps.front(), T(1))
                  .empty());
  EXPECT_TRUE(ResolvePlanActTargetGuid(
                  s, MustParse("always -> to healer").steps.front(), T(1))
                  .empty());
  EXPECT_TRUE(ResolvePlanActTargetGuid(
                  s, MustParse("always -> away group 10").steps.front(), T(1))
                  .empty());
}

}  // namespace
}  // namespace harness
