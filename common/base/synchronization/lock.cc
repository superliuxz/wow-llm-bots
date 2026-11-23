// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/synchronization/lock.h"

#include <pthread.h>

#include <cstdlib>

namespace base {
namespace {

void CheckZero(int rc) {
  if (rc != 0) {
    std::abort();
  }
}

}  // namespace

Lock::Lock() {
  pthread_mutexattr_t attr;
  CheckZero(pthread_mutexattr_init(&attr));
  // Catch double-locking and unlocking from the wrong thread in every build.
  CheckZero(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
  CheckZero(pthread_mutex_init(&mutex_, &attr));
  CheckZero(pthread_mutexattr_destroy(&attr));
}

Lock::~Lock() {
  pthread_mutex_destroy(&mutex_);
}

void Lock::Acquire() {
  CheckZero(pthread_mutex_lock(&mutex_));
}

void Lock::Release() {
  CheckZero(pthread_mutex_unlock(&mutex_));
}

bool Lock::Try() {
  return pthread_mutex_trylock(&mutex_) == 0;
}

}  // namespace base
