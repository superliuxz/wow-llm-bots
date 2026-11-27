// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/leader_travel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "options/options.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

LeaderTravelInput GroupedFollower() {
  LeaderTravelInput in;
  in.in_group = true;
  in.is_leader = false;
  in.bot_alive = true;
  in.bot_in_combat = false;
  in.bot_in_flight = false;
  in.bot_in_bg_or_arena = false;
  in.bot_in_instance = false;
  in.bot_alliance = true;
  in.bot_money_copper = 50000;
  in.bot_map_id = 0;
  in.bot_instance_id = 0;
  in.bot_phase_mask = 1;
  in.bot_pos = {0.0f, 0.0f, 0.0f};
  in.leader_present = true;
  in.leader_alive = true;
  in.leader_in_bg_or_arena = false;
  in.leader_name = "Aldric";
  in.leader_map_id = 0;
  in.leader_instance_id = 0;
  in.leader_phase_mask = 1;
  in.leader_pos = {100.0f, 0.0f, 0.0f};
  return in;
}

TEST(LeaderTravelTest, SameMapFarOffersDirectWalkWithout2500YdCap) {
  LeaderTravelInput in = GroupedFollower();
  in.leader_pos = {120.0f, 0.0f, 0.0f};
  const std::optional<LeaderTravelPlan> near_far = PlanLeaderTravel(in, {});
  ASSERT_TRUE(near_far.has_value());
  EXPECT_EQ(near_far->kind, TravelLegKind::kWalk);
  EXPECT_EQ(near_far->place_id, "place:leader");
  EXPECT_TRUE(near_far->route_label.empty());
  EXPECT_FLOAT_EQ(near_far->dist_yd, 120.0f);
  EXPECT_EQ(near_far->cross_map_hops, 0u);

  // Beyond the 2500 yd hub cap: still offered on the same continent.
  in.leader_pos = {4200.0f, 0.0f, 0.0f};
  const std::optional<LeaderTravelPlan> very_far = PlanLeaderTravel(in, {});
  ASSERT_TRUE(very_far.has_value());
  EXPECT_EQ(very_far->kind, TravelLegKind::kWalk);
  EXPECT_FLOAT_EQ(very_far->dist_yd, 4200.0f);

  const std::optional<pb::OptionFacts::Place> place =
      BuildLeaderTravelPlace(in, {});
  ASSERT_TRUE(place.has_value());
  pb::Snapshot snap;
  OptionFacts facts;
  facts.places.push_back(*place);
  const std::vector<OptionTarget> targets = AddOptions(&snap, facts);
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_EQ(targets[0].kind, OptionTarget::Kind::kGoTo);
  EXPECT_EQ(snap.options[0].text, "Go to Aldric (party leader), 4200 yd N");
}

// Adversarial counter-test: within 25 yd must NOT offer Go to leader so it
// never duplicates close-range Follow (8-25 yd) or clutters melee range (<8
// yd).
TEST(LeaderTravelTest, Within25YdDoesNotDuplicateFollow) {
  LeaderTravelInput in = GroupedFollower();
  for (float d : {0.0f, 5.0f, 8.0f, 15.0f, 24.9f, 25.0f}) {
    in.leader_pos = {d, 0.0f, 0.0f};
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt) << "dist=" << d;
  }
  in.leader_pos = {25.1f, 0.0f, 0.0f};
  EXPECT_TRUE(PlanLeaderTravel(in, {}).has_value());
}

