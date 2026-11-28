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

  // Open-world `place:travel:*` and transport round-trip (including negative
  // coordinates).
  const LeaderTravelArrival travel_at =
      ParseLeaderTravelPlaceId("place:travel:at:362");
  EXPECT_EQ(travel_at.kind, TravelLegKind::kAreaTrigger);
  EXPECT_EQ(travel_at.trigger_id, 362u);

  const LeaderTravelArrival travel_portal =
      ParseLeaderTravelPlaceId("place:travel:portal:183325");
  EXPECT_EQ(travel_portal.kind, TravelLegKind::kPortalObject);
  EXPECT_EQ(travel_portal.object_entry, 183325u);

  const LeaderTravelArrival tr = ParseLeaderTravelPlaceId(
      "place:travel:transport:176310:571:-2230:1705:5");
  EXPECT_EQ(tr.kind, TravelLegKind::kTransport);
  EXPECT_EQ(tr.object_entry, 176310u);
  EXPECT_EQ(tr.dest_map_id, 571u);
  EXPECT_FLOAT_EQ(tr.dest_pos.x, -2230.0f);
  EXPECT_FLOAT_EQ(tr.dest_pos.y, 1705.0f);
  EXPECT_FLOAT_EQ(tr.dest_pos.z, 5.0f);

  const LeaderTravelArrival ldr_tr =
      ParseLeaderTravelPlaceId("place:leader:transport:181688:571:585:-5100:2");
  EXPECT_EQ(ldr_tr.kind, TravelLegKind::kTransport);
  EXPECT_EQ(ldr_tr.object_entry, 181688u);
  EXPECT_EQ(ldr_tr.dest_map_id, 571u);

  // Adversarial counter-tests: malformed transport IDs degrade to kWalk.
  for (const char* bad : {
           "place:travel:transport:0:571:1:2:3",
           "place:travel:transport:176310:571:1:2",
           "place:travel:transport:176310:571:1:2:3:4",
           "place:travel:transport:176310:571:abc:2:3",
           "place:other:transport:176310:571:1:2:3",
       }) {
    EXPECT_EQ(ParseLeaderTravelPlaceId(bad).kind, TravelLegKind::kWalk) << bad;
  }
}

