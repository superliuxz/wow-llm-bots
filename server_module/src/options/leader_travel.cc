// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/leader_travel.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "base/strings/string_split.h"
#include "options/position_facts.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

// Small transition cost (yd) per cross-map hop so fewer portals win ties.
constexpr float kCrossMapHopPenaltyYd = 100.0f;

bool ParseUint32(std::string_view s, uint32_t* out) {
  if (s.empty()) {
    return false;
  }
  uint32_t val = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
  if (ec != std::errc{} || ptr != s.data() + s.size()) {
    return false;
  }
  *out = val;
  return true;
}

bool ParseInt32(std::string_view s, int32_t* out) {
  if (s.empty()) {
    return false;
  }
  int32_t val = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
  if (ec != std::errc{} || ptr != s.data() + s.size()) {
    return false;
  }
  *out = val;
  return true;
}

float ClampDiscount(float discount) {
  if (!std::isfinite(discount) || discount <= 0.0f || discount > 1.0f) {
    return 1.0f;
  }
  return discount;
}

uint64_t EffectiveTaxiCost(uint64_t raw_cost, float discount) {
  return static_cast<uint64_t>(
      std::ceil(static_cast<double>(raw_cost) * ClampDiscount(discount)));
}

bool EdgeAllowedForFaction(const CrossMapEdge& edge, bool alliance) {
  return alliance ? edge.for_alliance : edge.for_horde;
}

bool EdgeValid(const CrossMapEdge& edge) {
  if (edge.from_map_id == edge.to_map_id &&
      Dist(edge.from_pos, edge.to_pos.x, edge.to_pos.y, edge.to_pos.z) <
          kSameMapPortalMinDistYd) {
    return false;
  }
  if (edge.kind == TravelLegKind::kAreaTrigger) {
    return edge.trigger_id != 0;
  }
  if (edge.kind == TravelLegKind::kPortalObject ||
      edge.kind == TravelLegKind::kTransport) {
    return edge.object_entry != 0;
  }
  return false;
}

std::string EncodeEdgePlaceId(std::string_view prefix,
                              const CrossMapEdge& edge) {
  if (edge.kind == TravelLegKind::kAreaTrigger) {
    return std::string(prefix) + ":at:" + std::to_string(edge.trigger_id);
  }
  if (edge.kind == TravelLegKind::kPortalObject) {
    return std::string(prefix) + ":portal:" + std::to_string(edge.object_entry);
  }
  if (edge.kind == TravelLegKind::kTransport) {
    return std::string(prefix) +
           ":transport:" + std::to_string(edge.object_entry) + ":" +
           std::to_string(edge.to_map_id) + ":" +
           std::to_string(static_cast<int32_t>(std::lround(edge.to_pos.x))) +
           ":" +
           std::to_string(static_cast<int32_t>(std::lround(edge.to_pos.y))) +
           ":" +
           std::to_string(static_cast<int32_t>(std::lround(edge.to_pos.z)));
  }
  return std::string(prefix);
}

struct TaxiDijkstraResult {
  std::vector<const TaxiNodeFact*> map_nodes;
  size_t src_idx = 0;
  std::vector<uint64_t> min_cost;
  std::vector<size_t> prev;
};

