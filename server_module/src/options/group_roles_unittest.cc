// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/group_roles.h"

#include <cstdint>
#include <optional>
#include <string_view>
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

TEST(GroupRolesTest, ResolveTankStrategyModeRespectsGroupRoleAndHumanTank) {
  // Solo tank-spec bot keeps tank strategies; solo DPS bot leaves them alone.
  {
    TankStrategyInput solo_tank;
    solo_tank.in_group = false;
    solo_tank.spec_tank = true;
    EXPECT_EQ(ResolveTankStrategyMode(solo_tank), TankStrategyMode::kTank);

    TankStrategyInput solo_dps;
    solo_dps.in_group = false;
    EXPECT_EQ(ResolveTankStrategyMode(solo_dps), TankStrategyMode::kNone);
  }

  // Tank-spec bot in a hand-formed 5-man with a human tank must suppress
  // `tank assist` so it does not fight the human tank for aggro.
  {
    TankStrategyInput with_human_tank;
    with_human_tank.in_group = true;
    with_human_tank.is_raid = false;
    with_human_tank.spec_tank = true;
    with_human_tank.has_tank_strategy = true;
    with_human_tank.group_has_human_tank = true;
    EXPECT_EQ(ResolveTankStrategyMode(with_human_tank),
              TankStrategyMode::kSuppress);

    // Recovery: once the human tank leaves (or bot goes solo), even if
    // `has_tank_strategy` was stripped earlier, `spec_tank` restores tanking.
    with_human_tank.has_tank_strategy = false;
    with_human_tank.group_has_human_tank = false;
    EXPECT_EQ(ResolveTankStrategyMode(with_human_tank),
              TankStrategyMode::kTank);
  }

  // Adversarial counter-test: tank-spec bot in a 5-man with a human DPS (not
  // tank) stays the tank.
  {
    TankStrategyInput with_human_dps;
    with_human_dps.in_group = true;
    with_human_dps.is_raid = false;
    with_human_dps.spec_tank = true;
    with_human_dps.group_has_human_tank = false;
    EXPECT_EQ(ResolveTankStrategyMode(with_human_dps), TankStrategyMode::kTank);
  }

  // Adversarial counter-test: tank-spec bot in a raid with a human tank stays
  // an active off-tank unless `not_tank_policy` or LFG DPS role is set.
  {
    TankStrategyInput raid_ot;
    raid_ot.in_group = true;
    raid_ot.is_raid = true;
    raid_ot.spec_tank = true;
    raid_ot.group_has_human_tank = true;
    EXPECT_EQ(ResolveTankStrategyMode(raid_ot), TankStrategyMode::kTank);

    raid_ot.not_tank_policy = true;
    EXPECT_EQ(ResolveTankStrategyMode(raid_ot), TankStrategyMode::kSuppress);
  }

  // LFG role overrides talent spec in both directions.
  {
    TankStrategyInput prot_as_dps;
    prot_as_dps.in_group = true;
    prot_as_dps.spec_tank = true;
    prot_as_dps.lfg_role = pb::Role::DPS;
    EXPECT_EQ(ResolveTankStrategyMode(prot_as_dps),
              TankStrategyMode::kSuppress);

    TankStrategyInput prot_as_healer = prot_as_dps;
    prot_as_healer.lfg_role = pb::Role::HEALER;
    EXPECT_EQ(ResolveTankStrategyMode(prot_as_healer),
              TankStrategyMode::kSuppress);

    TankStrategyInput dps_as_dps;
    dps_as_dps.in_group = true;
    dps_as_dps.lfg_role = pb::Role::DPS;
    EXPECT_EQ(ResolveTankStrategyMode(dps_as_dps), TankStrategyMode::kNone);

    // Explicitly assigned LFG Tank tanks even if a human in the party happens
    // to be tank-spec (queued as DPS).
    TankStrategyInput lfg_tank;
    lfg_tank.in_group = true;
    lfg_tank.spec_tank = false;
    lfg_tank.lfg_role = pb::Role::TANK;
    lfg_tank.group_has_human_tank = true;
    EXPECT_EQ(ResolveTankStrategyMode(lfg_tank), TankStrategyMode::kTank);
  }
}

