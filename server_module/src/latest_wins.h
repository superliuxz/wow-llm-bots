// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Messages that wait because the socket is full, keyed by what they describe
// (a bot's snapshot): a newer message for a key replaces the waiting one. The
// harness wants each bot's latest state; sending it the old ones first only
// makes it older. (Events are not kept here: every one must arrive, in order.)

#ifndef MOD_LLM_BRAIN_LATEST_WINS_H_
#define MOD_LLM_BRAIN_LATEST_WINS_H_

#include <cstddef>
#include <string>
#include <vector>

#include "base/time/time.h"

namespace llm_brain {

class LatestWins {
 public:
  struct Item {
    std::string key;
    std::string message;
    base::TimeTicks queued;  // When the newest message for the key was queued.
  };

  // Returns true if it replaced a message that was still waiting for `key`.
  bool Put(std::string key, std::string message, base::TimeTicks queued);

  // Everything waiting, keys in the order they first arrived; empties the
  // queue.
  std::vector<Item> TakeAll();

  // The oldest waiting key's message, removed from the queue. Must not be
  // empty.
  Item TakeOldest();

  bool empty() const { return items_.empty(); }
  size_t size() const { return items_.size(); }

 private:
  std::vector<Item> items_;  // Few keys (one per bot): a scan is fine.
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_LATEST_WINS_H_
