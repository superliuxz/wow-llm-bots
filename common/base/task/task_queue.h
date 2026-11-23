// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_TASK_TASK_QUEUE_H_
#define COMMON_BASE_TASK_TASK_QUEUE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <queue>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"

namespace base {

// A thread-safe sequenced task queue. It has no loop of its own: something
// pumps it by calling RunPendingTasks(), either a base::Thread's message pump
// or an existing loop (e.g. AzerothCore's world-thread OnUpdate).
// Create with Create(); held in scoped_refptr like any SequencedTaskRunner.
class TaskQueue : public SequencedTaskRunner {
 public:
  static scoped_refptr<TaskQueue> Create();

  // Makes `queue` the current sequence and current-default task runner of the
  // calling thread for this object's lifetime (base::Thread holds one for its
  // whole run, so fd callbacks can post too). RunPendingTasks() also binds.
  class ScopedBindToCurrentThread {
   public:
    explicit ScopedBindToCurrentThread(TaskQueue* queue);
    ScopedBindToCurrentThread(const ScopedBindToCurrentThread&) = delete;
    ScopedBindToCurrentThread& operator=(const ScopedBindToCurrentThread&) =
        delete;
    ~ScopedBindToCurrentThread();

   private:
    const raw_ptr<const TaskQueue> previous_queue_;
    ScopedCurrentDefault current_default_;
  };

  // SequencedTaskRunner:
  bool PostDelayedTask(OnceClosure task, TimeDelta delay) override;
  bool RunsTasksInCurrentSequence() const override;

  // Runs every task that is ready now (tasks posted while running wait for the
  // next call). Returns the number of tasks run.
  size_t RunPendingTasks();
  // Stops accepting tasks and destroys pending ones. Thread-safe.
  void Shutdown();

  // Called (on the posting thread) whenever a new task is posted, so an
  // external loop can wake up. Set once, before posting starts.
  void SetWakeupCallback(RepeatingClosure wakeup);

  std::optional<TimeTicks> NextDelayedRunTime() const;
  size_t PendingTaskCount() const;

 private:
  struct DelayedTask {
    TimeTicks run_time;
    uint64_t sequence;
    mutable OnceClosure task;
    bool operator>(const DelayedTask& other) const {
      return run_time != other.run_time ? run_time > other.run_time
                                        : sequence > other.sequence;
    }
  };

  TaskQueue();
  ~TaskQueue() override;
  std::vector<OnceClosure> TakeReadyTasks() LOCKS_EXCLUDED(lock_);

  mutable Lock lock_;
  ConditionVariable wakeups_drained_{&lock_};
  std::deque<OnceClosure> immediate_ GUARDED_BY(lock_);
  std::priority_queue<DelayedTask, std::vector<DelayedTask>, std::greater<>>
      delayed_ GUARDED_BY(lock_);
  uint64_t next_sequence_ GUARDED_BY(lock_) = 0;
  bool shut_down_ GUARDED_BY(lock_) = false;
  RepeatingClosure wakeup_ GUARDED_BY(lock_);
  size_t active_wakeups_ GUARDED_BY(lock_) = 0;
};

}  // namespace base

#endif  // COMMON_BASE_TASK_TASK_QUEUE_H_
