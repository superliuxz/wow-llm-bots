// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_session.h"

#include <cstddef>
#include <cstdint>

#include "base/synchronization/lock.h"

namespace llm_brain {

// static
BotSessionRegistry& BotSessionRegistry::Instance() {
  static auto* registry = new BotSessionRegistry();
  return *registry;
}

void BotSessionRegistry::ResetOnLogout(uint64_t bot_guid) {
  base::AutoLock lock(lock_);
  sessions_.erase(bot_guid);
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
