// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_
#define COMMON_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_

#include <pthread.h>  // IWYU pragma: keep (pthread_cond_t)

#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace base {

// Condition variable bound to one Lock, like Chromium's
// base::ConditionVariable. Wait()/TimedWait() must be called with the lock held
// and may wake spuriously: always re-check the condition in a loop. Uses the
// monotonic clock, so wall-clock changes don't affect timeouts.
class ConditionVariable {
 public:
  explicit ConditionVariable(Lock* user_lock);
  ConditionVariable(const ConditionVariable&) = delete;
  ConditionVariable& operator=(const ConditionVariable&) = delete;
  ~ConditionVariable();

  void Wait();
  void TimedWait(TimeDelta max_time);
  void Signal();
  void Broadcast();

 private:
  pthread_cond_t cond_;
  const raw_ptr<Lock> user_lock_;
};

}  // namespace base

#endif  // COMMON_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_