// Adversarial counter-test: solo, group leader, dead, in combat, in flight,
// BG/arena, offline/dead leader, or incompatible phase never offer Go to
// leader.
TEST(LeaderTravelTest, SoloLeaderCombatDeadAndInvalidStatesNeverOffered) {
  {
    LeaderTravelInput in = GroupedFollower();
    in.in_group = false;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.is_leader = true;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.bot_alive = false;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.bot_in_combat = true;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.bot_in_flight = true;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.bot_in_bg_or_arena = true;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.leader_present = false;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.leader_alive = false;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.leader_in_bg_or_arena = true;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
  {
    LeaderTravelInput in = GroupedFollower();
    in.bot_phase_mask = 1;
    in.leader_phase_mask = 2;
    EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
  }
}

TEST(LeaderTravelTest, CrossMapInstanceEntranceRoutesToMatchingWing) {
  // Multi-wing dungeon: two entrances on map 0 into map 189 (Scarlet
  // Monastery).
  std::vector<CrossMapEdge> edges = {
      {.kind = TravelLegKind::kAreaTrigger,
       .from_map_id = 0,
       .from_pos = {2900.0f, -800.0f, 160.0f},
       .to_map_id = 189,
       .to_pos = {1608.0f, -318.0f, 18.0f},  // Library wing
       .trigger_id = 45,
       .label = "Scarlet Monastery entrance"},
      {.kind = TravelLegKind::kAreaTrigger,
       .from_map_id = 0,
       .from_pos = {2915.0f, -820.0f, 160.0f},
       .to_map_id = 189,
       .to_pos = {853.0f, 1321.0f, 18.0f},  // Cathedral wing
       .trigger_id = 610,
       .label = "Scarlet Monastery entrance"},
  };

  LeaderTravelInput in = GroupedFollower();
  in.bot_map_id = 0;
  in.bot_pos = {2850.0f, -800.0f, 160.0f};
  in.leader_map_id = 189;
  in.leader_instance_id = 42;
  in.leader_pos = {860.0f, 1340.0f, 18.0f};  // Inside Cathedral wing.

  const std::optional<LeaderTravelPlan> plan = PlanLeaderTravel(in, edges);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->kind, TravelLegKind::kAreaTrigger);
  EXPECT_EQ(plan->trigger_id, 610u);
  EXPECT_EQ(plan->place_id, "place:leader:at:610");
  EXPECT_EQ(plan->route_label, "Scarlet Monastery entrance");
  EXPECT_EQ(plan->cross_map_hops, 1u);

  const std::optional<pb::OptionFacts::Place> place =
      BuildLeaderTravelPlace(in, edges);
  ASSERT_TRUE(place.has_value());
  pb::Snapshot snap;
  OptionFacts facts;
  facts.places.push_back(*place);
  AddOptions(&snap, facts);
  ASSERT_EQ(snap.options.size(), 2u);
  EXPECT_EQ(snap.options[0].text,
            "Go to Aldric (party leader, via Scarlet Monastery entrance), "
            "68 yd N");
}

TEST(LeaderTravelTest, CrossMapMultiHopRoutesThroughPortalAndEntrance) {
  // Bot in Stormwind (map 0) -> Dark Portal -> Outland (map 530) -> Ramparts
  // (map 543).
  std::vector<CrossMapEdge> edges = {
      {.kind = TravelLegKind::kAreaTrigger,
       .from_map_id = 0,
       .from_pos = {-11900.0f, -3200.0f, -14.0f},
       .to_map_id = 530,
       .to_pos = {-248.0f, 922.0f, 84.0f},
       .trigger_id = 4354,
       .label = "portal to Outland"},
      {.kind = TravelLegKind::kAreaTrigger,
       .from_map_id = 530,
       .from_pos = {-360.0f, 3072.0f, -15.0f},
       .to_map_id = 543,
       .to_pos = {-1355.0f, 1646.0f, 68.0f},
       .trigger_id = 4297,
       .label = "Hellfire Ramparts entrance"},
  };

  LeaderTravelInput in = GroupedFollower();
  in.bot_map_id = 0;
  in.bot_pos = {-11800.0f, -3200.0f, -14.0f};
  in.leader_map_id = 543;
  in.leader_instance_id = 9;
  in.leader_pos = {-1300.0f, 1650.0f, 68.0f};

  const std::optional<LeaderTravelPlan> plan = PlanLeaderTravel(in, edges);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->kind, TravelLegKind::kAreaTrigger);
  EXPECT_EQ(plan->trigger_id, 4354u);
  EXPECT_EQ(plan->cross_map_hops, 2u);
  EXPECT_EQ(plan->route_label, "portal to Outland");
}

