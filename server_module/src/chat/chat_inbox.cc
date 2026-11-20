// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_inbox.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/synchronization/lock.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

ChatInbox::ChatInbox() = default;

ChatInbox::~ChatInbox() = default;

void ChatInbox::SetBots(std::vector<std::string> bots) {
  base::AutoLock lock(lock_);
  bots_ = std::move(bots);
}

std::vector<std::string> ChatInbox::Bots() {
  base::AutoLock lock(lock_);
  return bots_;
}

bool ChatInbox::Add(std::string_view to, wlb::snapshot::ChatEvent chat) {
  base::AutoLock lock(lock_);
  if (std::find(bots_.begin(), bots_.end(), to) == bots_.end()) {
    return false;
  }
  if (lines_.size() >= kMaxWaiting) {
    ++dropped_;
    return false;
  }
  lines_.push_back({std::string(to), std::move(chat)});
  return true;
}

std::vector<ChatInbox::Line> ChatInbox::TakeAll() {
  base::AutoLock lock(lock_);
  return std::exchange(lines_, {});
}

size_t ChatInbox::TakeDropped() {
  base::AutoLock lock(lock_);
  return std::exchange(dropped_, 0);
}

}  // namespace llm_brain
