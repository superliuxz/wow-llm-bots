// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_session.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "base/synchronization/lock.h"

namespace llm_brain {

// static
BotSessionRegistry& BotSessionRegistry::Instance() {
  static auto* registry = new BotSessionRegistry();
  return *registry;
}

void BotSessionRegistry::ResetOnLogout(uint64_t bot_guid) {
  base::AutoLock lock(lock_);
  auto it = sessions_.find(bot_guid);
  if (it == sessions_.end()) {
    return;
  }
  if (it->second.group_guid == 0) {
    sessions_.erase(it);
    return;
  }
  // A relog keeps its group, so it keeps the brain's policy for that group.
  BotSession kept;
  kept.group_guid = it->second.group_guid;
  kept.group_policy = it->second.group_policy;
  it->second = std::move(kept);
}

void BotSessionRegistry::ResetOnMapChange(uint64_t bot_guid) {
  base::AutoLock lock(lock_);
  if (auto it = sessions_.find(bot_guid); it != sessions_.end()) {
    it->second.last_interrupted = {};
    it->second.avoided_targets.clear();
  }
}

void BotSessionRegistry::ResetOnDisconnect(uint64_t bot_guid) {
  base::AutoLock lock(lock_);
  if (auto it = sessions_.find(bot_guid); it != sessions_.end()) {
    it->second.last_interrupted = {};
    it->second.avoided_targets.clear();
  }
}

void BotSessionRegistry::ClearAll() {
  base::AutoLock lock(lock_);
  sessions_.clear();
}

size_t BotSessionRegistry::size() const {
  base::AutoLock lock(lock_);
  return sessions_.size();
}

}  // namespace llm_brain
