// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Compiler/sanitizer helpers, like Chromium's base/compiler_specific.h.

#ifndef COMMON_BASE_COMPILER_SPECIFIC_H_
#define COMMON_BASE_COMPILER_SPECIFIC_H_

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
// Marks memory filled by the kernel through a syscall MemorySanitizer has no
// interceptor for (e.g. epoll_pwait2) as initialized.
#define MSAN_UNPOISON(p, size) __msan_unpoison(p, size)
#endif
#endif

#if !defined(MSAN_UNPOISON)
#define MSAN_UNPOISON(p, size)
#endif

#endif  // COMMON_BASE_COMPILER_SPECIFIC_H_
