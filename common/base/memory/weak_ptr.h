// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// WeakPtr / WeakPtrFactory. They are not thread-safe: hand out, dereference and
// invalidate on the owning object's thread only. They may be *copied and
// passed* across threads (e.g. bound into a task posted back to the owner's
// thread). A flag binds to the thread that hands out its first WeakPtr (the
// owner's), and dereferencing or invalidating on any other thread aborts: the
// race shows up at once instead of as a rare use-after-free. Unlike Chromium
// this checks the OS thread, not the sequence: an object whose sequence hops
// between pool threads can't use WeakPtrs (none does here).

#ifndef COMMON_BASE_MEMORY_WEAK_PTR_H_
#define COMMON_BASE_MEMORY_WEAK_PTR_H_

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/threading/current_thread_id.h"

namespace base {

template <typename T>
class WeakPtrFactory;

namespace internal {
// Shared validity flag.
class WeakFlag : public RefCountedThreadSafe<WeakFlag> {
 public:
  // The owner's thread; called when the first WeakPtr is handed out.
  void Bind() { CheckThread(); }
  bool IsValid() {
    CheckThread();
    return valid_.load(std::memory_order_acquire);
  }
  void Invalidate() {
    CheckThread();
    valid_.store(false, std::memory_order_release);
  }

 private:
  void CheckThread() {
    const int current = CurrentThreadId();
    int bound = 0;
    if (!bound_thread_.compare_exchange_strong(bound, current) &&
        bound != current) {
      std::fprintf(stderr,
                   "WeakPtr used on thread %d but bound to thread %d: "
                   "dereference and invalidate on the owner's sequence only\n",
                   current, bound);
      std::abort();
    }
  }

  std::atomic<bool> valid_{true};
  std::atomic<int> bound_thread_{0};

  friend class RefCountedThreadSafe<WeakFlag>;
  ~WeakFlag() = default;
};
}  // namespace internal

template <typename T>
class WeakPtr {
 public:
  WeakPtr() = default;

  T* get() const { return (flag_ && flag_->IsValid()) ? ptr_ : nullptr; }
  T* operator->() const { return get(); }
  T& operator*() const { return *get(); }
  explicit operator bool() const { return get() != nullptr; }

 private:
  friend class WeakPtrFactory<T>;
  WeakPtr(T* ptr, scoped_refptr<internal::WeakFlag> flag)
      : ptr_(ptr), flag_(std::move(flag)) {}

  // May dangle by design: only dereferenced while `flag_` says it's valid.
  MayBeDangling<T> ptr_ = nullptr;
  scoped_refptr<internal::WeakFlag> flag_;
};

// Declare as the last member of the owning class so it is destroyed first.
template <typename T>
class WeakPtrFactory {
 public:
  explicit WeakPtrFactory(T* ptr) : ptr_(ptr) {}
  WeakPtrFactory(const WeakPtrFactory&) = delete;
  WeakPtrFactory& operator=(const WeakPtrFactory&) = delete;
  ~WeakPtrFactory() { InvalidateWeakPtrs(); }

  WeakPtr<T> GetWeakPtr() {
    if (!flag_) {
      flag_ = MakeRefCounted<internal::WeakFlag>();
    }
    flag_->Bind();
    return WeakPtr<T>(ptr_, flag_);
  }

  void InvalidateWeakPtrs() {
    if (flag_) {
      flag_->Invalidate();
      flag_.reset();
    }
  }

  bool HasWeakPtrs() const { return flag_ && !flag_->HasOneRef(); }

 private:
  // MayBeDangling: the factory is a member of *ptr_ and dies with it.
  const MayBeDangling<T> ptr_;
  scoped_refptr<internal::WeakFlag> flag_;
};

}  // namespace base

#endif  // COMMON_BASE_MEMORY_WEAK_PTR_H_
