// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/synchronization/waitable_event.h"

#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace base {

WaitableEvent::WaitableEvent(ResetPolicy reset_policy,
                             InitialState initial_state)
    : reset_policy_(reset_policy),
      cv_(&lock_),
      signaled_(initial_state == InitialState::SIGNALED) {}

WaitableEvent::~WaitableEvent() = default;

void WaitableEvent::Signal() {
  AutoLock lock(lock_);
  signaled_ = true;
  if (reset_policy_ == ResetPolicy::AUTOMATIC) {
    cv_.Signal();
  } else {
    cv_.Broadcast();
  }
}

void WaitableEvent::Reset() {
  AutoLock lock(lock_);
  signaled_ = false;
}

bool WaitableEvent::IsSignaled() {
  AutoLock lock(lock_);
  const bool signaled = signaled_;
  if (signaled && reset_policy_ == ResetPolicy::AUTOMATIC) {
    signaled_ = false;
  }
  return signaled;
}

void WaitableEvent::Wait() {
  AutoLock lock(lock_);
  while (!signaled_) {
    cv_.Wait();
  }
  if (reset_policy_ == ResetPolicy::AUTOMATIC) {
    signaled_ = false;
  }
}

bool WaitableEvent::TimedWait(TimeDelta wait_delta) {
  const TimeTicks deadline = TimeTicks::Now() + wait_delta;
  AutoLock lock(lock_);
  while (!signaled_) {
    const TimeDelta remaining = deadline - TimeTicks::Now();
    if (!remaining.is_positive()) {
      return false;
    }
    cv_.TimedWait(remaining);
  }
  if (reset_policy_ == ResetPolicy::AUTOMATIC) {
    signaled_ = false;
  }
  return true;
}

}  // namespace base
