// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/task/task_queue.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <queue>
#include <utility>
#include <vector>
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace base {
namespace {
thread_local const TaskQueue* g_running_queue = nullptr;
}  // namespace

// static
scoped_refptr<TaskQueue> TaskQueue::Create() {
  return scoped_refptr<TaskQueue>(new TaskQueue());
}

TaskQueue::ScopedBindToCurrentThread::ScopedBindToCurrentThread(
    TaskQueue* queue)
    : previous_queue_(g_running_queue), current_default_(queue) {
  g_running_queue = queue;
}

TaskQueue::ScopedBindToCurrentThread::~ScopedBindToCurrentThread() {
  g_running_queue = previous_queue_;
}

TaskQueue::TaskQueue() = default;
TaskQueue::~TaskQueue() = default;

bool TaskQueue::PostDelayedTask(OnceClosure task, TimeDelta delay) {
  RepeatingClosure wakeup;
  {
    AutoLock lock(lock_);
    if (shut_down_) {
      return false;
    }
    if (!delay.is_positive()) {
      immediate_.push_back(std::move(task));
    } else {
      delayed_.push(DelayedTask{TimeTicks::Now() + delay, next_sequence_++,
                                std::move(task)});
    }
    wakeup = wakeup_;
    if (wakeup) {
      ++active_wakeups_;
    }
  }
  if (wakeup) {
    wakeup.Run();
    wakeup = {};
    AutoLock lock(lock_);
    if (--active_wakeups_ == 0) {
      wakeups_drained_.Broadcast();
    }
  }
  return true;
}

bool TaskQueue::RunsTasksInCurrentSequence() const {
  return g_running_queue == this;
}

std::vector<OnceClosure> TaskQueue::TakeReadyTasks() {
  std::vector<OnceClosure> ready;
  const TimeTicks now = TimeTicks::Now();
  AutoLock lock(lock_);
  while (!delayed_.empty() && delayed_.top().run_time <= now) {
    ready.push_back(std::move(delayed_.top().task));
    delayed_.pop();
  }
  for (OnceClosure& task : immediate_) {
    ready.push_back(std::move(task));
  }
  immediate_.clear();
  return ready;
}

size_t TaskQueue::RunPendingTasks() {
  std::vector<OnceClosure> ready = TakeReadyTasks();
  ScopedBindToCurrentThread bind(this);
  for (OnceClosure& task : ready) {
    std::move(task).Run();
  }
  return ready.size();
}

void TaskQueue::Shutdown() {
  std::deque<OnceClosure> immediate;
  std::priority_queue<DelayedTask, std::vector<DelayedTask>, std::greater<>>
      delayed;
  {
    AutoLock lock(lock_);
    shut_down_ = true;
    immediate.swap(immediate_);
    delayed.swap(delayed_);
  }
  // Pending tasks are destroyed here, outside the lock.
}

void TaskQueue::SetWakeupCallback(RepeatingClosure wakeup) {
  RepeatingClosure old;
  {
    AutoLock lock(lock_);
    old = std::move(wakeup_);
    wakeup_ = {};
    while (active_wakeups_ > 0) {
      wakeups_drained_.Wait();
    }
    wakeup_ = std::move(wakeup);
  }
}

std::optional<TimeTicks> TaskQueue::NextDelayedRunTime() const {
  AutoLock lock(lock_);
  if (delayed_.empty()) {
    return std::nullopt;
  }
  return delayed_.top().run_time;
}

size_t TaskQueue::PendingTaskCount() const {
  AutoLock lock(lock_);
  return immediate_.size() + delayed_.size();
}

}  // namespace base
