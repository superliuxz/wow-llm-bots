// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// What the brain-run bots hear, on its way to the world thread. Chat hooks run
// on whichever thread sent the line: a player's whisper on the world thread, a
// playerbot's on a map update thread. They put the line here; the world script
// takes it out on its next update and sends it to the harness as an event.

#ifndef MOD_LLM_BRAIN_CHAT_CHAT_INBOX_H_
#define MOD_LLM_BRAIN_CHAT_CHAT_INBOX_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

class ChatInbox : public base::RawPtrTarget {
 public:
  // At most this many lines wait; more are dropped (and counted) until the
  // world thread catches up.
  static constexpr size_t kMaxWaiting = 256;

  struct Line {
    std::string to;  // The bot that heard it.
    wlb::snapshot::ChatEvent chat;
  };

  ChatInbox();
  ChatInbox(const ChatInbox&) = delete;
  ChatInbox& operator=(const ChatInbox&) = delete;
  ~ChatInbox();

  // The bots whose chat is kept (names as in LlmBrain.Bots). Any thread.
  void SetBots(std::vector<std::string> bots);
  std::vector<std::string> Bots();

  // Keeps `chat` if `to` is one of the bots. Returns whether it was kept. Any
  // thread.
  bool Add(std::string_view to, wlb::snapshot::ChatEvent chat);

  // Everything waiting, oldest first; empties the inbox. Any thread.
  std::vector<Line> TakeAll();

  // Lines dropped because the inbox was full, since the last call.
  size_t TakeDropped();

 private:
  base::Lock lock_;
  std::vector<std::string> bots_ GUARDED_BY(lock_);
  std::vector<Line> lines_ GUARDED_BY(lock_);
  size_t dropped_ GUARDED_BY(lock_) = 0;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CHAT_CHAT_INBOX_H_
