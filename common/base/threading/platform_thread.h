// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_THREADING_PLATFORM_THREAD_H_
#define COMMON_BASE_THREADING_PLATFORM_THREAD_H_

#include <pthread.h>  // IWYU pragma: keep (pthread_t; not <bits/...>)

#include <string>

#include "base/functional/callback.h"
#include "base/time/time.h"

namespace base {

// Thin pthread wrapper. Prefer
// base::Thread or base::ThreadPool; use this only to build those.
class PlatformThreadHandle {
 public:
  PlatformThreadHandle() = default;
  bool is_null() const { return !valid_; }

 private:
  friend class PlatformThread;
  pthread_t handle_{};
  bool valid_ = false;
};

class PlatformThread {
 public:
  // Starts a joinable thread running `body`. Returns a null handle on failure.
  static PlatformThreadHandle Create(std::string name, OnceClosure body);
  // Waits for the thread to finish. `handle` becomes null.
  static void Join(PlatformThreadHandle& handle);
  // Names the calling thread (shown in top, gdb, perf; max 15 chars on Linux).
  static void SetName(const std::string& name);
  static void Sleep(TimeDelta duration);
  // Kernel thread id of the calling thread.
  static int CurrentId();
};

}  // namespace base

#endif  // COMMON_BASE_THREADING_PLATFORM_THREAD_H_
