// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure C++20 group/raid role and scope resolution shared by snapshot_builder,
// act_executor, bot_manager, and world_script so the server and harness agree
// on `tank`, `offtank`, `healer`, and `group` across solo, 5-man, hand-formed
// 10/25-man raids, and LFG groups.

#ifndef MOD_LLM_BRAIN_OPTIONS_GROUP_ROLES_H_
#define MOD_LLM_BRAIN_OPTIONS_GROUP_ROLES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

// AzerothCore-independent view of one group/raid member slot.
struct GroupMemberRoleInput {
  uint64_t guid = 0;
  // 1-based raid subgroup (`slot.group + 1`); 0 when unknown.
  uint32_t subgroup = 0;
  // Dungeon Finder role from `slot.roles`, or ROLE_UNSPECIFIED in hand-formed
  // parties/raids.
  wlb::snapshot::Role lfg_role = wlb::snapshot::Role::ROLE_UNSPECIFIED;
  // Raid leader Main Tank flag (`MEMBER_FLAG_MAINTANK`).
  bool main_tank_flag = false;
  // Raid leader Main Assist flag (`MEMBER_FLAG_MAINASSIST`).
  bool main_assist_flag = false;
  // True when the member is online and in world (spec/stance can be inspected).
  bool online_in_world = false;
  // Spec/stance fallback (`PlayerbotAI::IsTank` / `PlayerbotAI::IsHeal`).
  bool spec_tank = false;
  bool spec_healer = false;
  // True when a bot chose `Let <name> tank` (`GroupPolicy::not_tank`).
  bool not_tank_policy = false;
  // True when on the same map as the querying bot (targetable by acts).
  bool same_map = true;
  // True when alive (healer selection prefers living healers over corpses).
  bool alive = true;
  // Distance from the querying bot in yards.
  float dist_yd = 0.0f;
};

// Resolves a member's effective `GroupPart::role`:
// - Dungeon Finder role when set, else spec/stance role when `online_in_world`
//   (TANK -> HEALER -> DPS), else ROLE_UNSPECIFIED when offline.
// - Demotes TANK -> DPS when `not_tank_policy` is set.
wlb::snapshot::Role ResolveEffectiveRole(const GroupMemberRoleInput& m);

// True when `m` counts as a tank in its group/raid: explicit `main_tank_flag`
// OR effective role == TANK.
bool IsEffectiveTank(const GroupMemberRoleInput& m);

// True when `m` counts as a healer in its group/raid: effective role == HEALER.
bool IsEffectiveHealer(const GroupMemberRoleInput& m);

// Orders all available (`online_in_world && same_map`) group/raid tanks so
// index 0 is `tank` and indices 1.. are `offtank`:
// 1. Explicit `main_tank_flag` members first, preserving raid slot order.
// 2. Remaining effective-TANK members next, sorted deterministically by
//    `(subgroup, guid)` so server and every bot's harness agree on `tank` vs
//    `offtank` even in hand-formed raids without Main Tank flags.
std::vector<uint64_t> ResolveGroupTankOrder(
    const std::vector<GroupMemberRoleInput>& members);

// Selects the best available (`online_in_world && same_map`) healer GUID for
// `healer` acts: nearest living healer first, falling back to nearest dead
// healer if all healers are dead. Returns nullopt if no healer is available.
std::optional<uint64_t> ResolveNearestGroupHealer(
    const std::vector<GroupMemberRoleInput>& members);

// Candidate nearby player for `Command_Act::GROUP` (e.g. `away group`).
struct NearbyGroupCandidate {
  uint64_t guid = 0;
  // True when the player is a member of the bot's party or raid (any subgroup).
  bool in_same_group = false;
  bool alive = true;
  float dist_yd = 0.0f;
};

// Selects the nearest living player in the bot's whole party or raid (across
// all subgroups), excluding `bot_guid`. Returns nullopt when solo or when no
// living group member is nearby.
std::optional<uint64_t> ResolveNearestGroupMember(
    uint64_t bot_guid,
    const std::vector<NearbyGroupCandidate>& candidates);

// Inputs for deciding whether a bot should run playerbots' tank strategies
// (`+tank assist,+pull back,-dps assist`) or suppress them in favor of DPS
// assist (`-tank assist,-pull back,+dps assist`) (M6).
struct TankStrategyInput {
  bool in_group = false;
  bool is_raid = false;
  // Talent spec / bear stance check (`PlayerbotAI::IsTank(bot, true)`).
  bool spec_tank = false;
  // Active tank strategy check (`PlayerbotAI::IsTank(bot, false)`), e.g. from
  // `llmtest maketank` or default class tank strategy.
  bool has_tank_strategy = false;
  // Assigned Dungeon Finder role (`slot.roles`), or ROLE_UNSPECIFIED.
  wlb::snapshot::Role lfg_role = wlb::snapshot::Role::ROLE_UNSPECIFIED;
  // Explicit raid Main Tank flag (`MEMBER_FLAG_MAINTANK`).
  bool main_tank_flag = false;
  // Brain chose `Let <name> tank` (`GroupPolicy::not_tank`).
  bool not_tank_policy = false;
  // True when another member of the group is a human tank (main-tank flag,
  // LFG tank role, or tank spec/stance).
  bool group_has_human_tank = false;
};

enum class TankStrategyMode : uint8_t {
  kNone,  // Not tank-capable and not assigned tank: leave tank strats alone.
  kTank,  // Active tank: +tank assist,+pull back,-dps assist.
  kSuppress,  // Non-tank role on a tank-capable bot: -tank assist,-pull
              // back,+dps assist.
};

// Derives tank combat strategy mode from effective group role rather than raw
// talent spec so a tank-spec bot assigned DPS/Healer (or in a 5-man party with
// a human tank) does not keep `tank assist` active and fight for aggro.
TankStrategyMode ResolveTankStrategyMode(const TankStrategyInput& in);

// Inputs for bot-led group controls exposed when a bot leads a group with a
// human player (M6): promote human, toggle Heroic/Normal dungeon difficulty,
// change loot method, or reset instances.
struct GroupLeaderControlsInput {
  struct HumanMember {
    std::string guid;
    std::string name;
    bool online_in_world = false;
  };
  bool in_group = false;
  bool self_is_leader = false;
  bool alive = true;
  bool in_combat = false;
  bool is_raid = false;
  bool is_lfg_restricted = false;
  bool all_members_in_world = true;
  bool any_member_in_dungeon = false;
  bool current_heroic = false;
  uint32_t current_loot_method = 2;  // GROUP_LOOT
  std::vector<HumanMember> human_members;
};

// Returns mechanically valid bot-led group control options for the brain.
std::vector<wlb::snapshot::OptionFacts_Action> CollectGroupLeaderControls(
    const GroupLeaderControlsInput& in);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_GROUP_ROLES_H_
