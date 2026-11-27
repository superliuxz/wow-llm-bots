// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/group_roles.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

GroupMemberRoleInput Member(uint64_t guid,
                            uint32_t subgroup = 1,
                            pb::Role lfg = pb::Role::ROLE_UNSPECIFIED) {
  GroupMemberRoleInput m;
  m.guid = guid;
  m.subgroup = subgroup;
  m.lfg_role = lfg;
  m.online_in_world = true;
  m.same_map = true;
  m.alive = true;
  return m;
}

TEST(GroupRolesTest, SoloReturnsEmpty) {
  EXPECT_TRUE(ResolveGroupTankOrder({}).empty());
  EXPECT_EQ(ResolveNearestGroupHealer({}), std::nullopt);
  EXPECT_EQ(ResolveNearestGroupMember(/*bot_guid=*/1, {}), std::nullopt);
}

TEST(GroupRolesTest, FiveManHandFormedFallsBackToSpecRole) {
  auto war = Member(10);
  war.spec_tank = true;
  auto priest = Member(20);
  priest.spec_healer = true;
  priest.dist_yd = 12.0f;
  auto mage = Member(30);
  auto rogue = Member(40);
  auto hunter = Member(50);

  const std::vector<GroupMemberRoleInput> party = {war, priest, mage, rogue,
                                                   hunter};
  EXPECT_EQ(ResolveEffectiveRole(war), pb::Role::TANK);
  EXPECT_EQ(ResolveEffectiveRole(priest), pb::Role::HEALER);
  EXPECT_EQ(ResolveEffectiveRole(mage), pb::Role::DPS);

  const std::vector<uint64_t> tanks = ResolveGroupTankOrder(party);
  ASSERT_EQ(tanks.size(), 1u);
  EXPECT_EQ(tanks[0], 10u);
  EXPECT_EQ(ResolveNearestGroupHealer(party), 20u);
}

TEST(GroupRolesTest, FiveManLfgRoleOverridesSpecAndRespectsNotTank) {
  // Prot warrior queued as DPS in LFG: must NOT be resolved as tank.
  auto prot_as_dps = Member(10, 1, pb::Role::DPS);
  prot_as_dps.spec_tank = true;
  // Ret paladin queued as TANK in LFG: IS resolved as tank.
  auto ret_as_tank = Member(20, 1, pb::Role::TANK);
  // Shadow priest queued as HEALER in LFG: IS resolved as healer.
  auto spriest_as_heal = Member(30, 1, pb::Role::HEALER);
  spriest_as_heal.dist_yd = 15.0f;
  // Tank that chose `Let <name> tank` (not_tank_policy): demoted to DPS even
  // when LFG role says TANK.
  auto yielded_tank = Member(40, 1, pb::Role::TANK);
  yielded_tank.spec_tank = true;
  yielded_tank.not_tank_policy = true;

  EXPECT_EQ(ResolveEffectiveRole(prot_as_dps), pb::Role::DPS);
  EXPECT_FALSE(IsEffectiveTank(prot_as_dps));
  EXPECT_EQ(ResolveEffectiveRole(ret_as_tank), pb::Role::TANK);
  EXPECT_TRUE(IsEffectiveTank(ret_as_tank));
  EXPECT_EQ(ResolveEffectiveRole(spriest_as_heal), pb::Role::HEALER);
  EXPECT_TRUE(IsEffectiveHealer(spriest_as_heal));
  EXPECT_EQ(ResolveEffectiveRole(yielded_tank), pb::Role::DPS);
  EXPECT_FALSE(IsEffectiveTank(yielded_tank));

  const std::vector<GroupMemberRoleInput> party = {
      prot_as_dps, ret_as_tank, spriest_as_heal, yielded_tank};
  EXPECT_EQ(ResolveGroupTankOrder(party), (std::vector<uint64_t>{20u}));
  EXPECT_EQ(ResolveNearestGroupHealer(party), 30u);
}

