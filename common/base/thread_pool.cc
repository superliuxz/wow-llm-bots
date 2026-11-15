// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/thread_pool.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include "base/callback.h"
#include "base/platform_thread.h"

namespace base {

ThreadPool::ThreadPool(std::string name, size_t num_threads) {
  for (size_t i = 0; i < num_threads; ++i) {
    workers_.push_back(PlatformThread::Create(name + std::to_string(i),
                                              [this] { WorkerLoop(); }));
  }
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
  }
  cv_.notify_all();
  for (PlatformThreadHandle& worker : workers_) {
    PlatformThread::Join(worker);
  }
}

bool ThreadPool::PostTask(OnceClosure task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) {
      return false;
    }
    tasks_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

void ThreadPool::WorkerLoop() {
  while (true) {
    OnceClosure task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return shutting_down_ || !tasks_.empty(); });
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
