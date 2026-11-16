// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "scheduler.h"

#include <cstddef>
#include <string>
#include <utility>

#include "base/functional/callback.h"

namespace harness {

Scheduler::Scheduler(size_t max_in_flight) : max_in_flight_(max_in_flight) {}

Scheduler::~Scheduler() = default;

void Scheduler::Submit(const std::string& bot, Urgency urgency, Work work) {
  for (Waiting& waiting : queue_) {
    if (waiting.bot == bot) {
      // The newer wake-up replaces the stale one but keeps its place in line
      // and the stronger urgency.
      waiting.work = std::move(work);
      if (urgency == Urgency::kUrgent) {
        waiting.urgency = Urgency::kUrgent;
      }
      StartWaiting();
      return;
    }
  }
  queue_.push_back({bot, urgency, next_order_++, std::move(work)});
  StartWaiting();
}

void Scheduler::StartWaiting() {
  if (starting_) {
    return;  // The loop below picks up whatever changed.
  }
  starting_ = true;
  while (in_flight_.size() < max_in_flight_) {
    // The next to start: urgent first, then the longest waiting, skipping bots
    // that already have a question in progress.
    size_t best = queue_.size();
    for (size_t i = 0; i < queue_.size(); ++i) {
      if (in_flight_.count(queue_[i].bot) > 0) {
        continue;
      }
      if (best == queue_.size() || queue_[i].urgency < queue_[best].urgency ||
          (queue_[i].urgency == queue_[best].urgency &&
           queue_[i].order < queue_[best].order)) {
        best = i;
      }
    }
    if (best == queue_.size()) {
      break;
    }
    Waiting next = std::move(queue_[best]);
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(best));
    in_flight_.insert(next.bot);
    std::move(next.work).Run(base::BindOnce(
        &Scheduler::OnDone, weak_factory_.GetWeakPtr(), next.bot));
  }
  starting_ = false;
}

void Scheduler::OnDone(const std::string& bot) {
  in_flight_.erase(bot);
  StartWaiting();
}

}  // namespace harness