TEST(GroupRolesTest, HandFormedTenAndTwentyFiveManRaidDeterministicOrder) {
  // Hand-formed 25-man raid (no MT flags, no LFG roles).
  // Tank in subgroup 2 appears earlier in the vector than tank in subgroup 1;
  // deterministic `(subgroup, guid)` order must put subgroup 1 first so server
  // and every bot's harness agree on `tank` vs `offtank`.
  auto ot_sub2 = Member(100, /*subgroup=*/2);
  ot_sub2.spec_tank = true;
  auto mt_sub1_b = Member(200, /*subgroup=*/1);
  mt_sub1_b.spec_tank = true;
  auto mt_sub1_a = Member(150, /*subgroup=*/1);
  mt_sub1_a.spec_tank = true;

  auto heal_far = Member(301, /*subgroup=*/1);
  heal_far.spec_healer = true;
  heal_far.dist_yd = 35.0f;
  auto heal_dead_near = Member(302, /*subgroup=*/2);
  heal_dead_near.spec_healer = true;
  heal_dead_near.alive = false;
  heal_dead_near.dist_yd = 4.0f;
  auto heal_alive_near = Member(303, /*subgroup=*/4);
  heal_alive_near.spec_healer = true;
  heal_alive_near.dist_yd = 11.0f;

  // Fill out to 25 members across subgroups 1..5.
  std::vector<GroupMemberRoleInput> raid = {
      ot_sub2, mt_sub1_b, mt_sub1_a, heal_far, heal_dead_near, heal_alive_near};
  for (uint64_t i = 0; i < 19; ++i) {
    raid.push_back(Member(400 + i, static_cast<uint32_t>((i % 5) + 1)));
  }

  // Tanks ordered by (subgroup, guid): (1, 150), (1, 200), (2, 100).
  EXPECT_EQ(ResolveGroupTankOrder(raid),
            (std::vector<uint64_t>{150u, 200u, 100u}));
  // Living healer in subgroup 4 (11 yd) beats dead healer (4 yd) and far
  // living healer (35 yd).
  EXPECT_EQ(ResolveNearestGroupHealer(raid), 303u);

  // If all healers die, falls back to nearest dead healer (e.g. for Rebirth).
  raid[3].alive = false;
  raid[5].alive = false;
  EXPECT_EQ(ResolveNearestGroupHealer(raid), 302u);
}

TEST(GroupRolesTest, MainTankFlagsTakePrecedenceAndPreserveRaidSlotOrder) {
  // LFG raid where raid leader marked slot 0 (subgroup 3, guid 900) and slot 1
  // (subgroup 1, guid 100) as Main Tanks, plus a spec tank in subgroup 1.
  auto mt_first = Member(900, /*subgroup=*/3, pb::Role::DPS);
  mt_first.main_tank_flag = true;
  auto mt_second = Member(100, /*subgroup=*/1, pb::Role::TANK);
  mt_second.main_tank_flag = true;
  auto spec_ot = Member(50, /*subgroup=*/1);
  spec_ot.spec_tank = true;
  auto offline_mt = Member(10, /*subgroup=*/1);
  offline_mt.main_tank_flag = true;
  offline_mt.online_in_world = false;
  auto other_map_tank = Member(20, /*subgroup=*/1, pb::Role::TANK);
  other_map_tank.same_map = false;

  const std::vector<GroupMemberRoleInput> raid = {mt_first, mt_second, spec_ot,
                                                  offline_mt, other_map_tank};
  // Main tank flags preserve slot order (900, 100), then spec tank (50).
  // Offline and other-map tanks are excluded.
  EXPECT_EQ(ResolveGroupTankOrder(raid),
            (std::vector<uint64_t>{900u, 100u, 50u}));
}

TEST(GroupRolesTest, OfflineMemberKeepsLfgRoleWithoutSpecFallback) {
  auto offline_no_lfg = Member(10);
  offline_no_lfg.online_in_world = false;
  offline_no_lfg.spec_tank = true;
  EXPECT_EQ(ResolveEffectiveRole(offline_no_lfg), pb::Role::ROLE_UNSPECIFIED);

  auto offline_lfg_heal = Member(20, 1, pb::Role::HEALER);
  offline_lfg_heal.online_in_world = false;
  EXPECT_EQ(ResolveEffectiveRole(offline_lfg_heal), pb::Role::HEALER);
}

TEST(GroupRolesTest, AwayGroupScopeCoversWholeRaidNotJustSubgroup) {
  // Bot (guid 1) is in subgroup 1; nearest raid member is guid 22 in subgroup
  // 4 (3 yd), while subgroup 1 ally (guid 2) is 25 yd away, a dead raid member
  // is 1 yd away, and a friendly non-group player is 2 yd away.
  const std::vector<NearbyGroupCandidate> candidates = {
      {.guid = 1, .in_same_group = true, .alive = true, .dist_yd = 0.0f},
      {.guid = 99, .in_same_group = false, .alive = true, .dist_yd = 2.0f},
      {.guid = 15, .in_same_group = true, .alive = false, .dist_yd = 1.0f},
      {.guid = 2, .in_same_group = true, .alive = true, .dist_yd = 25.0f},
      {.guid = 22, .in_same_group = true, .alive = true, .dist_yd = 3.0f},
  };
  EXPECT_EQ(ResolveNearestGroupMember(/*bot_guid=*/1, candidates), 22u);

  // Solo / only non-group players nearby -> nullopt (never treats strangers as
  // `group`).
  const std::vector<NearbyGroupCandidate> solo = {
      {.guid = 1, .in_same_group = true, .alive = true, .dist_yd = 0.0f},
      {.guid = 99, .in_same_group = false, .alive = true, .dist_yd = 2.0f},
  };
  EXPECT_EQ(ResolveNearestGroupMember(/*bot_guid=*/1, solo), std::nullopt);
}

}  // namespace
}  // namespace llm_brain