TEST(LeaderTravelTest, MultiHopTaxiRoutesExposeMoreThanFourAndRespectBudget) {
  // Linear + branching Eastern Kingdoms flight graph with 7 reachable nodes.
  std::vector<TaxiNodeFact> nodes = {
      {.node_id = 2, .map_id = 0, .pos = {0, 0, 0}, .name = "Stormwind"},
      {.node_id = 4, .map_id = 0, .pos = {500, 0, 0}, .name = "Sentinel Hill"},
      {.node_id = 12, .map_id = 0, .pos = {900, 0, 0}, .name = "Darkshire"},
      {.node_id = 14, .map_id = 0, .pos = {1300, 0, 0}, .name = "Lakeshire"},
      {.node_id = 19, .map_id = 0, .pos = {1800, 0, 0}, .name = "Booty Bay"},
      {.node_id = 6, .map_id = 0, .pos = {2200, 0, 0}, .name = "Ironforge"},
      {.node_id = 7, .map_id = 0, .pos = {2700, 0, 0}, .name = "Menethil"},
      {.node_id = 8, .map_id = 0, .pos = {3200, 0, 0}, .name = "Thelsamar"},
  };
  std::vector<TaxiLinkFact> links = {
      {.from_node = 2, .to_node = 4, .price_copper = 50},
      {.from_node = 4, .to_node = 12, .price_copper = 60},
      {.from_node = 2, .to_node = 14, .price_copper = 80},
      {.from_node = 12, .to_node = 19, .price_copper = 100},
      {.from_node = 2, .to_node = 6, .price_copper = 120},
      {.from_node = 6, .to_node = 7, .price_copper = 90},
      {.from_node = 6, .to_node = 8, .price_copper = 70},
      // Expensive direct shortcut to Booty Bay (300c) vs 3-hop cheaper (210c).
      {.from_node = 2, .to_node = 19, .price_copper = 300},
  };

  const std::vector<TaxiRouteOption> all =
      ComputeReachableTaxiRoutes(2, /*bot_money_copper=*/500,
                                 /*price_discount=*/1.0f, nodes, links);
  // All 7 destinations exposed (not capped at 4), nearest first.
  ASSERT_EQ(all.size(), 7u);
  EXPECT_EQ(all[0].dest_name, "Sentinel Hill");
  EXPECT_EQ(all[0].nodes, (std::vector<uint32_t>{2, 4}));
  EXPECT_EQ(all[1].dest_name, "Darkshire");
  EXPECT_EQ(all[1].nodes, (std::vector<uint32_t>{2, 4, 12}));
  EXPECT_EQ(all[1].total_cost_copper, 110u);
  EXPECT_EQ(all[3].dest_name, "Booty Bay");
  // Picks cheaper 3-hop path (50+60+100=210) over 300c direct link.
  EXPECT_EQ(all[3].nodes, (std::vector<uint32_t>{2, 4, 12, 19}));
  EXPECT_EQ(all[3].total_cost_copper, 210u);

  // FindTaxiNodePath reconstructs the exact hop-by-hop sequence.
  EXPECT_EQ(FindTaxiNodePath(2, 19, 500, 1.0f, nodes, links),
            (std::vector<uint32_t>{2, 4, 12, 19}));
  // Reputation discount (10% off): ceil(210 * 0.9) = 189c fits 190c budget.
  EXPECT_EQ(FindTaxiNodePath(2, 19, 190, 0.9f, nodes, links),
            (std::vector<uint32_t>{2, 4, 12, 19}));

  // Adversarial counter-test: insufficient copper excludes Booty Bay (210c)
  // while keeping cheaper destinations; invalid/NaN/negative discount clamps
  // to 1.0 (never grants free flights).
  EXPECT_TRUE(FindTaxiNodePath(2, 19, 200, 1.0f, nodes, links).empty());
  EXPECT_TRUE(FindTaxiNodePath(2, 19, 200, 0.0f, nodes, links).empty());
  EXPECT_TRUE(FindTaxiNodePath(2, 19, 200, -0.5f, nodes, links).empty());
  EXPECT_EQ(FindTaxiNodePath(2, 2, 500, 1.0f, nodes, links).size(), 0u);

  // Adversarial counter-test: removing intermediate node 12 from known_nodes
  // prevents routing through undiscovered nodes (falls back to 300c direct).
  std::vector<TaxiNodeFact> without_darkshire;
  for (const TaxiNodeFact& n : nodes) {
    if (n.node_id != 12) {
      without_darkshire.push_back(n);
    }
  }
  EXPECT_EQ(FindTaxiNodePath(2, 19, 500, 1.0f, without_darkshire, links),
            (std::vector<uint32_t>{2, 19}));
  EXPECT_TRUE(
      FindTaxiNodePath(2, 19, 250, 1.0f, without_darkshire, links).empty());
}

