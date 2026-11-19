// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bridge/latest_wins.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/time/time.h"

namespace llm_brain {

bool LatestWins::Put(std::string key,
                     std::string message,
                     base::TimeTicks queued) {
  for (Item& item : items_) {
    if (item.key == key) {
      item.message = std::move(message);
      item.queued = queued;
      return true;
    }
  }
  items_.push_back({std::move(key), std::move(message), queued});
  return false;
}

LatestWins::Item LatestWins::TakeOldest() {
  Item oldest = std::move(items_.front());
  items_.erase(items_.begin());
  return oldest;
}

std::optional<LatestWins::Item> LatestWins::TakeKey(std::string_view key) {
  for (auto it = items_.begin(); it != items_.end(); ++it) {
    if (it->key == key) {
      Item found = std::move(*it);
      items_.erase(it);
      return found;
    }
  }
  return std::nullopt;
}

std::vector<LatestWins::Item> LatestWins::TakeAll() {
  return std::exchange(items_, {});
}

}  // namespace llm_brain
