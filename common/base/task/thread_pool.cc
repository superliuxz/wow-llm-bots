// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/task/thread_pool.h"

#include <cstddef>
#include <string>
#include <utility>
#include "base/functional/callback.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"

namespace base {

ThreadPool::ThreadPool(std::string name, size_t num_threads) {
  for (size_t i = 0; i < num_threads; ++i) {
    workers_.push_back(PlatformThread::Create(name + std::to_string(i),
                                              [this] { WorkerLoop(); }));
  }
}

ThreadPool::~ThreadPool() {
  {
    AutoLock lock(lock_);
    shutting_down_ = true;
    cv_.Broadcast();
  }
  for (PlatformThreadHandle& worker : workers_) {
    PlatformThread::Join(worker);
  }
}

bool ThreadPool::PostTask(OnceClosure task) {
  {
    AutoLock lock(lock_);
    if (shutting_down_) {
      return false;
    }
    tasks_.push_back(std::move(task));
    cv_.Signal();
  }
  return true;
}

void ThreadPool::WorkerLoop() {
  while (true) {
    OnceClosure task;
    {
      AutoLock lock(lock_);
      while (!shutting_down_ && tasks_.empty()) {
        cv_.Wait();
      }
      if (tasks_.empty()) {
        return;  // Shutting down and drained.
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    std::move(task).Run();
  }
}

}  // namespace base
