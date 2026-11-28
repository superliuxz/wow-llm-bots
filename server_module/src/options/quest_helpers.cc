// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/quest_helpers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llm_brain {

std::vector<AbandonQuestOption> SelectAbandonQuestOptions(
    bool alive,
    bool in_combat,
    size_t total_log_quests,
    bool quest_log_open,
    const std::vector<AbandonQuestCandidate>& candidates,
    size_t max_options) {
  std::vector<AbandonQuestOption> out;
  if (!alive || in_combat || max_options == 0) {
    return out;
  }
  const bool allow_active =
      total_log_quests >= kMaxQuestLogSize || quest_log_open;

  std::vector<const AbandonQuestCandidate*> ranked;
  ranked.reserve(candidates.size());
  for (const AbandonQuestCandidate& c : candidates) {
    if (c.quest_id == 0 || c.title.empty()) {
      continue;
    }
    if (!c.failed && !allow_active) {
      continue;
    }
    ranked.push_back(&c);
  }
  if (ranked.empty()) {
    return out;
  }

  // Priority: failed quests first, then incomplete before complete, then timed
  // before untimed, then lowest quest level, then lowest quest_id.
  std::stable_sort(
      ranked.begin(), ranked.end(),
      [](const AbandonQuestCandidate* a, const AbandonQuestCandidate* b) {
        if (a->failed != b->failed) {
          return a->failed;
        }
        if (a->complete != b->complete) {
          return !a->complete;
        }
        if (a->timed != b->timed) {
          return a->timed;
        }
        if (a->level != b->level) {
          return a->level < b->level;
        }
        return a->quest_id < b->quest_id;
      });

  std::vector<uint32_t> seen_ids;
  for (const AbandonQuestCandidate* c : ranked) {
    if (out.size() >= max_options) {
      break;
    }
    if (std::find(seen_ids.begin(), seen_ids.end(), c->quest_id) !=
        seen_ids.end()) {
      continue;
    }
    seen_ids.push_back(c->quest_id);
    std::string label;
    if (c->failed) {
      label = "Abandon failed quest \"" + c->title + "\"";
    } else if (c->timed && !c->complete) {
      label = "Abandon timed quest \"" + c->title + "\"";
    } else {
      label = "Abandon quest \"" + c->title + "\"";
    }
    out.push_back(AbandonQuestOption{
        .quest_id = c->quest_id,
        .guid = "qabandon:" + std::to_string(c->quest_id),
        .label = std::move(label),
    });
  }
  return out;
}

void PruneExpiredSharedQuests(std::vector<PendingSharedQuest>* queue,
                              uint32_t now_ms,
                              uint32_t timeout_ms) {
  if (!queue) {
    return;
  }
  std::erase_if(*queue, [now_ms, timeout_ms](const PendingSharedQuest& q) {
    if (q.quest_id == 0 || q.sharer_guid == 0) {
      return true;
    }
    const uint32_t elapsed_ms = now_ms - q.received_ms;
    return elapsed_ms >= timeout_ms;
  });
}

void EnqueueSharedQuest(std::vector<PendingSharedQuest>* queue,
                        const PendingSharedQuest& entry,
                        uint32_t now_ms,
                        uint32_t timeout_ms) {
  if (!queue || entry.quest_id == 0 || entry.sharer_guid == 0) {
    return;
  }
  PruneExpiredSharedQuests(queue, now_ms, timeout_ms);
  for (PendingSharedQuest& existing : *queue) {
    if (existing.quest_id == entry.quest_id) {
      existing = entry;
      return;
    }
  }
  if (queue->size() >= kMaxPendingSharedQuests) {
    queue->erase(queue->begin());
  }
  queue->push_back(entry);
}

std::optional<PendingSharedQuest> TakeSharedQuest(
    std::vector<PendingSharedQuest>* queue,
    uint32_t quest_id) {
  if (!queue || quest_id == 0) {
    return std::nullopt;
  }
  for (auto it = queue->begin(); it != queue->end(); ++it) {
    if (it->quest_id == quest_id) {
      PendingSharedQuest found = *it;
      queue->erase(it);
      return found;
    }
  }
  return std::nullopt;
}

bool ShouldPauseForOfflineHuman(const OfflineHumanPauseInput& input) {
  return input.in_group && input.has_offline_human && !input.has_online_human;
}

}  // namespace llm_brain