std::optional<TaxiDijkstraResult> RunTaxiDijkstra(
    uint32_t cur_node,
    std::span<const TaxiNodeFact> known_nodes,
    std::span<const TaxiLinkFact> links) {
  if (cur_node == 0 || known_nodes.size() < 2 || links.empty()) {
    return std::nullopt;
  }
  const TaxiNodeFact* src_fact = nullptr;
  for (const TaxiNodeFact& n : known_nodes) {
    if (n.node_id == cur_node) {
      src_fact = &n;
      break;
    }
  }
  if (!src_fact) {
    return std::nullopt;
  }

  TaxiDijkstraResult res;
  res.map_nodes.reserve(known_nodes.size());
  for (const TaxiNodeFact& n : known_nodes) {
    if (n.node_id == 0 || n.map_id != src_fact->map_id) {
      continue;
    }
    bool dup = false;
    for (const TaxiNodeFact* existing : res.map_nodes) {
      if (existing->node_id == n.node_id) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      if (n.node_id == cur_node) {
        res.src_idx = res.map_nodes.size();
      }
      res.map_nodes.push_back(&n);
    }
  }
  if (res.map_nodes.size() < 2) {
    return std::nullopt;
  }

  auto find_idx = [&](uint32_t node_id) -> std::optional<size_t> {
    for (size_t i = 0; i < res.map_nodes.size(); ++i) {
      if (res.map_nodes[i]->node_id == node_id) {
        return i;
      }
    }
    return std::nullopt;
  };

  struct AdjEdge {
    size_t to = 0;
    uint32_t price = 0;
  };
  const size_t n = res.map_nodes.size();
  std::vector<std::vector<AdjEdge>> adj(n);
  for (const TaxiLinkFact& link : links) {
    const std::optional<size_t> from_i = find_idx(link.from_node);
    const std::optional<size_t> to_i = find_idx(link.to_node);
    if (from_i && to_i && *from_i != *to_i) {
      adj[*from_i].push_back({*to_i, link.price_copper});
    }
  }

  constexpr uint64_t kInfCost = std::numeric_limits<uint64_t>::max();
  res.min_cost.assign(n, kInfCost);
  res.prev.assign(n, n);
  std::vector<size_t> min_hops(n, n + 1);
  std::vector<bool> visited(n, false);
  res.min_cost[res.src_idx] = 0;
  min_hops[res.src_idx] = 0;

  for (size_t iter = 0; iter < n; ++iter) {
    size_t u = n;
    uint64_t u_cost = kInfCost;
    size_t u_hops = n + 1;
    for (size_t i = 0; i < n; ++i) {
      if (!visited[i] &&
          (res.min_cost[i] < u_cost ||
           (res.min_cost[i] == u_cost && min_hops[i] < u_hops))) {
        u = i;
        u_cost = res.min_cost[i];
        u_hops = min_hops[i];
      }
    }
    if (u == n || u_cost == kInfCost) {
      break;
    }
    visited[u] = true;
    for (const AdjEdge& e : adj[u]) {
      const uint64_t cand_cost = u_cost + e.price;
      const size_t cand_hops = u_hops + 1;
      if (cand_cost < res.min_cost[e.to] ||
          (cand_cost == res.min_cost[e.to] && cand_hops < min_hops[e.to])) {
        res.min_cost[e.to] = cand_cost;
        min_hops[e.to] = cand_hops;
        res.prev[e.to] = u;
      }
    }
  }
  return res;
}

std::vector<uint32_t> ReconstructTaxiPath(const TaxiDijkstraResult& res,
                                          size_t dst_idx) {
  std::vector<uint32_t> path;
  const size_t n = res.map_nodes.size();
  for (size_t cur = dst_idx; cur != n; cur = res.prev[cur]) {
    path.push_back(res.map_nodes[cur]->node_id);
    if (cur == res.src_idx) {
      break;
    }
  }
  std::reverse(path.begin(), path.end());
  if (path.size() < 2 || path.front() != res.map_nodes[res.src_idx]->node_id) {
    return {};
  }
  return path;
}

struct TaxiRouteChoice {
  wlb::snapshot::Vec3 src_pos;
  std::vector<uint32_t> nodes;
  std::string dest_name;
  float total_walk_yd = 0.0f;
};

