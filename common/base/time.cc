// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/time.h"

#include <chrono>

namespace base {

// static
TimeTicks TimeTicks::Now() {
  return TimeTicks(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
}

}  // namespace base