TEST(LeaderTravelTest, CrossMapPortalObjectRespectsFaction) {
  std::vector<CrossMapEdge> edges = {
      {.kind = TravelLegKind::kPortalObject,
       .from_map_id = 530,
       .from_pos = {-1830.0f, 5400.0f, -12.0f},
       .to_map_id = 0,
       .to_pos = {-8830.0f, 625.0f, 94.0f},
       .object_entry = 183325,
       .label = "Portal to Stormwind",
       .for_alliance = true,
       .for_horde = false},
      {.kind = TravelLegKind::kPortalObject,
       .from_map_id = 530,
       .from_pos = {-1840.0f, 5420.0f, -12.0f},
       .to_map_id = 0,
       .to_pos = {1770.0f, 60.0f, -46.0f},
       .object_entry = 183327,
       .label = "Portal to Undercity",
       .for_alliance = false,
       .for_horde = true},
  };

  LeaderTravelInput in = GroupedFollower();
  in.bot_map_id = 530;
  in.bot_pos = {-1820.0f, 5410.0f, -12.0f};
  in.leader_map_id = 0;
  in.leader_pos = {-8800.0f, 620.0f, 94.0f};

  // Alliance bot gets Stormwind portal.
  in.bot_alliance = true;
  const std::optional<LeaderTravelPlan> ally = PlanLeaderTravel(in, edges);
  ASSERT_TRUE(ally.has_value());
  EXPECT_EQ(ally->kind, TravelLegKind::kPortalObject);
  EXPECT_EQ(ally->object_entry, 183325u);
  EXPECT_EQ(ally->place_id, "place:leader:portal:183325");

  // Horde bot cannot use the Alliance portal even though it lands closer.
  in.bot_alliance = false;
  const std::optional<LeaderTravelPlan> horde = PlanLeaderTravel(in, edges);
  ASSERT_TRUE(horde.has_value());
  EXPECT_EQ(horde->kind, TravelLegKind::kPortalObject);
  EXPECT_EQ(horde->object_entry, 183327u);
  EXPECT_EQ(horde->place_id, "place:leader:portal:183327");
}

// Adversarial counter-test: when no legitimate cross-map route exists, return
// std::nullopt (never fabricate a cheat teleport).
TEST(LeaderTravelTest, UnreachableMapReturnsNulloptNoCheatTeleport) {
  LeaderTravelInput in = GroupedFollower();
  in.bot_map_id = 0;
  in.leader_map_id = 571;  // Northrend with no edges provided.
  EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);
}

TEST(LeaderTravelTest, DifferentInstanceOnSameMapExitsInstanceFirst) {
  std::vector<CrossMapEdge> edges = {
      {.kind = TravelLegKind::kAreaTrigger,
       .from_map_id = 36,
       .from_pos = {-16.0f, -383.0f, 61.0f},
       .to_map_id = 0,
       .to_pos = {-11208.0f, 1673.0f, 24.0f},
       .trigger_id = 121,
       .label = "instance exit"},
  };

  LeaderTravelInput in = GroupedFollower();
  in.bot_map_id = 36;
  in.bot_instance_id = 10;
  in.bot_in_instance = true;
  in.bot_pos = {-50.0f, -383.0f, 61.0f};
  in.leader_map_id = 36;
  in.leader_instance_id = 11;
  in.leader_pos = {-100.0f, -400.0f, 61.0f};

  const std::optional<LeaderTravelPlan> plan = PlanLeaderTravel(in, edges);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->kind, TravelLegKind::kAreaTrigger);
  EXPECT_EQ(plan->trigger_id, 121u);
  EXPECT_EQ(plan->route_label, "instance exit");
}

