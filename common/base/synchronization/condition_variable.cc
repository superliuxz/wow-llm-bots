// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/synchronization/condition_variable.h"

#include <pthread.h>
#include <time.h>

#include <cstdint>

#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace base {

ConditionVariable::ConditionVariable(Lock* user_lock) : user_lock_(user_lock) {
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&cond_, &attr);
  pthread_condattr_destroy(&attr);
}

ConditionVariable::~ConditionVariable() {
  pthread_cond_destroy(&cond_);
}

void ConditionVariable::Wait() {
  pthread_cond_wait(&cond_, &user_lock_->mutex_);
}

void ConditionVariable::TimedWait(TimeDelta max_time) {
  timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  const int64_t us = max_time.is_negative() ? 0 : max_time.InMicroseconds();
  deadline.tv_sec += static_cast<time_t>(us / 1000000);
  deadline.tv_nsec += static_cast<long>((us % 1000000) * 1000);
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_cond_timedwait(&cond_, &user_lock_->mutex_, &deadline);
}

void ConditionVariable::Signal() {
  pthread_cond_signal(&cond_);
}

void ConditionVariable::Broadcast() {
  pthread_cond_broadcast(&cond_);
}

}  // namespace base
