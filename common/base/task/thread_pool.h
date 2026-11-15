// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_TASK_THREAD_POOL_H_
#define COMMON_BASE_TASK_THREAD_POOL_H_

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/threading/platform_thread.h"

namespace base {

// A fixed pool of worker threads for blocking work (e.g. HTTP calls). Tasks run
// in parallel and in no particular order. Use PostTaskAndReplyWithResult to get
// results back onto a sequence.
class ThreadPool {
 public:
  ThreadPool(std::string name, size_t num_threads);
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ~ThreadPool();  // Runs queued tasks, then joins.

  // Thread-safe. Returns false after shutdown started.
  bool PostTask(OnceClosure task);

 private:
  void WorkerLoop();

  Lock lock_;
  ConditionVariable cv_{&lock_};
  std::deque<OnceClosure> tasks_ GUARDED_BY(lock_);
  bool shutting_down_ GUARDED_BY(lock_) = false;
  std::vector<PlatformThreadHandle> workers_;
};

}  // namespace base

#endif  // COMMON_BASE_TASK_THREAD_POOL_H_
