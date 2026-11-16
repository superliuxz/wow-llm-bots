// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "latest_wins.h"

#include <string>
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

std::vector<LatestWins::Item> LatestWins::TakeAll() {
  return std::exchange(items_, {});
}

}  // namespace llm_brain
