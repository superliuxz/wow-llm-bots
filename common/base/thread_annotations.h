// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Clang thread-safety annotations.
// With -Wthread-safety the compiler checks that guarded data is only touched
// with its lock held. See https://clang.llvm.org/docs/ThreadSafetyAnalysis.html

#ifndef COMMON_BASE_THREAD_ANNOTATIONS_H_
#define COMMON_BASE_THREAD_ANNOTATIONS_H_

#if defined(__clang__)
#define THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x)
#endif

#define GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
#define PT_GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))
#define LOCKABLE THREAD_ANNOTATION_ATTRIBUTE__(capability("mutex"))
#define SCOPED_LOCKABLE THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)
#define EXCLUSIVE_LOCKS_REQUIRED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_locks_required(__VA_ARGS__))
#define LOCKS_EXCLUDED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))
#define EXCLUSIVE_LOCK_FUNCTION(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_lock_function(__VA_ARGS__))
#define EXCLUSIVE_TRYLOCK_FUNCTION(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_trylock_function(__VA_ARGS__))
#define UNLOCK_FUNCTION(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(unlock_function(__VA_ARGS__))
#define ASSERT_EXCLUSIVE_LOCK(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(assert_exclusive_lock(__VA_ARGS__))
#define NO_THREAD_SAFETY_ANALYSIS \
  THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

#endif  // COMMON_BASE_THREAD_ANNOTATIONS_H_
