// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/platform_thread.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include "base/callback.h"
#include "base/time.h"

namespace base {
namespace {

struct StartParams {
  std::string name;
  OnceClosure body;
};

void* ThreadMain(void* arg) {
  std::unique_ptr<StartParams> params(static_cast<StartParams*>(arg));
  PlatformThread::SetName(params->name);
  std::move(params->body).Run();
  return nullptr;
}

}  // namespace

// static
PlatformThreadHandle PlatformThread::Create(std::string name,
                                            OnceClosure body) {
  PlatformThreadHandle handle;
  auto* params = new StartParams{std::move(name), std::move(body)};
  if (pthread_create(&handle.handle_, nullptr, &ThreadMain, params) != 0) {
    delete params;
    return handle;
  }
  handle.valid_ = true;
  return handle;
}

// static
void PlatformThread::Join(PlatformThreadHandle& handle) {
  if (handle.valid_) {
    pthread_join(handle.handle_, nullptr);
    handle.valid_ = false;
  }
}

// static
void PlatformThread::SetName(const std::string& name) {
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

// static
void PlatformThread::Sleep(TimeDelta duration) {
  const int64_t us = duration.InMicroseconds();
  timespec ts{static_cast<time_t>(us / 1000000),
              static_cast<long>((us % 1000000) * 1000)};
  while (nanosleep(&ts, &ts) != 0) {
  }
}

// static
int PlatformThread::CurrentId() {
  return static_cast<int>(syscall(SYS_gettid));
}

}  // namespace base