// Finds the best known, affordable taxi route on `map_id` from a flight master
// within `kLeaderTaxiMaxApproachYd` of `bot_pos` to a known node that saves at
// least `kLeaderTaxiMinSavingsYd` of walking toward `target_pos`.
std::optional<TaxiRouteChoice> FindBestTaxiRoute(
    uint32_t map_id,
    const pb::Vec3& bot_pos,
    const pb::Vec3& target_pos,
    uint32_t bot_money_copper,
    std::span<const TaxiNodeFact> known_nodes,
    std::span<const TaxiLinkFact> links) {
  const float direct_dist =
      Dist(bot_pos, target_pos.x, target_pos.y, target_pos.z);
  if (direct_dist < kLeaderTaxiMinTripYd || known_nodes.size() < 2 ||
      links.empty()) {
    return std::nullopt;
  }

  // Index known nodes on `map_id`.
  std::vector<const TaxiNodeFact*> map_nodes;
  map_nodes.reserve(known_nodes.size());
  for (const TaxiNodeFact& n : known_nodes) {
    if (n.node_id != 0 && n.map_id == map_id) {
      map_nodes.push_back(&n);
    }
  }
  if (map_nodes.size() < 2) {
    return std::nullopt;
  }

  auto find_idx = [&](uint32_t node_id) -> std::optional<size_t> {
    for (size_t i = 0; i < map_nodes.size(); ++i) {
      if (map_nodes[i]->node_id == node_id) {
        return i;
      }
    }
    return std::nullopt;
  };

  // Adjacency list over `map_nodes` indices.
  struct AdjEdge {
    size_t to = 0;
    uint32_t price = 0;
  };
  std::vector<std::vector<AdjEdge>> adj(map_nodes.size());
  for (const TaxiLinkFact& link : links) {
    const std::optional<size_t> from_i = find_idx(link.from_node);
    const std::optional<size_t> to_i = find_idx(link.to_node);
    if (from_i && to_i && *from_i != *to_i) {
      adj[*from_i].push_back({*to_i, link.price_copper});
    }
  }

  std::optional<TaxiRouteChoice> best;
  const float max_allowed_walk = direct_dist - kLeaderTaxiMinSavingsYd;

  for (size_t src = 0; src < map_nodes.size(); ++src) {
    const float walk_to_src =
        Dist(bot_pos, map_nodes[src]->pos.x, map_nodes[src]->pos.y,
             map_nodes[src]->pos.z);
    if (walk_to_src > kLeaderTaxiMaxApproachYd ||
        walk_to_src >= max_allowed_walk) {
      continue;
    }

    // Dijkstra by copper cost from `src` to all reachable known nodes.
    constexpr uint64_t kInfCost = std::numeric_limits<uint64_t>::max();
    std::vector<uint64_t> min_cost(map_nodes.size(), kInfCost);
    std::vector<size_t> prev(map_nodes.size(), map_nodes.size());
    std::vector<bool> visited(map_nodes.size(), false);
    min_cost[src] = 0;

    for (size_t iter = 0; iter < map_nodes.size(); ++iter) {
      size_t u = map_nodes.size();
      uint64_t u_cost = kInfCost;
      for (size_t i = 0; i < map_nodes.size(); ++i) {
        if (!visited[i] && min_cost[i] < u_cost) {
          u = i;
          u_cost = min_cost[i];
        }
      }
      if (u == map_nodes.size() || u_cost > bot_money_copper) {
        break;
      }
      visited[u] = true;
      for (const AdjEdge& e : adj[u]) {
        const uint64_t cand_cost = u_cost + e.price;
        if (cand_cost <= bot_money_copper && cand_cost < min_cost[e.to]) {
          min_cost[e.to] = cand_cost;
          prev[e.to] = u;
        }
      }
    }

    for (size_t dst = 0; dst < map_nodes.size(); ++dst) {
      if (dst == src || min_cost[dst] > bot_money_copper) {
        continue;
      }
      const float walk_from_dst =
          Dist(map_nodes[dst]->pos, target_pos.x, target_pos.y, target_pos.z);
      const float total_walk = walk_to_src + walk_from_dst;
      if (total_walk > max_allowed_walk) {
        continue;
      }
      if (!best || total_walk < best->total_walk_yd) {
        std::vector<uint32_t> path;
        for (size_t cur = dst; cur != map_nodes.size(); cur = prev[cur]) {
          path.push_back(map_nodes[cur]->node_id);
          if (cur == src) {
            break;
          }
        }
        std::reverse(path.begin(), path.end());
        if (path.size() >= 2 && path.front() == map_nodes[src]->node_id) {
          best = TaxiRouteChoice{
              .src_pos = map_nodes[src]->pos,
              .nodes = std::move(path),
              .dest_name = map_nodes[dst]->name,
              .total_walk_yd = total_walk,
          };
        }
      }
    }
  }
  return best;
}

std::string EncodeTaxiPlaceId(const std::vector<uint32_t>& nodes) {
  std::string id = "place:leader:taxi";
  for (uint32_t n : nodes) {
    id.push_back(':');
    id += std::to_string(n);
  }
  return id;
}