TEST(LeaderTravelTest, IslandAndNorthrendTravelOptionsWorkForSoloAndGrouped) {
  // Teldrassil (map 1): Darnassus -> Rut'theran Village (same-map portal) ->
  // Auberdine (same-map boat) -> Stormwind Harbor (cross-map boat to map 0) ->
  // Valiance Keep (cross-map boat to Northrend map 571).
  // Plus Horde zeppelin Orgrimmar (map 1) -> Warsong Hold (map 571),
  // Azuremyst boat Valaar's Berth (map 530) -> Auberdine (map 1), and a
  // dungeon entrance that must NOT leak into solo travel places.
  std::vector<CrossMapEdge> edges = {
      {.kind = TravelLegKind::kAreaTrigger,
       .from_map_id = 1,
       .from_pos = {9946.0f, 2610.0f, 1316.0f},
       .to_map_id = 1,
       .to_pos = {8785.0f, 967.0f, 30.0f},
       .trigger_id = 362,
       .label = "portal to Rut'theran Village",
       .dest_name = "Rut'theran Village",
       .for_alliance = true,
       .for_horde = true},
      {.kind = TravelLegKind::kTransport,
       .from_map_id = 1,
       .from_pos = {8300.0f, 1170.0f, 5.0f},
       .to_map_id = 1,
       .to_pos = {6550.0f, 930.0f, 5.0f},
       .object_entry = 176244,
       .label = "transport to Auberdine",
       .dest_name = "Auberdine",
       .for_alliance = true,
       .for_horde = false},
      {.kind = TravelLegKind::kTransport,
       .from_map_id = 1,
       .from_pos = {6580.0f, 950.0f, 5.0f},
       .to_map_id = 0,
       .to_pos = {-8290.0f, 1400.0f, 5.0f},
       .object_entry = 176310,
       .label = "transport to Stormwind Harbor",
       .dest_name = "Stormwind Harbor",
       .dest_continent = "Eastern Kingdoms",
       .for_alliance = true,
       .for_horde = false},
      {.kind = TravelLegKind::kTransport,
       .from_map_id = 530,
       .from_pos = {-4260.0f, -11330.0f, 5.0f},
       .to_map_id = 1,
       .to_pos = {6540.0f, 920.0f, 5.0f},
       .object_entry = 181646,
       .label = "transport to Auberdine",
       .dest_name = "Auberdine",
       .dest_continent = "Kalimdor",
       .for_alliance = true,
       .for_horde = false},
      {.kind = TravelLegKind::kTransport,
       .from_map_id = 0,
       .from_pos = {-8300.0f, 1420.0f, 5.0f},
       .to_map_id = 571,
       .to_pos = {2230.0f, 5130.0f, 5.0f},
       .object_entry = 190536,
       .label = "transport to Valiance Keep",
       .dest_name = "Valiance Keep",
       .dest_continent = "Northrend",
       .for_alliance = true,
       .for_horde = false},
      {.kind = TravelLegKind::kTransport,
       .from_map_id = 1,
       .from_pos = {1180.0f, -4150.0f, 50.0f},
       .to_map_id = 571,
       .to_pos = {2840.0f, 6190.0f, 120.0f},
       .object_entry = 186238,
       .label = "transport to Warsong Hold",
       .dest_name = "Warsong Hold",
       .dest_continent = "Northrend",
       .for_alliance = false,
       .for_horde = true},
      {.kind = TravelLegKind::kAreaTrigger,
       .from_map_id = 0,
       .from_pos = {-8770.0f, 840.0f, 88.0f},
       .to_map_id = 34,
       .to_pos = {48.0f, 0.0f, -16.0f},
       .trigger_id = 101,
       .label = "The Stockade entrance",
       .dest_name = "The Stockade",
       .for_alliance = true,
       .for_horde = true,
       .involves_instance = true},
  };

  // Solo Sylva in Darnassus sees portal to Rut'theran Village first (nearest)
  // and the Rut'theran boat to Auberdine second.
  const std::vector<pb::OptionFacts::Place> sylva_places =
      BuildOpenWorldTravelPlaces(1, {9900.0f, 2550.0f, 1316.0f},
                                 /*bot_alliance=*/true, edges);
  ASSERT_EQ(sylva_places.size(), 2u);
  EXPECT_EQ(sylva_places[0].id, "place:travel:at:362");
  EXPECT_EQ(sylva_places[0].name, "Rut'theran Village");
  EXPECT_EQ(sylva_places[0].services, "portal");
  EXPECT_EQ(sylva_places[1].name, "Auberdine");
  EXPECT_EQ(sylva_places[1].services, "boat/zeppelin");

  // Solo Talaan on Azuremyst Isle sees boat to Auberdine (Kalimdor).
  const std::vector<pb::OptionFacts::Place> talaan_places =
      BuildOpenWorldTravelPlaces(530, {-4000.0f, -11500.0f, 10.0f},
                                 /*bot_alliance=*/true, edges);
  ASSERT_EQ(talaan_places.size(), 1u);
  EXPECT_EQ(talaan_places[0].name, "Auberdine");
  EXPECT_EQ(talaan_places[0].services, "boat/zeppelin to Kalimdor");
  EXPECT_EQ(ParseLeaderTravelPlaceId(talaan_places[0].id).kind,
            TravelLegKind::kTransport);

  // Solo Alliance bot in Stormwind sees boat to Valiance Keep (Northrend),
  // and does NOT see The Stockade dungeon entrance.
  const std::vector<pb::OptionFacts::Place> sw_places =
      BuildOpenWorldTravelPlaces(0, {-8800.0f, 650.0f, 95.0f},
                                 /*bot_alliance=*/true, edges);
  ASSERT_EQ(sw_places.size(), 1u);
  EXPECT_EQ(sw_places[0].name, "Valiance Keep");
  EXPECT_EQ(sw_places[0].services, "boat/zeppelin to Northrend");

  // Adversarial counter-test: Horde bot near Stormwind does NOT see Alliance
  // boat, and Alliance bot near Orgrimmar does NOT see Horde zeppelin.
  EXPECT_TRUE(BuildOpenWorldTravelPlaces(0, {-8310.0f, 1410.0f, 5.0f},
                                         /*bot_alliance=*/false, edges)
                  .empty());
  EXPECT_TRUE(BuildOpenWorldTravelPlaces(1, {1200.0f, -4160.0f, 50.0f},
                                         /*bot_alliance=*/true, edges)
                  .empty());
  const std::vector<pb::OptionFacts::Place> org_horde =
      BuildOpenWorldTravelPlaces(1, {1200.0f, -4160.0f, 50.0f},
                                 /*bot_alliance=*/false, edges);
  ASSERT_EQ(org_horde.size(), 1u);
  EXPECT_EQ(org_horde[0].name, "Warsong Hold");

  // Grouped Sylva in Darnassus routes 3 hops to leader in Stormwind (map 0):
  // Darnassus -> Rut'theran Village -> Auberdine -> Stormwind Harbor.
  LeaderTravelInput grouped = GroupedFollower();
  grouped.bot_alliance = true;
  grouped.bot_map_id = 1;
  grouped.bot_pos = {9900.0f, 2550.0f, 1316.0f};
  grouped.leader_map_id = 0;
  grouped.leader_pos = {-8830.0f, 625.0f, 94.0f};
  const std::optional<LeaderTravelPlan> plan = PlanLeaderTravel(grouped, edges);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->kind, TravelLegKind::kAreaTrigger);
  EXPECT_EQ(plan->trigger_id, 362u);
  EXPECT_EQ(plan->cross_map_hops, 3u);
}

