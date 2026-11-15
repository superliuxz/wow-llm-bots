// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// WeakPtr / WeakPtrFactory in the spirit of Chromium's //base/memory/weak_ptr.
// Like Chromium's, they are not thread-safe: create, dereference and invalidate
// on the owning object's sequence only. They may be *copied and passed* across
// threads (e.g. bound into a task posted back to the owner's sequence).

#ifndef COMMON_BASE_MEMORY_WEAK_PTR_H_
#define COMMON_BASE_MEMORY_WEAK_PTR_H_

#include <utility>
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"

namespace base {

template <typename T>
class WeakPtrFactory;

namespace internal {
// Shared validity flag, like Chromium's WeakReference::Flag.
class WeakFlag : public RefCountedThreadSafe<WeakFlag> {
 public:
  bool valid = true;

 private:
  friend class RefCountedThreadSafe<WeakFlag>;
  ~WeakFlag() = default;
};
}  // namespace internal

template <typename T>
class WeakPtr {
 public:
  WeakPtr() = default;

  T* get() const { return (flag_ && flag_->valid) ? ptr_ : nullptr; }
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
    return WeakPtr<T>(ptr_, flag_);
  }

  void InvalidateWeakPtrs() {
    if (flag_) {
      flag_->valid = false;
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
