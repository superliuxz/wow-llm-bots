// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/group_roles.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

namespace pb = wlb::snapshot;

pb::Role ResolveEffectiveRole(const GroupMemberRoleInput& m) {
  pb::Role role = m.lfg_role;
  if (role == pb::Role::ROLE_UNSPECIFIED && m.online_in_world) {
    if (m.spec_tank) {
      role = pb::Role::TANK;
    } else if (m.spec_healer) {
      role = pb::Role::HEALER;
    } else {
      role = pb::Role::DPS;
    }
  }
  if (role == pb::Role::TANK && m.not_tank_policy) {
    return pb::Role::DPS;
  }
  return role;
}

bool IsEffectiveTank(const GroupMemberRoleInput& m) {
  return m.main_tank_flag || ResolveEffectiveRole(m) == pb::Role::TANK;
}

bool IsEffectiveHealer(const GroupMemberRoleInput& m) {
  return ResolveEffectiveRole(m) == pb::Role::HEALER;
}

std::vector<uint64_t> ResolveGroupTankOrder(
    const std::vector<GroupMemberRoleInput>& members) {
  std::vector<uint64_t> tanks;
  auto seen = [&](uint64_t guid) {
    return std::find(tanks.begin(), tanks.end(), guid) != tanks.end();
  };

  // Tier 1: explicit raid Main Tank flags, preserving raid slot order.
  for (const GroupMemberRoleInput& m : members) {
    if (!m.online_in_world || !m.same_map || m.guid == 0) {
      continue;
    }
    if (m.main_tank_flag && !seen(m.guid)) {
      tanks.push_back(m.guid);
    }
  }

  // Tier 2: effective TANK role (LFG or spec fallback, respecting `not_tank`),
  // ordered deterministically by `(subgroup, guid)` so server and harness agree
  // on `tank` vs `offtank` regardless of which bot is querying.
  std::vector<const GroupMemberRoleInput*> role_tanks;
  for (const GroupMemberRoleInput& m : members) {
    if (!m.online_in_world || !m.same_map || m.guid == 0 || seen(m.guid)) {
      continue;
    }
    if (ResolveEffectiveRole(m) == pb::Role::TANK) {
      role_tanks.push_back(&m);
    }
  }
  std::sort(role_tanks.begin(), role_tanks.end(),
            [](const GroupMemberRoleInput* a, const GroupMemberRoleInput* b) {
              return std::tie(a->subgroup, a->guid) <
                     std::tie(b->subgroup, b->guid);
            });
  for (const GroupMemberRoleInput* m : role_tanks) {
    if (!seen(m->guid)) {
      tanks.push_back(m->guid);
    }
  }
  return tanks;
}

std::optional<uint64_t> ResolveNearestGroupHealer(
    const std::vector<GroupMemberRoleInput>& members) {
  std::optional<uint64_t> best_alive;
  float best_alive_dist = std::numeric_limits<float>::infinity();
  std::optional<uint64_t> best_dead;
  float best_dead_dist = std::numeric_limits<float>::infinity();

  for (const GroupMemberRoleInput& m : members) {
    if (!m.online_in_world || !m.same_map || m.guid == 0 ||
        !IsEffectiveHealer(m)) {
      continue;
    }
    if (m.alive) {
      if (!best_alive || m.dist_yd < best_alive_dist) {
        best_alive = m.guid;
        best_alive_dist = m.dist_yd;
      }
    } else {
      if (!best_dead || m.dist_yd < best_dead_dist) {
        best_dead = m.guid;
        best_dead_dist = m.dist_yd;
      }
    }
  }
  return best_alive ? best_alive : best_dead;
}

std::optional<uint64_t> ResolveNearestGroupMember(
    uint64_t bot_guid,
    const std::vector<NearbyGroupCandidate>& candidates) {
  std::optional<uint64_t> best;
  float best_dist = std::numeric_limits<float>::infinity();
  for (const NearbyGroupCandidate& c : candidates) {
    if (c.guid == 0 || c.guid == bot_guid || !c.alive || !c.in_same_group) {
      continue;
    }
    if (!best || c.dist_yd < best_dist) {
      best = c.guid;
      best_dist = c.dist_yd;
    }
  }
  return best;
}

}  // namespace llm_brain
