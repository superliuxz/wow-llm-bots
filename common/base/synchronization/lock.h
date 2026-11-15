// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_SYNCHRONIZATION_LOCK_H_
#define COMMON_BASE_SYNCHRONIZATION_LOCK_H_

#include <pthread.h>  // IWYU pragma: keep (pthread_mutex_t)

#include "base/memory/raw_ptr.h"
#include "base/thread_annotations.h"

namespace base {

class ConditionVariable;

// A non-recursive mutex, like Chromium's base::Lock. Annotated so that
// -Wthread-safety checks GUARDED_BY data is only used with the lock held.
class LOCKABLE Lock : public RawPtrTarget {
 public:
  Lock();
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
  ~Lock();

  void Acquire() EXCLUSIVE_LOCK_FUNCTION();
  void Release() UNLOCK_FUNCTION();
  bool Try() EXCLUSIVE_TRYLOCK_FUNCTION(true);

 private:
  friend class ConditionVariable;
  pthread_mutex_t mutex_;
};

// Holds a Lock for its scope, like Chromium's base::AutoLock.
class SCOPED_LOCKABLE AutoLock {
 public:
  explicit AutoLock(Lock& lock) EXCLUSIVE_LOCK_FUNCTION(lock) : lock_(lock) {
    lock_.Acquire();
  }
  AutoLock(const AutoLock&) = delete;
  AutoLock& operator=(const AutoLock&) = delete;
  ~AutoLock() UNLOCK_FUNCTION() { lock_.Release(); }

 private:
  Lock& lock_;
};

}  // namespace base

#endif  // COMMON_BASE_SYNCHRONIZATION_LOCK_H_
