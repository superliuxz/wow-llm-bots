// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_TASK_SEQUENCED_TASK_RUNNER_H_
#define COMMON_BASE_TASK_SEQUENCED_TASK_RUNNER_H_

#include <utility>
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"

namespace base {

// Runs posted tasks one at a time, in posting order (delayed tasks in due-time
// order). Held in scoped_refptr, like Chromium's.
class SequencedTaskRunner : public RefCountedThreadSafe<SequencedTaskRunner> {
 public:
  // Thread-safe. Returns false if the task runner no longer accepts tasks (the
  // task is then destroyed without running).
  virtual bool PostDelayedTask(OnceClosure task, TimeDelta delay) = 0;
  bool PostTask(OnceClosure task) {
    return PostDelayedTask(std::move(task), TimeDelta());
  }

  // True if called from a task running on this sequence.
  virtual bool RunsTasksInCurrentSequence() const = 0;

  // The task runner of the task currently running on this thread, or null.
  static scoped_refptr<SequencedTaskRunner> GetCurrentDefault();
  static bool HasCurrentDefault();

 protected:
  friend class RefCountedThreadSafe<SequencedTaskRunner>;
  virtual ~SequencedTaskRunner() = default;

  // Sets the current-default task runner for the lifetime of this object.
  class ScopedCurrentDefault {
   public:
    explicit ScopedCurrentDefault(SequencedTaskRunner* runner);
    ScopedCurrentDefault(const ScopedCurrentDefault&) = delete;
    ScopedCurrentDefault& operator=(const ScopedCurrentDefault&) = delete;
    ~ScopedCurrentDefault();

   private:
    SequencedTaskRunner* previous_;
  };
};

}  // namespace base

#endif  // COMMON_BASE_TASK_SEQUENCED_TASK_RUNNER_H_
