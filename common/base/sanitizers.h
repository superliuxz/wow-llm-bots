// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Sanitizer helpers, like Chromium's base/compiler_specific.h MSAN_UNPOISON.

#ifndef COMMON_BASE_SANITIZERS_H_
#define COMMON_BASE_SANITIZERS_H_

#include <cstddef>

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define WLB_MEMORY_SANITIZER 1
#endif
#endif

namespace base {

// Marks memory filled by the kernel through a syscall MemorySanitizer has no
// interceptor for (e.g. epoll_pwait2) as initialized. No-op without MSan.
inline void MsanUnpoison(const void* p, size_t size) {
#if defined(WLB_MEMORY_SANITIZER)
  __msan_unpoison(p, size);
#else
  (void)p;
  (void)size;
#endif
}

}  // namespace base

#endif  // COMMON_BASE_SANITIZERS_H_
