// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Decides which woken bot's brain is asked next. Many bots can be woken at
// once and the model can only work on so many questions at a time (the slots
// of llama-server), so the rest wait. Urgent wake-ups (a popup with a timer,
// a fight) go first; within the same urgency, the bot that has waited longest.
//
// A bot has at most one question in progress. If it is woken again while one
// is waiting for its turn, the newer wake-up replaces the old (the old screen
// is stale); if one is in progress, the newer waits behind it.
//
// Not thread-safe: use on one sequence. Work reports back by calling `done`
// on that sequence.

#ifndef HARNESS_SCHEDULER_H_
#define HARNESS_SCHEDULER_H_

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

namespace harness {

enum class Urgency {
  kUrgent,
  kNormal,
};

class Scheduler {
 public:
  // Starts the work for one bot (asks the model). It must call `done` once,
  // on the scheduler's sequence, when it has finished with the answer.
  using Work = base::OnceCallback<void(base::OnceClosure done)>;

  explicit Scheduler(size_t max_in_flight);
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  ~Scheduler();

  void Submit(const std::string& bot, Urgency urgency, Work work);

  size_t in_flight() const { return in_flight_.size(); }
  size_t queued() const { return queue_.size(); }

 private:
  struct Waiting {
    std::string bot;
    Urgency urgency = Urgency::kNormal;
    uint64_t order = 0;  // Lower waited longer.
    Work work;
  };

  void StartWaiting();
  void OnDone(const std::string& bot);

  const size_t max_in_flight_;
  uint64_t next_order_ = 0;
  std::vector<Waiting> queue_;
  std::set<std::string> in_flight_;
  bool starting_ = false;  // StartWaiting is running (work may finish inside).
  base::WeakPtrFactory<Scheduler> weak_factory_{this};
};

}  // namespace harness

#endif  // HARNESS_SCHEDULER_H_
