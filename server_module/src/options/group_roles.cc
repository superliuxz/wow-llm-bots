// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/group_roles.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
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

  // Tier 1.5: explicit raid Main Assist flags on effective TANK members
  // (common WotLK MT/OT raid convention; never elevates DPS main assists).
  for (const GroupMemberRoleInput& m : members) {
    if (!m.online_in_world || !m.same_map || m.guid == 0 || seen(m.guid)) {
      continue;
    }
    if (m.main_assist_flag && ResolveEffectiveRole(m) == pb::Role::TANK) {
      tanks.push_back(m.guid);
    }
  }

  // Tier 2: explicit Dungeon Finder TANK role before spec-only fallbacks,
  // ordered deterministically by `(subgroup, guid)` so server and harness
  // agree on `tank` vs `offtank` regardless of group/distance order.
  auto append_sorted = [&](std::vector<const GroupMemberRoleInput*>& list) {
    std::sort(list.begin(), list.end(),
              [](const GroupMemberRoleInput* a, const GroupMemberRoleInput* b) {
                return std::tie(a->subgroup, a->guid) <
                       std::tie(b->subgroup, b->guid);
              });
    for (const GroupMemberRoleInput* m : list) {
      if (!seen(m->guid)) {
        tanks.push_back(m->guid);
      }
    }
  };
  std::vector<const GroupMemberRoleInput*> lfg_tanks;
  std::vector<const GroupMemberRoleInput*> spec_tanks;
  for (const GroupMemberRoleInput& m : members) {
    if (!m.online_in_world || !m.same_map || m.guid == 0 || seen(m.guid)) {
      continue;
    }
    if (ResolveEffectiveRole(m) == pb::Role::TANK) {
      if (m.lfg_role == pb::Role::TANK) {
        lfg_tanks.push_back(&m);
      } else {
        spec_tanks.push_back(&m);
      }
    }
  }
  append_sorted(lfg_tanks);
  append_sorted(spec_tanks);
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

TankStrategyMode ResolveTankStrategyMode(const TankStrategyInput& in) {
  const bool tank_capable = in.spec_tank || in.has_tank_strategy;
  if (!in.in_group) {
    return tank_capable ? TankStrategyMode::kTank : TankStrategyMode::kNone;
  }
  if (in.not_tank_policy) {
    return TankStrategyMode::kSuppress;
  }
  if (in.main_tank_flag || in.lfg_role == pb::Role::TANK) {
    return TankStrategyMode::kTank;
  }
  if (in.lfg_role == pb::Role::DPS || in.lfg_role == pb::Role::HEALER) {
    return tank_capable ? TankStrategyMode::kSuppress : TankStrategyMode::kNone;
  }
  // Hand-formed group (no LFG role, no Main Tank flag): in a 5-man party with
  // a human tank, yield tanking so a tank-spec bot does not fight the human
  // tank for aggro. In raids, multiple tanks are normal (MT + OT).
  if (!in.is_raid && in.group_has_human_tank) {
    return tank_capable ? TankStrategyMode::kSuppress : TankStrategyMode::kNone;
  }
  return tank_capable ? TankStrategyMode::kTank : TankStrategyMode::kNone;
}

std::vector<pb::OptionFacts_Action> CollectGroupLeaderControls(
    const GroupLeaderControlsInput& in) {
  std::vector<pb::OptionFacts_Action> out;
  if (!in.in_group || !in.self_is_leader || !in.alive || in.in_combat ||
      in.human_members.empty()) {
    return out;
  }
  // 1. Promote a connected human to leader (valid in regular, raid, and LFG
  //    groups alike: WorldSession::HandleGroupSetLeaderOpcode).
  for (const GroupLeaderControlsInput::HumanMember& h : in.human_members) {
    if (h.online_in_world && !h.guid.empty() && !h.name.empty()) {
      out.push_back({"group:promote:" + h.guid,
                     "Promote " + h.name + " to party leader"});
    }
  }
  // 2. Dungeon difficulty toggle (Normal <-> Heroic): non-raid, non-restricted
  //    LFG, all members in world, nobody inside a dungeon.
  if (!in.is_raid && !in.is_lfg_restricted && in.all_members_in_world &&
      !in.any_member_in_dungeon) {
    if (in.current_heroic) {
      out.push_back({"group:difficulty:0", "Set dungeon difficulty to Normal"});
    } else {
      out.push_back({"group:difficulty:1", "Set dungeon difficulty to Heroic"});
    }
  }
  // 3. Loot method: non-restricted LFG (HandleLootMethodOpcode rejects
  //    isLFGGroup(true)).
  if (!in.is_lfg_restricted) {
    struct LootOption {
      uint32_t method;
      const char* label;
    };
    static constexpr LootOption kLootOptions[] = {
        {0, "Set loot method to Free-for-All"},
        {2, "Set loot method to Group Loot"},
        {4, "Set loot method to Need Before Greed"},
    };
    for (const LootOption& opt : kLootOptions) {
      if (opt.method != in.current_loot_method) {
        out.push_back({"group:loot:" + std::to_string(opt.method), opt.label});
      }
    }
  }
  // 4. Reset all instances: non-restricted LFG, nobody inside a dungeon.
  if (!in.is_lfg_restricted && !in.any_member_in_dungeon) {
    out.push_back({"group:reset", "Reset all instances"});
  }
  return out;
}

}  // namespace llm_brain
