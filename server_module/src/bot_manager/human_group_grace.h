// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure state machine for BotManager::LeaveAbandonedHumanGroup: decides when a
// bot's human-formed group should be abandoned or cleaned up after the human
// leaves or disconnects.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_HUMAN_GROUP_GRACE_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_HUMAN_GROUP_GRACE_H_

#include <cstdint>
#include <optional>

namespace llm_brain {

// How long a bot waits for a disconnected human groupmate before leaving the
// group and teleporting out:
// - Open world: 60 s, so stray bot groups clean up promptly.
// - Dungeon / raid (Map::IsDungeon(), covering MAP_INSTANCE and MAP_RAID):
//   30 min, so a client crash, reboot, or long loading screen never aborts an
//   active run.
inline constexpr uint32_t kOpenWorldOfflineGraceMs = 60u * 1000u;
inline constexpr uint32_t kInstanceOfflineGraceMs = 30u * 60u * 1000u;

struct HumanGroupGraceState {
  uint64_t human_group_guid = 0;
  std::optional<uint32_t> human_offline_since_ms;
};

struct HumanGroupGraceInput {
  // 0 when the bot is not currently in a group.
  uint64_t group_guid = 0;
  // True when at least one connected human is in the group.
  bool has_online_human = false;
  // True when at least one disconnected human slot remains in the group.
  bool has_offline_human = false;
  // True when the bot is inside a dungeon or raid (Map::IsDungeon()).
  bool in_instance = false;
  // True when the bot is in combat (never leave or teleport mid-fight).
  bool in_combat = false;
  // Current monotonic server time in ms (getMSTime()).
  uint32_t now_ms = 0;
};

enum class HumanGroupAction : uint8_t {
  // Stay in the group / take no action.
  kKeep,
  // The bot is no longer in a group after previously being in a human group:
  // leave Dungeon Finder and teleport out of any dungeon.
  kCleanupAfterGroupGone,
  // The human left or stayed offline past the grace period: leave Dungeon
  // Finder, disband/leave the group, and teleport out of any dungeon.
  kAbandonGroup,
};

// Returns the offline grace period in milliseconds for the bot's current map.
uint32_t OfflineGraceMs(bool in_instance);

// Evaluates whether a bot's human-formed group has been abandoned, updating
// `state` in place.
HumanGroupAction EvaluateHumanGroupGrace(HumanGroupGraceState& state,
                                         const HumanGroupGraceInput& input);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_HUMAN_GROUP_GRACE_H_