TEST(LeaderTravelTest,
     LeaderOnTaxiSuppressesMidRoutePursuitOrTargetsFlightMaster) {
  LeaderTravelInput in = GroupedFollower();
  in.bot_pos = {0.0f, 0.0f, 0.0f};
  // Leader is mid-air 900 yd away on a taxi flight to Sentinel Hill (2000 yd).
  in.leader_pos = {900.0f, 0.0f, 250.0f};
  in.leader_in_flight = true;
  in.leader_taxi_dest_node = 4;
  in.leader_taxi_dest_pos = pb::Vec3{2000.0f, 0.0f, 40.0f};
  in.leader_taxi_dest_name = "Sentinel Hill";

  // Without known taxi nodes, must NOT walk toward mid-air coordinates.
  EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);

  // With known affordable taxi route to the leader's destination, target the
  // flight master rather than walking to transient mid-air coordinates.
  const std::vector<TaxiNodeFact> nodes = {
      {2, 0, {50.0f, 0.0f, 0.0f}, "Stormwind"},
      {4, 0, {2000.0f, 0.0f, 40.0f}, "Sentinel Hill"},
  };
  const std::vector<TaxiLinkFact> links = {
      {2, 4, 500},
  };
  const std::optional<LeaderTravelPlan> plan =
      PlanLeaderTravel(in, {}, nodes, links);
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->kind, TravelLegKind::kFlightMaster);
  EXPECT_EQ(plan->taxi_nodes, (std::vector<uint32_t>{2, 4}));
  EXPECT_FLOAT_EQ(plan->leg_dest.x, 50.0f);

  // Counter-test: if bot cannot afford the taxi, wait for arrival (nullopt).
  in.bot_money_copper = 100;
  EXPECT_EQ(PlanLeaderTravel(in, {}, nodes, links), std::nullopt);

  // Counter-test: once leader lands, normal travel resumes.
  in.leader_in_flight = false;
  in.leader_pos = {2000.0f, 0.0f, 40.0f};
  const std::optional<LeaderTravelPlan> landed =
      PlanLeaderTravel(in, {}, nodes, links);
  ASSERT_TRUE(landed.has_value());
  EXPECT_EQ(landed->kind, TravelLegKind::kWalk);
  EXPECT_FLOAT_EQ(landed->leg_dest.x, 2000.0f);
}