LeaderTravelPlan MakeTaxiPlan(const pb::Vec3& bot_pos,
                              const TaxiRouteChoice& taxi,
                              size_t cross_map_hops) {
  const float dist =
      Dist(bot_pos, taxi.src_pos.x, taxi.src_pos.y, taxi.src_pos.z);
  const std::string label =
      taxi.dest_name.empty() ? "flight path" : "flight to " + taxi.dest_name;
  return LeaderTravelPlan{
      .kind = TravelLegKind::kFlightMaster,
      .leg_dest = taxi.src_pos,
      .trigger_id = 0,
      .object_entry = 0,
      .dest_map_id = 0,
      .dest_pos = {},
      .taxi_nodes = taxi.nodes,
      .place_id = EncodeTaxiPlaceId(taxi.nodes),
      .route_label = label,
      .dist_yd = dist,
      .dir = DirFrom(bot_pos, taxi.src_pos.x, taxi.src_pos.y),
      .cross_map_hops = cross_map_hops,
  };
}

LeaderTravelPlan MakeEdgePlan(const pb::Vec3& bot_pos,
                              const CrossMapEdge& edge,
                              size_t cross_map_hops) {
  const float dist =
      Dist(bot_pos, edge.from_pos.x, edge.from_pos.y, edge.from_pos.z);
  std::string place_id = EncodeEdgePlaceId("place:leader", edge);
  std::string label = edge.label;
  if (label.empty()) {
    label = edge.kind == TravelLegKind::kTransport ? "transport" : "portal";
  }
  return LeaderTravelPlan{
      .kind = edge.kind,
      .leg_dest = edge.from_pos,
      .trigger_id =
          edge.kind == TravelLegKind::kAreaTrigger ? edge.trigger_id : 0,
      .object_entry = (edge.kind == TravelLegKind::kPortalObject ||
                       edge.kind == TravelLegKind::kTransport)
                          ? edge.object_entry
                          : 0,
      .dest_map_id = edge.to_map_id,
      .dest_pos = edge.to_pos,
      .taxi_nodes = {},
      .place_id = std::move(place_id),
      .route_label = std::move(label),
      .dist_yd = dist,
      .dir = DirFrom(bot_pos, edge.from_pos.x, edge.from_pos.y),
      .cross_map_hops = cross_map_hops,
  };
}

}  // namespace

std::vector<TaxiRouteOption> ComputeReachableTaxiRoutes(
    uint32_t cur_node,
    uint32_t bot_money_copper,
    float price_discount,
    std::span<const TaxiNodeFact> known_nodes,
    std::span<const TaxiLinkFact> links,
    size_t max_results) {
  if (max_results == 0) {
    return {};
  }
  const std::optional<TaxiDijkstraResult> res =
      RunTaxiDijkstra(cur_node, known_nodes, links);
  if (!res) {
    return {};
  }
  constexpr uint64_t kInfCost = std::numeric_limits<uint64_t>::max();
  const TaxiNodeFact& src = *res->map_nodes[res->src_idx];
  std::vector<TaxiRouteOption> out;
  for (size_t i = 0; i < res->map_nodes.size(); ++i) {
    if (i == res->src_idx || res->min_cost[i] == kInfCost) {
      continue;
    }
    const TaxiNodeFact& dst = *res->map_nodes[i];
    if (dst.name.empty()) {
      continue;
    }
    const uint64_t cost = EffectiveTaxiCost(res->min_cost[i], price_discount);
    if (cost > bot_money_copper) {
      continue;
    }
    std::vector<uint32_t> path = ReconstructTaxiPath(*res, i);
    if (path.size() < 2) {
      continue;
    }
    out.push_back(TaxiRouteOption{
        .dest_node = dst.node_id,
        .dest_name = dst.name,
        .nodes = std::move(path),
        .total_cost_copper = static_cast<uint32_t>(cost),
        .dist_yd = Dist(src.pos, dst.pos.x, dst.pos.y, dst.pos.z),
    });
  }
  std::sort(out.begin(), out.end(),
            [](const TaxiRouteOption& a, const TaxiRouteOption& b) {
              if (a.dist_yd != b.dist_yd) {
                return a.dist_yd < b.dist_yd;
              }
              if (a.total_cost_copper != b.total_cost_copper) {
                return a.total_cost_copper < b.total_cost_copper;
              }
              return a.dest_node < b.dest_node;
            });
  if (out.size() > max_results) {
    out.resize(max_results);
  }
  return out;
}