TEST(LeaderTravelTest, KnownAffordableFlightPathAcceleratesLongTrip) {
  std::vector<TaxiNodeFact> nodes = {
      {.node_id = 2,
       .map_id = 0,
       .pos = {50.0f, 0.0f, 0.0f},
       .name = "Stormwind"},
      {.node_id = 4,
       .map_id = 0,
       .pos = {1000.0f, 0.0f, 0.0f},
       .name = "Sentinel Hill"},
      {.node_id = 12,
       .map_id = 0,
       .pos = {2000.0f, 0.0f, 0.0f},
       .name = "Darkshire"},
  };
  std::vector<TaxiLinkFact> links = {
      {.from_node = 2, .to_node = 4, .price_copper = 100},
      {.from_node = 4, .to_node = 12, .price_copper = 150},
  };

  LeaderTravelInput in = GroupedFollower();
  in.bot_pos = {0.0f, 0.0f, 0.0f};
  in.leader_pos = {2100.0f, 0.0f, 0.0f};
  in.bot_money_copper = 500;

  const std::optional<LeaderTravelPlan> plan =
      PlanLeaderTravel(in, {}, nodes, links);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->kind, TravelLegKind::kFlightMaster);
  EXPECT_EQ(plan->taxi_nodes, (std::vector<uint32_t>{2, 4, 12}));
  EXPECT_EQ(plan->place_id, "place:leader:taxi:2:4:12");
  EXPECT_EQ(plan->route_label, "flight to Darkshire");

  // Adversarial counter-test: insufficient copper falls back to direct walk
  // (or cheaper partial flight if affordable; with 0 copper -> direct walk).
  in.bot_money_copper = 0;
  const std::optional<LeaderTravelPlan> broke =
      PlanLeaderTravel(in, {}, nodes, links);
  ASSERT_TRUE(broke.has_value());
  EXPECT_EQ(broke->kind, TravelLegKind::kWalk);
  EXPECT_EQ(broke->place_id, "place:leader");
}

TEST(LeaderTravelTest, ParseLeaderTravelPlaceIdRoundTripsAndRejectsGarbage) {
  EXPECT_EQ(ParseLeaderTravelPlaceId("place:leader").kind,
            TravelLegKind::kWalk);
  EXPECT_EQ(ParseLeaderTravelPlaceId("place:razor_hill").kind,
            TravelLegKind::kWalk);

  const LeaderTravelArrival at =
      ParseLeaderTravelPlaceId("place:leader:at:119");
  EXPECT_EQ(at.kind, TravelLegKind::kAreaTrigger);
  EXPECT_EQ(at.trigger_id, 119u);

  const LeaderTravelArrival portal =
      ParseLeaderTravelPlaceId("place:leader:portal:183325");
  EXPECT_EQ(portal.kind, TravelLegKind::kPortalObject);
  EXPECT_EQ(portal.object_entry, 183325u);

  const LeaderTravelArrival taxi =
      ParseLeaderTravelPlaceId("place:leader:taxi:2:4:12");
  EXPECT_EQ(taxi.kind, TravelLegKind::kFlightMaster);
  EXPECT_EQ(taxi.taxi_nodes, (std::vector<uint32_t>{2, 4, 12}));

  // Malformed suffixes degrade safely to kWalk.
  EXPECT_EQ(ParseLeaderTravelPlaceId("place:leader:at:0").kind,
            TravelLegKind::kWalk);
  EXPECT_EQ(ParseLeaderTravelPlaceId("place:leader:at:abc").kind,
            TravelLegKind::kWalk);
  EXPECT_EQ(ParseLeaderTravelPlaceId("place:leader:taxi:2").kind,
            TravelLegKind::kWalk);
  EXPECT_EQ(ParseLeaderTravelPlaceId("place:leader:unknown:1").kind,
            TravelLegKind::kWalk);
}

}  // namespace
}  // namespace llm_brain