TEST(LeaderTravelTest, LeaderOnBoatSuppressesMidOceanWalkOrTargetsDock) {
  LeaderTravelInput in = GroupedFollower();
  in.bot_map_id = 0;
  in.bot_pos = {-8350.0f, 1400.0f, 5.0f};
  // Leader is mid-ocean on Stormwind -> Valiance Keep boat (entry 190536).
  in.leader_map_id = 0;
  in.leader_pos = {-6000.0f, 2500.0f, 0.0f};
  in.leader_on_transport = true;
  in.leader_transport_entry = 190536;
  in.bot_on_same_transport = false;

  // Without a matching dock edge, must NOT walk into the ocean.
  EXPECT_EQ(PlanLeaderTravel(in, {}), std::nullopt);

  // With the matching boat edge on bot's map, target the dock (kTransport),
  // never the leader's mid-ocean coordinates.
  const std::vector<CrossMapEdge> edges = {
      CrossMapEdge{
          .kind = TravelLegKind::kTransport,
          .from_map_id = 0,
          .from_pos = {-8295.0f, 1405.0f, 5.0f},
          .to_map_id = 571,
          .to_pos = {2213.0f, 5224.0f, 12.0f},
          .object_entry = 190536,
          .label = "boat to Valiance Keep",
          .dest_name = "Valiance Keep",
          .dest_continent = "Northrend",
          .for_alliance = true,
          .for_horde = false,
      },
  };
  const std::optional<LeaderTravelPlan> dock = PlanLeaderTravel(in, edges);
  ASSERT_TRUE(dock.has_value());
  EXPECT_EQ(dock->kind, TravelLegKind::kTransport);
  EXPECT_EQ(dock->object_entry, 190536u);
  EXPECT_FLOAT_EQ(dock->leg_dest.x, -8295.0f);

  // Counter-test: when bot is already aboard the same boat, no Go-to offered.
  in.bot_on_same_transport = true;
  EXPECT_EQ(PlanLeaderTravel(in, edges), std::nullopt);
}