std::vector<uint32_t> FindTaxiNodePath(
    uint32_t cur_node,
    uint32_t dest_node,
    uint32_t bot_money_copper,
    float price_discount,
    std::span<const TaxiNodeFact> known_nodes,
    std::span<const TaxiLinkFact> links) {
  if (cur_node == 0 || dest_node == 0 || cur_node == dest_node) {
    return {};
  }
  const std::optional<TaxiDijkstraResult> res =
      RunTaxiDijkstra(cur_node, known_nodes, links);
  if (!res) {
    return {};
  }
  constexpr uint64_t kInfCost = std::numeric_limits<uint64_t>::max();
  for (size_t i = 0; i < res->map_nodes.size(); ++i) {
    if (res->map_nodes[i]->node_id != dest_node) {
      continue;
    }
    if (res->min_cost[i] == kInfCost) {
      return {};
    }
    if (EffectiveTaxiCost(res->min_cost[i], price_discount) >
        bot_money_copper) {
      return {};
    }
    return ReconstructTaxiPath(*res, i);
  }
  return {};
}

std::vector<pb::OptionFacts::Place> BuildOpenWorldTravelPlaces(
    uint32_t bot_map_id,
    const pb::Vec3& bot_pos,
    bool bot_alliance,
    std::span<const CrossMapEdge> edges,
    float max_dist_yd,
    size_t max_places) {
  if (max_places == 0 || max_dist_yd <= 0.0f) {
    return {};
  }
  struct Candidate {
    const CrossMapEdge* edge = nullptr;
    float dist_yd = 0.0f;
  };
  std::vector<Candidate> cands;
  for (const CrossMapEdge& e : edges) {
    if (e.involves_instance || !EdgeValid(e) ||
        !EdgeAllowedForFaction(e, bot_alliance) ||
        e.from_map_id != bot_map_id) {
      continue;
    }
    const float d = Dist(bot_pos, e.from_pos.x, e.from_pos.y, e.from_pos.z);
    if (d > max_dist_yd) {
      continue;
    }
    cands.push_back({&e, d});
  }
  std::sort(cands.begin(), cands.end(),
            [](const Candidate& a, const Candidate& b) {
              if (a.dist_yd != b.dist_yd) {
                return a.dist_yd < b.dist_yd;
              }
              if (a.edge->dest_name != b.edge->dest_name) {
                return a.edge->dest_name < b.edge->dest_name;
              }
              if (a.edge->to_map_id != b.edge->to_map_id) {
                return a.edge->to_map_id < b.edge->to_map_id;
              }
              return a.edge->object_entry + a.edge->trigger_id <
                     b.edge->object_entry + b.edge->trigger_id;
            });

  std::vector<pb::OptionFacts::Place> out;
  std::vector<std::pair<uint32_t, std::string>> seen_dests;
  for (const Candidate& c : cands) {
    if (out.size() >= max_places) {
      break;
    }
    const CrossMapEdge& e = *c.edge;
    std::string name = !e.dest_name.empty() ? e.dest_name : e.label;
    if (name.empty()) {
      name = e.kind == TravelLegKind::kTransport ? "Transport" : "Portal";
    }
    const std::pair<uint32_t, std::string> dedup_key{e.to_map_id, name};
    if (std::find(seen_dests.begin(), seen_dests.end(), dedup_key) !=
        seen_dests.end()) {
      continue;
    }
    seen_dests.push_back(dedup_key);

    const std::string kind_word =
        e.kind == TravelLegKind::kTransport ? "boat/zeppelin" : "portal";
    const std::string services = e.dest_continent.empty()
                                     ? kind_word
                                     : kind_word + " to " + e.dest_continent;
    out.push_back(pb::OptionFacts::Place{
        .id = EncodeEdgePlaceId("place:travel", e),
        .name = std::move(name),
        .services = services,
        .dir = DirFrom(bot_pos, e.from_pos.x, e.from_pos.y),
        .dist_yd = c.dist_yd,
        .pos = e.from_pos,
    });
  }
  return out;
}

