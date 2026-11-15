// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/synchronization/lock.h"

#include <pthread.h>

namespace base {

Lock::Lock() {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  // Catch double-locking and unlocking from the wrong thread in every build.
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&mutex_, &attr);
  pthread_mutexattr_destroy(&attr);
}

Lock::~Lock() {
  pthread_mutex_destroy(&mutex_);
}

void Lock::Acquire() {
  pthread_mutex_lock(&mutex_);
}

void Lock::Release() {
  pthread_mutex_unlock(&mutex_);
}

bool Lock::Try() {
  return pthread_mutex_trylock(&mutex_) == 0;
}

}  // namespace base
