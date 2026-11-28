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

// Maximum cross-map portal/trigger/transport transitions in one planned route.
inline constexpr size_t kMaxCrossMapHops = 4;

// Minimum distance (yd) between source and destination for a same-map portal
// or transport edge to be considered a valid long-distance travel link (e.g.
// Darnassus <-> Rut'theran Village, Rut'theran <-> Auberdine).
inline constexpr float kSameMapPortalMinDistYd = 150.0f;

// Maximum flight-master destinations exposed in one NPC action list.
inline constexpr size_t kMaxFlightDestinations = 12;

enum class TravelLegKind : uint8_t {
  // Walk directly along the navmesh to `leg_dest` on `bot_map_id`.
  kWalk,
  // Walk to an AreaTrigger (`trigger_id`) and send CMSG_AREATRIGGER.
  kAreaTrigger,
  // Walk to a portal GameObject (`object_entry`) and click/use it.
  kPortalObject,
  // Walk to a flight master (`taxi_nodes.front()`) and fly `taxi_nodes`.
  kFlightMaster,
  // Walk to a boat/zeppelin dock (`from_pos`), wait/board `object_entry`, and
  // disembark at `(dest_map_id, dest_pos)`.
  kTransport,
};

// One legitimate world travel transition (AreaTriggerTeleport, portal GO, or
// continent boat/zeppelin MotionTransport leg).
struct CrossMapEdge {
  TravelLegKind kind = TravelLegKind::kAreaTrigger;
  uint32_t from_map_id = 0;
  wlb::snapshot::Vec3 from_pos;
  uint32_t to_map_id = 0;
  wlb::snapshot::Vec3 to_pos;
  uint32_t trigger_id = 0;    // Set when kind == kAreaTrigger.
  uint32_t object_entry = 0;  // Set when kind == kPortalObject or kTransport.
  std::string label;
  std::string dest_name;       // Destination area/map name for open-world UI.
  std::string dest_continent;  // Non-empty when to_map_id != from_map_id.
  bool for_alliance = true;
  bool for_horde = true;
  bool involves_instance = false;

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

  // Set when the leader is mid-route on a taxi flight path so "Go to leader"
  // never aims at transient mid-air coordinates.
  bool leader_in_flight = false;
  std::optional<uint32_t> leader_taxi_dest_node;
  std::optional<wlb::snapshot::Vec3> leader_taxi_dest_pos;
  std::string leader_taxi_dest_name;

  // Set when the leader is aboard a moving boat/zeppelin transport so "Go to
  // leader" never aims at transient mid-ocean coordinates.
  bool leader_on_transport = false;
  uint32_t leader_transport_entry = 0;
  bool bot_on_same_transport = false;
};

// One reachable multi-hop flight path from a flight master.
struct TaxiRouteOption {
  uint32_t dest_node = 0;
  std::string dest_name;
  std::vector<uint32_t> nodes;  // [cur_node, ..., dest_node], size >= 2.
  uint32_t total_cost_copper = 0;
  float dist_yd = 0.0f;

  bool operator==(const TaxiRouteOption&) const = default;
};

// Computes reachable, affordable multi-hop taxi destinations from `cur_node`
// over `known_nodes` and `links` using Dijkstra by total copper cost (with
// `price_discount` applied via ceil(total_cost * discount) to match
// Player::ActivateTaxiPathTo). Sorted nearest-first, up to `max_results`.
std::vector<TaxiRouteOption> ComputeReachableTaxiRoutes(
    uint32_t cur_node,
    uint32_t bot_money_copper,
    float price_discount,
    std::span<const TaxiNodeFact> known_nodes,
    std::span<const TaxiLinkFact> links,
    size_t max_results = kMaxFlightDestinations);

// Finds the cheapest known, affordable hop-by-hop node sequence
// `[cur_node, ..., dest_node]` for `Player::ActivateTaxiPathTo`. Returns empty
// vector when unreachable, unknown, same node, or unaffordable.
std::vector<uint32_t> FindTaxiNodePath(
    uint32_t cur_node,
    uint32_t dest_node,
    uint32_t bot_money_copper,
    float price_discount,
    std::span<const TaxiNodeFact> known_nodes,
    std::span<const TaxiLinkFact> links);