std::optional<LeaderTravelPlan> PlanLeaderTravel(
    const LeaderTravelInput& input,
    std::span<const CrossMapEdge> edges,
    std::span<const TaxiNodeFact> known_taxi_nodes,
    std::span<const TaxiLinkFact> taxi_links) {
  if (!input.in_group || input.is_leader || !input.bot_alive ||
      input.bot_in_combat || input.bot_in_flight || input.bot_in_bg_or_arena) {
    return std::nullopt;
  }
  if (!input.leader_present || !input.leader_alive ||
      input.leader_in_bg_or_arena || input.leader_name.empty()) {
    return std::nullopt;
  }

  // Case 1: Same map and same instance.
  if (input.bot_map_id == input.leader_map_id &&
      input.bot_instance_id == input.leader_instance_id) {
    if ((input.bot_phase_mask & input.leader_phase_mask) == 0) {
      return std::nullopt;
    }
    const float dist = Dist(input.bot_pos, input.leader_pos.x,
                            input.leader_pos.y, input.leader_pos.z);
    if (dist <= kLeaderTravelMinDistYd) {
      return std::nullopt;
    }
    if (const std::optional<TaxiRouteChoice> taxi = FindBestTaxiRoute(
            input.bot_map_id, input.bot_pos, input.leader_pos,
            input.bot_money_copper, known_taxi_nodes, taxi_links)) {
      return MakeTaxiPlan(input.bot_pos, *taxi, /*cross_map_hops=*/0);
    }
    // Check if a same-map portal or boat (e.g. Darnassus -> Rut'theran
    // Village, Rut'theran -> Auberdine) cuts walking distance significantly.
    if (dist >= kLeaderTaxiMinTripYd) {
      const CrossMapEdge* best_same_map = nullptr;
      float best_walk = dist - kLeaderTaxiMinSavingsYd;
      for (const CrossMapEdge& edge : edges) {
        if (!EdgeValid(edge) ||
            !EdgeAllowedForFaction(edge, input.bot_alliance) ||
            edge.from_map_id != input.bot_map_id ||
            edge.to_map_id != input.bot_map_id) {
          continue;
        }
        const float approach = Dist(input.bot_pos, edge.from_pos.x,
                                    edge.from_pos.y, edge.from_pos.z);
        const float egress = Dist(edge.to_pos, input.leader_pos.x,
                                  input.leader_pos.y, input.leader_pos.z);
        const float total_walk = approach + egress + kCrossMapHopPenaltyYd;
        if (total_walk < best_walk) {
          best_walk = total_walk;
          best_same_map = &edge;
        }
      }
      if (best_same_map) {
        return MakeEdgePlan(input.bot_pos, *best_same_map,
                            /*cross_map_hops=*/1);
      }
    }
    return LeaderTravelPlan{
        .kind = TravelLegKind::kWalk,
        .leg_dest = input.leader_pos,
        .trigger_id = 0,
        .object_entry = 0,
        .dest_map_id = 0,
        .dest_pos = {},
        .taxi_nodes = {},
        .place_id = "place:leader",
        .route_label = "",
        .dist_yd = dist,
        .dir = DirFrom(input.bot_pos, input.leader_pos.x, input.leader_pos.y),
        .cross_map_hops = 0,
    };
  }

  // Case 2: Same map, different instance (e.g. bot is in a different copy of
  // the same dungeon). Must exit the current instance first.
  if (input.bot_map_id == input.leader_map_id) {
    if (!input.bot_in_instance) {
      return std::nullopt;
    }
    const CrossMapEdge* best_exit = nullptr;
    float best_dist = std::numeric_limits<float>::infinity();
    for (const CrossMapEdge& edge : edges) {
      if (!EdgeValid(edge) ||
          !EdgeAllowedForFaction(edge, input.bot_alliance) ||
          edge.from_map_id != input.bot_map_id ||
          edge.to_map_id == input.bot_map_id) {
        continue;
      }
      const float d = Dist(input.bot_pos, edge.from_pos.x, edge.from_pos.y,
                           edge.from_pos.z);
      if (!best_exit || d < best_dist) {
        best_exit = &edge;
        best_dist = d;
      }
    }
    if (!best_exit) {
      return std::nullopt;
    }
    return MakeEdgePlan(input.bot_pos, *best_exit, /*cross_map_hops=*/1);
  }

  // Case 3: Different map. Shortest-distance multi-hop search over legitimate
  // cross-map and same-map island edges (up to `kMaxCrossMapHops` transitions).
  struct SearchState {
    uint32_t map_id = 0;
    pb::Vec3 pos;
    float cost = 0.0f;
    size_t hops = 0;
    const CrossMapEdge* first_edge = nullptr;
    uint32_t visited_maps[kMaxCrossMapHops + 1] = {};
    size_t visited_count = 0;
    size_t used_edges[kMaxCrossMapHops] = {};
    size_t used_edge_count = 0;
  };

  auto has_visited = [](const SearchState& s, uint32_t map_id) {
    for (size_t i = 0; i < s.visited_count; ++i) {
      if (s.visited_maps[i] == map_id) {
        return true;
      }
    }
    return false;
  };
  auto has_used_edge = [](const SearchState& s, size_t edge_idx) {
    for (size_t i = 0; i < s.used_edge_count; ++i) {
      if (s.used_edges[i] == edge_idx) {
        return true;
      }
    }
    return false;
  };

  // Pre-filter valid faction-allowed edges for fast search.
  std::vector<const CrossMapEdge*> valid_edges;
  valid_edges.reserve(edges.size());
  for (const CrossMapEdge& e : edges) {
    if (EdgeValid(e) && EdgeAllowedForFaction(e, input.bot_alliance)) {
      valid_edges.push_back(&e);
    }
  }
  if (valid_edges.empty()) {
    return std::nullopt;
  }

  std::vector<SearchState> frontier;
  SearchState start;
  start.map_id = input.bot_map_id;
  start.pos = input.bot_pos;
  start.visited_maps[0] = input.bot_map_id;
  start.visited_count = 1;
  frontier.push_back(start);

  const CrossMapEdge* best_first_edge = nullptr;
  float best_total_cost = std::numeric_limits<float>::infinity();
  size_t best_hops = 0;

  // Bounded BFS/branch-and-bound over hops (max 4 hops; tiny branching per
  // map).
  for (size_t depth = 0; depth < kMaxCrossMapHops && !frontier.empty();
       ++depth) {
    std::vector<SearchState> next_frontier;
    for (const SearchState& cur : frontier) {
      if (cur.cost >= best_total_cost) {
        continue;
      }
      for (size_t ei = 0; ei < valid_edges.size(); ++ei) {
        const CrossMapEdge* edge = valid_edges[ei];
        if (edge->from_map_id != cur.map_id || has_used_edge(cur, ei)) {
          continue;
        }
        const bool same_map_hop = edge->to_map_id == cur.map_id;
        if (!same_map_hop && has_visited(cur, edge->to_map_id)) {
          continue;
        }
        const float walk =
            Dist(cur.pos, edge->from_pos.x, edge->from_pos.y, edge->from_pos.z);
        const float next_cost = cur.cost + walk + kCrossMapHopPenaltyYd;
        if (next_cost >= best_total_cost) {
          continue;
        }
        const CrossMapEdge* first = cur.first_edge ? cur.first_edge : edge;
        const size_t next_hops = cur.hops + 1;
        if (edge->to_map_id == input.leader_map_id) {
          const float final_walk = Dist(edge->to_pos, input.leader_pos.x,
                                        input.leader_pos.y, input.leader_pos.z);
          const float total = next_cost + final_walk;
          if (total < best_total_cost) {
            best_total_cost = total;
            best_first_edge = first;
            best_hops = next_hops;
          }
          continue;
        }
        if (next_hops < kMaxCrossMapHops) {
          SearchState nxt = cur;
          nxt.map_id = edge->to_map_id;
          nxt.pos = edge->to_pos;
          nxt.cost = next_cost;
          nxt.hops = next_hops;
          nxt.first_edge = first;
          nxt.used_edges[nxt.used_edge_count++] = ei;
          if (!same_map_hop && nxt.visited_count <= kMaxCrossMapHops) {
            nxt.visited_maps[nxt.visited_count++] = edge->to_map_id;
          }
          next_frontier.push_back(nxt);
        }
      }
    }
    // Keep only the best-cost states if a map has many parallel portals.
    if (next_frontier.size() > 64) {
      std::partial_sort(next_frontier.begin(), next_frontier.begin() + 64,
                        next_frontier.end(),
                        [](const SearchState& a, const SearchState& b) {
                          return a.cost < b.cost;
                        });
      next_frontier.resize(64);
    }
    frontier = std::move(next_frontier);
  }

  if (!best_first_edge) {
    return std::nullopt;
  }

  // If the first portal/entrance is far away on the bot's current map and a
  // known affordable flight path cuts the approach significantly, fly first.
  if (const std::optional<TaxiRouteChoice> taxi = FindBestTaxiRoute(
          input.bot_map_id, input.bot_pos, best_first_edge->from_pos,
          input.bot_money_copper, known_taxi_nodes, taxi_links)) {
    return MakeTaxiPlan(input.bot_pos, *taxi, best_hops);
  }

  return MakeEdgePlan(input.bot_pos, *best_first_edge, best_hops);
}

