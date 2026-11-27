// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure C++ "Go to leader" travel planner with cross-map routing (Milestone 5b,
// Finding #7).
//
// Computes a client-legal first travel leg to reach a grouped bot's party
// leader when the leader is far away (>25 yd on the same map, or on a different
// map/instance):
//  - Same map & instance, <= 25 yd: not offered (avoids duplicating Follow).
//  - Same map & instance, > 25 yd: direct walk (no 2500 yd cap), or a known
//    affordable flight path when it cuts walking distance significantly.
//  - Same map, different instance: exit the current instance via its exit
//    AreaTrigger first.
//  - Different map: shortest-distance multi-hop route over legitimate
//    AreaTriggerTeleports (instance entrances/exits, Dark Portal, Deeprun Tram)
//    and faction-compatible portal GameObjects (Shattrath/Dalaran portals,
//    Orb of Translocation), optionally accelerated on the current map by a
//    known affordable flight path to the first portal/entrance.
//  - Unreachable (no valid route, solo, leader, dead, in combat, in flight,
//    battleground/arena, or incompatible phase): not offered. Never cheat-
//    teleports.

#ifndef MOD_LLM_BRAIN_OPTIONS_LEADER_TRAVEL_H_
#define MOD_LLM_BRAIN_OPTIONS_LEADER_TRAVEL_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

// Minimum distance (yd) on the same map/instance before "Go to <leader>" is
// offered. Strictly greater than this avoids duplicating close-range "Follow
// <leader> (party leader)" (8-25 yd).
inline constexpr float kLeaderTravelMinDistYd = 25.0f;

// Maximum distance (yd) from the bot to a flight master to consider taking a
// flight path for the current leg.
inline constexpr float kLeaderTaxiMaxApproachYd = 600.0f;

// Minimum total distance (yd) before considering a flight path.
inline constexpr float kLeaderTaxiMinTripYd = 500.0f;

// Minimum walking distance saved (yd) for a flight path to beat direct walking.
inline constexpr float kLeaderTaxiMinSavingsYd = 300.0f;

// Maximum cross-map portal/trigger transitions in one planned route.
inline constexpr size_t kMaxCrossMapHops = 4;

enum class TravelLegKind : uint8_t {
  // Walk directly along the navmesh to `leg_dest` on `bot_map_id`.
  kWalk,
  // Walk to an AreaTrigger (`trigger_id`) and send CMSG_AREATRIGGER.
  kAreaTrigger,
  // Walk to a portal GameObject (`object_entry`) and click/use it.
  kPortalObject,
  // Walk to a flight master (`taxi_nodes.front()`) and fly `taxi_nodes`.
  kFlightMaster,
};

// One legitimate cross-map transition (AreaTriggerTeleport or portal GO).
struct CrossMapEdge {
  TravelLegKind kind = TravelLegKind::kAreaTrigger;
  uint32_t from_map_id = 0;
  wlb::snapshot::Vec3 from_pos;
  uint32_t to_map_id = 0;
  wlb::snapshot::Vec3 to_pos;
  uint32_t trigger_id = 0;    // Set when kind == kAreaTrigger.
  uint32_t object_entry = 0;  // Set when kind == kPortalObject.
  std::string label;
  bool for_alliance = true;
  bool for_horde = true;

  bool operator==(const CrossMapEdge&) const = default;
};

// One taxi node known to the bot on its current map.
struct TaxiNodeFact {
  uint32_t node_id = 0;
  uint32_t map_id = 0;
  wlb::snapshot::Vec3 pos;
  std::string name;

  bool operator==(const TaxiNodeFact&) const = default;
};

// One directed taxi path link between two taxi nodes.
struct TaxiLinkFact {
  uint32_t from_node = 0;
  uint32_t to_node = 0;
  uint32_t price_copper = 0;

  bool operator==(const TaxiLinkFact&) const = default;
};

struct LeaderTravelInput {
  bool in_group = false;
  bool is_leader = false;
  bool bot_alive = true;
  bool bot_in_combat = false;
  bool bot_in_flight = false;
  bool bot_in_bg_or_arena = false;
  bool bot_in_instance = false;
  bool bot_alliance = true;
  uint32_t bot_money_copper = 0;
  uint32_t bot_map_id = 0;
  uint32_t bot_instance_id = 0;
  uint32_t bot_phase_mask = 1;
  wlb::snapshot::Vec3 bot_pos;

  bool leader_present = false;
  bool leader_alive = true;
  bool leader_in_bg_or_arena = false;
  std::string leader_name;
  uint32_t leader_map_id = 0;
  uint32_t leader_instance_id = 0;
  uint32_t leader_phase_mask = 1;
  wlb::snapshot::Vec3 leader_pos;
};

struct LeaderTravelPlan {
  TravelLegKind kind = TravelLegKind::kWalk;
  wlb::snapshot::Vec3 leg_dest;
  uint32_t trigger_id = 0;
  uint32_t object_entry = 0;
  std::vector<uint32_t> taxi_nodes;
  std::string place_id;
  // Empty for direct same-map walk; otherwise e.g. "The Deadmines entrance",
  // "instance exit", or "flight to Sentinel Hill".
  std::string route_label;
  float dist_yd = 0.0f;
  wlb::snapshot::Direction dir = wlb::snapshot::DIRECTION_UNSPECIFIED;
  size_t cross_map_hops = 0;

  bool operator==(const LeaderTravelPlan&) const = default;
};

// Decoded arrival action for a `place:leader*` WalkAction id.
struct LeaderTravelArrival {
  TravelLegKind kind = TravelLegKind::kWalk;
  uint32_t trigger_id = 0;
  uint32_t object_entry = 0;
  std::vector<uint32_t> taxi_nodes;

  bool operator==(const LeaderTravelArrival&) const = default;
};

// Computes the first legitimate travel leg toward the party leader, or
// std::nullopt when not eligible, within 25 yd on the same map/instance, or no
// valid route exists.
std::optional<LeaderTravelPlan> PlanLeaderTravel(
    const LeaderTravelInput& input,
    std::span<const CrossMapEdge> edges,
    std::span<const TaxiNodeFact> known_taxi_nodes = {},
    std::span<const TaxiLinkFact> taxi_links = {});

// Builds the `OptionFacts::Place` entry for `Go to <leader>` when
// `PlanLeaderTravel` finds a valid route.
std::optional<wlb::snapshot::OptionFacts::Place> BuildLeaderTravelPlace(
    const LeaderTravelInput& input,
    std::span<const CrossMapEdge> edges,
    std::span<const TaxiNodeFact> known_taxi_nodes = {},
    std::span<const TaxiLinkFact> taxi_links = {});

// Parses the arrival action encoded in a `place:leader*` id:
//  - "place:leader"                 -> kWalk
//  - "place:leader:at:<id>"         -> kAreaTrigger
//  - "place:leader:portal:<entry>"  -> kPortalObject
//  - "place:leader:taxi:<n1>:<n2>…" -> kFlightMaster
// Any other id (or malformed suffix) returns `kWalk`.
LeaderTravelArrival ParseLeaderTravelPlaceId(std::string_view place_id);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_LEADER_TRAVEL_H_
