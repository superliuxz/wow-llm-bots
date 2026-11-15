// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/time/time.h"

#include <time.h>

#include <cstdint>

namespace base {

// static
TimeTicks TimeTicks::Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return TimeTicks(static_cast<int64_t>(ts.tv_sec) * 1000000 +
                   ts.tv_nsec / 1000);
}

}  // namespace base