std::optional<pb::OptionFacts::Place> BuildLeaderTravelPlace(
    const LeaderTravelInput& input,
    std::span<const CrossMapEdge> edges,
    std::span<const TaxiNodeFact> known_taxi_nodes,
    std::span<const TaxiLinkFact> taxi_links) {
  const std::optional<LeaderTravelPlan> plan =
      PlanLeaderTravel(input, edges, known_taxi_nodes, taxi_links);
  if (!plan) {
    return std::nullopt;
  }
  std::string services = "party leader";
  if (!plan->route_label.empty()) {
    services += ", via " + plan->route_label;
  }
  return pb::OptionFacts::Place{
      .id = plan->place_id,
      .name = input.leader_name,
      .services = std::move(services),
      .dir = plan->dir,
      .dist_yd = plan->dist_yd,
      .pos = plan->leg_dest,
  };
}

LeaderTravelArrival ParseLeaderTravelPlaceId(std::string_view place_id) {
  if (!place_id.starts_with("place:leader:") &&
      !place_id.starts_with("place:travel:")) {
    return {};
  }
  const std::vector<std::string_view> parts = base::SplitStringPiece(
      place_id, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() < 4) {
    return {};
  }
  if (parts[2] == "at" && parts.size() == 4) {
    uint32_t trigger_id = 0;
    if (ParseUint32(parts[3], &trigger_id) && trigger_id != 0) {
      return {.kind = TravelLegKind::kAreaTrigger,
              .trigger_id = trigger_id,
              .object_entry = 0,
              .dest_map_id = 0,
              .dest_pos = {},
              .taxi_nodes = {}};
    }
    return {};
  }
  if (parts[2] == "portal" && parts.size() == 4) {
    uint32_t entry = 0;
    if (ParseUint32(parts[3], &entry) && entry != 0) {
      return {.kind = TravelLegKind::kPortalObject,
              .trigger_id = 0,
              .object_entry = entry,
              .dest_map_id = 0,
              .dest_pos = {},
              .taxi_nodes = {}};
    }
    return {};
  }
  if (parts[2] == "transport" && parts.size() == 8) {
    uint32_t entry = 0;
    uint32_t dest_map = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    if (ParseUint32(parts[3], &entry) && entry != 0 &&
        ParseUint32(parts[4], &dest_map) && ParseInt32(parts[5], &x) &&
        ParseInt32(parts[6], &y) && ParseInt32(parts[7], &z)) {
      return {.kind = TravelLegKind::kTransport,
              .trigger_id = 0,
              .object_entry = entry,
              .dest_map_id = dest_map,
              .dest_pos = {static_cast<float>(x), static_cast<float>(y),
                           static_cast<float>(z)},
              .taxi_nodes = {}};
    }
    return {};
  }
  if (parts[2] == "taxi" && parts.size() >= 5) {
    std::vector<uint32_t> nodes;
    nodes.reserve(parts.size() - 3);
    for (size_t i = 3; i < parts.size(); ++i) {
      uint32_t node = 0;
      if (!ParseUint32(parts[i], &node) || node == 0) {
        return {};
      }
      nodes.push_back(node);
    }
    return {.kind = TravelLegKind::kFlightMaster,
            .trigger_id = 0,
            .object_entry = 0,
            .dest_map_id = 0,
            .dest_pos = {},
            .taxi_nodes = std::move(nodes)};
  }
  return {};
}

}  // namespace llm_brain