TEST(GroupRolesTest, CollectGroupLeaderControlsBotLedWithHumanAndCounterTests) {
  auto has_guid = [](const std::vector<pb::OptionFacts_Action>& opts,
                     std::string_view guid) {
    for (const auto& o : opts) {
      if (o.guid == guid) {
        return true;
      }
    }
    return false;
  };

  // Bot-led 5-man with a connected human outside a dungeon (Normal, Group
  // Loot): offers promote, Heroic toggle, other loot methods, and reset.
  GroupLeaderControlsInput in;
  in.in_group = true;
  in.self_is_leader = true;
  in.alive = true;
  in.in_combat = false;
  in.is_raid = false;
  in.is_lfg_restricted = false;
  in.all_members_in_world = true;
  in.any_member_in_dungeon = false;
  in.current_heroic = false;
  in.current_loot_method = 2;  // GROUP_LOOT
  in.human_members.push_back({"0x0000000000000042", "Arthas", true});

  {
    const auto opts = CollectGroupLeaderControls(in);
    EXPECT_TRUE(has_guid(opts, "group:promote:0x0000000000000042"));
    EXPECT_TRUE(has_guid(opts, "group:difficulty:1"));
    EXPECT_FALSE(has_guid(opts, "group:difficulty:0"));
    EXPECT_TRUE(has_guid(opts, "group:loot:0"));
    EXPECT_FALSE(has_guid(opts, "group:loot:2"));  // Already Group Loot.
    EXPECT_TRUE(has_guid(opts, "group:loot:4"));
    EXPECT_TRUE(has_guid(opts, "group:reset"));
  }

  // Heroic + Free-for-All flips to Normal and offers Group Loot + NBG.
  {
    auto heroic = in;
    heroic.current_heroic = true;
    heroic.current_loot_method = 0;  // FREE_FOR_ALL
    const auto opts = CollectGroupLeaderControls(heroic);
    EXPECT_TRUE(has_guid(opts, "group:difficulty:0"));
    EXPECT_FALSE(has_guid(opts, "group:difficulty:1"));
    EXPECT_FALSE(has_guid(opts, "group:loot:0"));
    EXPECT_TRUE(has_guid(opts, "group:loot:2"));
    EXPECT_TRUE(has_guid(opts, "group:loot:4"));
  }

  // LFG-restricted bot-led group: STILL offers Promote human, but blocks
  // Heroic toggle, loot method, and reset.
  {
    auto lfg = in;
    lfg.is_lfg_restricted = true;
    const auto opts = CollectGroupLeaderControls(lfg);
    ASSERT_EQ(opts.size(), 1u);
    EXPECT_EQ(opts[0].guid, "group:promote:0x0000000000000042");
  }

  // Member inside a dungeon: offers Promote + loot method, blocks Heroic
  // toggle and reset.
  {
    auto inside = in;
    inside.any_member_in_dungeon = true;
    const auto opts = CollectGroupLeaderControls(inside);
    EXPECT_TRUE(has_guid(opts, "group:promote:0x0000000000000042"));
    EXPECT_TRUE(has_guid(opts, "group:loot:0"));
    EXPECT_FALSE(has_guid(opts, "group:difficulty:1"));
    EXPECT_FALSE(has_guid(opts, "group:reset"));
  }

  // Adversarial counter-tests: all-bot group, non-leader, solo, in combat,
  // dead -> empty.
  {
    auto all_bot = in;
    all_bot.human_members.clear();
    EXPECT_TRUE(CollectGroupLeaderControls(all_bot).empty());

    auto not_leader = in;
    not_leader.self_is_leader = false;
    EXPECT_TRUE(CollectGroupLeaderControls(not_leader).empty());

    auto solo = in;
    solo.in_group = false;
    EXPECT_TRUE(CollectGroupLeaderControls(solo).empty());

    auto combat = in;
    combat.in_combat = true;
    EXPECT_TRUE(CollectGroupLeaderControls(combat).empty());

    auto dead = in;
    dead.alive = false;
    EXPECT_TRUE(CollectGroupLeaderControls(dead).empty());
  }
}

TEST(GroupRolesTest, OfftankSelectedByMainAssistAndLfgRoleNotGroupOrder) {
  auto sub1_spec_tank = Member(10, /*subgroup=*/1);
  sub1_spec_tank.spec_tank = true;

  auto sub2_mt = Member(20, /*subgroup=*/2);
  sub2_mt.spec_tank = true;
  sub2_mt.main_tank_flag = true;

  auto sub3_ma_ot = Member(30, /*subgroup=*/3);
  sub3_ma_ot.spec_tank = true;
  sub3_ma_ot.main_assist_flag = true;

  auto sub1_dps_ma = Member(5, /*subgroup=*/1, pb::Role::DPS);
  sub1_dps_ma.main_assist_flag = true;

  auto sub2_lfg_tank = Member(25, /*subgroup=*/2, pb::Role::TANK);

  const std::vector<GroupMemberRoleInput> raid = {
      sub1_dps_ma, sub1_spec_tank, sub2_mt, sub2_lfg_tank, sub3_ma_ot};
  // Order: Main Tank (20), Main Assist tank (30), LFG tank (25), spec tank
  // (10). DPS Main Assist (5) is never elevated to tank/offtank.
  EXPECT_EQ(ResolveGroupTankOrder(raid),
            (std::vector<uint64_t>{20u, 30u, 25u, 10u}));
}

}  // namespace
}  // namespace llm_brain
