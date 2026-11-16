// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The calling thread's id, without the rest of PlatformThread (low-level code
// such as WeakPtr uses it, and PlatformThread's header depends on callbacks,
// which depend on WeakPtr).

#ifndef COMMON_BASE_THREADING_CURRENT_THREAD_ID_H_
#define COMMON_BASE_THREADING_CURRENT_THREAD_ID_H_

#include <sys/syscall.h>  // IWYU pragma: keep (SYS_gettid)
#include <unistd.h>

namespace base {

// The kernel's id of the calling thread. A thread's id never changes, so the
// kernel is asked once per thread.
inline int CurrentThreadId() {
  thread_local const int id = static_cast<int>(syscall(SYS_gettid));
  return id;
}

}  // namespace base

#endif  // COMMON_BASE_THREADING_CURRENT_THREAD_ID_H_