TEST(LeaderTravelTest, LeaderTravelWatchWakesOnMapTaxiBoatAndRespectsCombat) {
  LeaderTravelWatch watch;
  LeaderTravelState s;
  s.leader_guid = 1001;
  s.leader_name = "Aldric";
  s.map_id = 0;
  s.instance_id = 0;
  s.zone_name = "Stormwind City";

  // First observation seeds state without spurious wake.
  EXPECT_EQ(UpdateLeaderTravelWatch(&watch, s, /*bot_map_id=*/0,
                                    /*bot_instance_id=*/0,
                                    /*bot_on_same_transport=*/false,
                                    /*bot_in_combat=*/false),
            std::nullopt);
  // Repeated identical state does not wake.
  EXPECT_EQ(UpdateLeaderTravelWatch(&watch, s, 0, 0, false, false),
            std::nullopt);

  // Leader boards a taxi flight -> wakes with destination.
  s.in_flight = true;
  s.taxi_dest_name = "Sentinel Hill";
  const std::optional<std::string> taxi_wake =
      UpdateLeaderTravelWatch(&watch, s, 0, 0, false, false);
  ASSERT_TRUE(taxi_wake.has_value());
  EXPECT_EQ(*taxi_wake,
            "leader travel: Aldric took a flight path to Sentinel Hill");
  // Edge-triggered: staying in flight does not wake again.
  EXPECT_EQ(UpdateLeaderTravelWatch(&watch, s, 0, 0, false, false),
            std::nullopt);

  // Leader lands -> arrival wake.
  s.in_flight = false;
  s.taxi_dest_name.clear();
  s.zone_name = "Westfall";
  const std::optional<std::string> land_wake =
      UpdateLeaderTravelWatch(&watch, s, 0, 0, false, false);
  ASSERT_TRUE(land_wake.has_value());
  EXPECT_EQ(*land_wake, "leader travel: Aldric arrived in Westfall");

  // Leader boards a boat/zeppelin -> wakes follower not on the boat.
  s.on_transport = true;
  s.transport_entry = 190536;
  const std::optional<std::string> boat_wake =
      UpdateLeaderTravelWatch(&watch, s, 0, 0, false, false);
  ASSERT_TRUE(boat_wake.has_value());
  EXPECT_EQ(*boat_wake, "leader travel: Aldric boarded a boat/zeppelin");

  // Leader crosses map to Northrend (map 571) -> wakes follower still on map 0.
  s.map_id = 571;
  s.zone_name = "Borean Tundra";
  const std::optional<std::string> map_wake =
      UpdateLeaderTravelWatch(&watch, s, 0, 0, false, false);
  ASSERT_TRUE(map_wake.has_value());
  EXPECT_EQ(*map_wake, "leader travel: Aldric moved to Borean Tundra");

  // Adversarial counter-test: P0 combat priority suppresses travel wake while
  // bot is in combat.
  s.map_id = 574;
  s.instance_id = 42;
  s.zone_name = "Utgarde Keep";
  EXPECT_EQ(UpdateLeaderTravelWatch(&watch, s, 0, 0, false,
                                    /*bot_in_combat=*/true),
            std::nullopt);

  // Adversarial counter-test: reset on solo / disconnect clears state cleanly
  // and does not emit a spurious wake on reconnect.
  watch.Reset();
  EXPECT_EQ(UpdateLeaderTravelWatch(&watch, s, 0, 0, false, false),
            std::nullopt);
}

TEST(LeaderTravelTest, CheckFollowLeaderReportsCrossMapTaxiAndBoatHonestly) {
  FollowLeaderCheckInput in;
  in.leader_present = true;
  in.leader_alive = true;
  in.leader_name = "Aldric";
  in.bot_map_id = 0;
  in.leader_map_id = 0;

  EXPECT_EQ(CheckFollowLeader(in).status,
            FollowLeaderStatus::kCanFollowDirectly);

  // Cross-map mismatch: must NOT report direct follow.
  in.leader_map_id = 571;
  {
    const FollowLeaderVerdict v = CheckFollowLeader(in);
    EXPECT_EQ(v.status, FollowLeaderStatus::kCrossMap);
    EXPECT_EQ(v.reason, "Aldric is on another map");
  }

  // Same map, different instance: cross-map mismatch.
  in.leader_map_id = 0;
  in.leader_instance_id = 7;
  EXPECT_EQ(CheckFollowLeader(in).status, FollowLeaderStatus::kCrossMap);
  in.leader_instance_id = 0;

  // Leader on taxi flight path.
  in.leader_in_flight = true;
  {
    const FollowLeaderVerdict v = CheckFollowLeader(in);
    EXPECT_EQ(v.status, FollowLeaderStatus::kLeaderInFlight);
    EXPECT_EQ(v.reason, "Aldric is on a flight path");
  }
  in.leader_in_flight = false;

  // Leader on boat/zeppelin (bot not on same transport).
  in.leader_on_transport = true;
  {
    const FollowLeaderVerdict v = CheckFollowLeader(in);
    EXPECT_EQ(v.status, FollowLeaderStatus::kLeaderOnTransport);
    EXPECT_EQ(v.reason, "Aldric is on a boat/zeppelin");
  }
  // Counter-test: both on the same boat -> can follow directly.
  in.bot_on_same_transport = true;
  EXPECT_EQ(CheckFollowLeader(in).status,
            FollowLeaderStatus::kCanFollowDirectly);

  // Counter-test: dead/ghost leader -> unavailable.
  in.leader_is_ghost = true;
  EXPECT_EQ(CheckFollowLeader(in).status,
            FollowLeaderStatus::kLeaderUnavailable);
}

}  // namespace
}  // namespace llm_brain