// Selects up to `max_places` nearest faction-compatible non-instance travel
// links (boats, zeppelins, portals) on `bot_map_id` within `max_dist_yd`,
// deduplicated by destination `(to_map_id, dest_name)`, so solo and grouped
// bots can choose cross-island and cross-continent routes.
std::vector<wlb::snapshot::OptionFacts::Place> BuildOpenWorldTravelPlaces(
    uint32_t bot_map_id,
    const wlb::snapshot::Vec3& bot_pos,
    bool bot_alliance,
    std::span<const CrossMapEdge> edges,
    float max_dist_yd = 2500.0f,
    size_t max_places = 4);

struct LeaderTravelPlan {
  TravelLegKind kind = TravelLegKind::kWalk;
  wlb::snapshot::Vec3 leg_dest;
  uint32_t trigger_id = 0;
  uint32_t object_entry = 0;
  uint32_t dest_map_id = 0;
  wlb::snapshot::Vec3 dest_pos;
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

// Decoded arrival action for a `place:leader*` or `place:travel*` WalkAction
// id.
struct LeaderTravelArrival {
  TravelLegKind kind = TravelLegKind::kWalk;
  uint32_t trigger_id = 0;
  uint32_t object_entry = 0;
  uint32_t dest_map_id = 0;
  wlb::snapshot::Vec3 dest_pos;
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

// Parses the arrival action encoded in a `place:leader*` or `place:travel*` id:
//  - "place:leader"                                        -> kWalk
//  - "place:(leader|travel):at:<id>"                       -> kAreaTrigger
//  - "place:(leader|travel):portal:<entry>"                -> kPortalObject
//  - "place:(leader|travel):transport:<entry>:<m>:<x>:<y>:<z>" -> kTransport
//  - "place:leader:taxi:<n1>:<n2>…"                        -> kFlightMaster
// Any other id (or malformed suffix) returns `kWalk`.
LeaderTravelArrival ParseLeaderTravelPlaceId(std::string_view place_id);

// Prefix for immediate info events emitted when the party leader changes map
// or boards/arrives on a taxi flight or boat/zeppelin. Matched by the harness
// trigger evaluator to wake grouped followers.
inline constexpr std::string_view kLeaderTravelEventPrefix = "leader travel: ";

struct LeaderTravelState {
  uint64_t leader_guid = 0;
  std::string leader_name;
  uint32_t map_id = 0;
  uint32_t instance_id = 0;
  bool in_flight = false;
  bool on_transport = false;
  uint32_t transport_entry = 0;
  std::string zone_name;
  std::string taxi_dest_name;

  bool operator==(const LeaderTravelState&) const = default;
};

struct LeaderTravelWatch {
  bool initialized = false;
  LeaderTravelState last;

  void Reset() {
    initialized = false;
    last = {};
  }
};

// Compares the leader's current travel state against `watch->last`, updates
// `watch`, and returns a `"leader travel: ..."` wake event text when the
// leader changed map/instance, boarded a taxi flight or boat/zeppelin, or
// arrived from one while away from the bot. Suppresses wakes on first
// observation, leader swap, solo state, or while `bot_in_combat` is true.
std::optional<std::string> UpdateLeaderTravelWatch(
    LeaderTravelWatch* watch,
    const LeaderTravelState& current,
    uint32_t bot_map_id,
    uint32_t bot_instance_id,
    bool bot_on_same_transport,
    bool bot_in_combat);

struct FollowLeaderCheckInput {
  bool leader_present = false;
  bool leader_alive = true;
  bool leader_is_ghost = false;
  std::string leader_name;
  uint32_t bot_map_id = 0;
  uint32_t bot_instance_id = 0;
  uint32_t leader_map_id = 0;
  uint32_t leader_instance_id = 0;
  bool leader_in_flight = false;
  bool leader_on_transport = false;
  bool bot_on_same_transport = false;
};

enum class FollowLeaderStatus : uint8_t {
  kCanFollowDirectly,
  kLeaderUnavailable,
  kCrossMap,
  kLeaderInFlight,
  kLeaderOnTransport,
};

struct FollowLeaderVerdict {
  FollowLeaderStatus status = FollowLeaderStatus::kLeaderUnavailable;
  std::string reason;

  bool operator==(const FollowLeaderVerdict&) const = default;
};

// Checks whether a follower can directly follow the leader right now, or why
// direct follow cannot apply (cross-map, leader on taxi, leader on boat, etc.).
FollowLeaderVerdict CheckFollowLeader(const FollowLeaderCheckInput& in);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_LEADER_TRAVEL_H_
