// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure questing helpers for M2 (no AzerothCore headers, unit-tested directly):
// - Abandon-quest candidate selection (failed quests always, active quests when
//   quest log is full or explicitly open).
// - Multi-share pending quest queue with wrap-safe expiry and deduplication.
// - Offline-human autonomous levelling pause predicate.

#ifndef MOD_LLM_BRAIN_OPTIONS_QUEST_HELPERS_H_
#define MOD_LLM_BRAIN_OPTIONS_QUEST_HELPERS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llm_brain {

inline constexpr size_t kMaxQuestLogSize = 25;
inline constexpr size_t kMaxAbandonOptions = 6;
inline constexpr uint32_t kPendingSharedQuestTimeoutMs = 60000;
inline constexpr size_t kMaxPendingSharedQuests = 16;

struct AbandonQuestCandidate {
  uint32_t quest_id = 0;
  std::string title;
  uint32_t level = 0;
  bool failed = false;
  bool complete = false;
  bool timed = false;
};

struct AbandonQuestOption {
  uint32_t quest_id = 0;
  std::string guid;
  std::string label;
};

// Selects quests to offer for abandonment:
// - Never while dead or in combat (preserves P0 combat priority).
// - Always offers any `failed` quest (failed escort or expired timed quest).
// - When `total_log_quests >= kMaxQuestLogSize` (full 25-quest log) or
//   `quest_log_open`, also offers active quests (preferring incomplete
//   lowest-level quests first, then complete lowest-level quests) up to
//   `max_options` total (failed quests are always prioritized first).
std::vector<AbandonQuestOption> SelectAbandonQuestOptions(
    bool alive,
    bool in_combat,
    size_t total_log_quests,
    bool quest_log_open,
    const std::vector<AbandonQuestCandidate>& candidates,
    size_t max_options = kMaxAbandonOptions);

// One queued quest share or party-escort accept offer received from a group
// member. Multiple shares can be queued simultaneously without blocking later
// shares for 60 s.
struct PendingSharedQuest {
  uint32_t quest_id = 0;
  uint64_t sharer_guid = 0;
  uint32_t received_ms = 0;
  bool is_confirm = false;

  bool operator==(const PendingSharedQuest&) const = default;
};

// Removes entries whose age (`now_ms - received_ms`, uint32_t wrap-safe) is at
// least `timeout_ms`.
void PruneExpiredSharedQuests(
    std::vector<PendingSharedQuest>* queue,
    uint32_t now_ms,
    uint32_t timeout_ms = kPendingSharedQuestTimeoutMs);

// Enqueues or refreshes a shared quest in `queue`. Deduplicates by `quest_id`
// and caps queue length at `kMaxPendingSharedQuests` (dropping the oldest).
void EnqueueSharedQuest(std::vector<PendingSharedQuest>* queue,
                        const PendingSharedQuest& entry,
                        uint32_t now_ms,
                        uint32_t timeout_ms = kPendingSharedQuestTimeoutMs);

// Removes and returns the queued entry for `quest_id`, or `std::nullopt`.
std::optional<PendingSharedQuest> TakeSharedQuest(
    std::vector<PendingSharedQuest>* queue,
    uint32_t quest_id);

struct OfflineHumanPauseInput {
  bool in_group = false;
  bool has_online_human = false;
  bool has_offline_human = false;
};

// True when the bot is grouped with an offline human and no online human is
// present: pause autonomous pulls, exploration, and non-essential travel so
// bots stay in the group and don't level ahead between sessions.
bool ShouldPauseForOfflineHuman(const OfflineHumanPauseInput& input);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_QUEST_HELPERS_H_
