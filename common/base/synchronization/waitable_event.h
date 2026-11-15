// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_
#define COMMON_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_

#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"

namespace base {

// A one-bit signal threads can wait on.
// Blocking is rare in our code (startup handshakes, tests): prefer posting a
// reply task.
class WaitableEvent {
 public:
  enum class ResetPolicy { MANUAL, AUTOMATIC };
  enum class InitialState { SIGNALED, NOT_SIGNALED };

  explicit WaitableEvent(
      ResetPolicy reset_policy = ResetPolicy::MANUAL,
      InitialState initial_state = InitialState::NOT_SIGNALED);
  WaitableEvent(const WaitableEvent&) = delete;
  WaitableEvent& operator=(const WaitableEvent&) = delete;
  ~WaitableEvent();

  void Signal();
  void Reset();
  bool IsSignaled();
  void Wait();
  // Returns true if signaled before `wait_delta` passed.
  bool TimedWait(TimeDelta wait_delta);

 private:
  const ResetPolicy reset_policy_;
  Lock lock_;
  ConditionVariable cv_;
  bool signaled_ GUARDED_BY(lock_);
};

}  // namespace base

#endif  // COMMON_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_
