// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// raw_ptr<T>: a non-owning pointer for class fields with a dangling pointer
// detector, by opt-in: if T derives from base::RawPtrTarget, every raw_ptr<T>
// holds a count on the object, and destroying the object while any raw_ptr<T>
// still points to it crashes immediately ("dangling raw_ptr") instead of
// becoming a use-after-free later. For other T, raw_ptr<T> is a plain pointer.
//
// Use raw_ptr<T> for pointer fields (not locals or parameters). Fields that may
// legitimately dangle (e.g. WeakPtr's target) use MayBeDangling<T>.

#ifndef COMMON_BASE_MEMORY_RAW_PTR_H_
#define COMMON_BASE_MEMORY_RAW_PTR_H_

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>

namespace base {

enum class RawPtrTraits {
  kEmpty,
  kMayDangle,  // No dangling detection (like DisableDanglingPtrDetection).
};

template <typename T, RawPtrTraits Traits>
class raw_ptr;

// Base class that makes raw_ptr<Derived> detect dangling pointers.
class RawPtrTarget {
 protected:
  RawPtrTarget() = default;
  RawPtrTarget(const RawPtrTarget&) {}  // Copies start with no raw_ptrs.
  RawPtrTarget& operator=(const RawPtrTarget&) { return *this; }
  ~RawPtrTarget() {
    const int count = raw_ptr_count_.load(std::memory_order_acquire);
    if (count != 0) {
      std::fprintf(stderr,
                   "FATAL: dangling raw_ptr: object destroyed while %d "
                   "raw_ptr(s) still point to it\n",
                   count);
      std::abort();
    }
  }

 private:
  template <typename T, RawPtrTraits Traits>
  friend class raw_ptr;
  mutable std::atomic<int> raw_ptr_count_{0};
};

template <typename T, RawPtrTraits Traits = RawPtrTraits::kEmpty>
class raw_ptr {
 public:
  constexpr raw_ptr() = default;
  constexpr raw_ptr(std::nullptr_t) {}    // NOLINT
  raw_ptr(T* p) : ptr_(p) { Acquire(); }  // NOLINT: implicit like T*
  raw_ptr(const raw_ptr& other) : ptr_(other.ptr_) { Acquire(); }
  raw_ptr(raw_ptr&& other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)) {}
  ~raw_ptr() { ReleaseRef(); }

  raw_ptr& operator=(const raw_ptr& other) {
    if (this != &other) {
      ReleaseRef();
      ptr_ = other.ptr_;
      Acquire();
    }
    return *this;
  }
  raw_ptr& operator=(raw_ptr&& other) noexcept {
    if (this != &other) {
      ReleaseRef();
      ptr_ = std::exchange(other.ptr_, nullptr);
    }
    return *this;
  }
  raw_ptr& operator=(T* p) {
    ReleaseRef();
    ptr_ = p;
    Acquire();
    return *this;
  }
  raw_ptr& operator=(std::nullptr_t) {
    ReleaseRef();
    ptr_ = nullptr;
    return *this;
  }

  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  T& operator*() const { return *ptr_; }
  operator T*() const { return ptr_; }  // NOLINT: implicit like T*
  explicit operator bool() const { return ptr_ != nullptr; }

  // Deletes the pointee after dropping this reference.
  void ClearAndDelete() {
    T* p = ptr_;
    ReleaseRef();
    ptr_ = nullptr;
    delete p;
  }
  // Drops this reference and returns the pointer (now unchecked).
  T* ExtractAsDangling() {
    T* p = ptr_;
    ReleaseRef();
    ptr_ = nullptr;
    return p;
  }

 private:
  static constexpr bool Counted() {
    return Traits != RawPtrTraits::kMayDangle &&
           std::is_base_of_v<RawPtrTarget, std::remove_cv_t<T>>;
  }
  void Acquire() {
    if constexpr (Counted()) {
      if (ptr_) {
        static_cast<const RawPtrTarget*>(ptr_)->raw_ptr_count_.fetch_add(
            1, std::memory_order_relaxed);
      }
    }
  }
  void ReleaseRef() {
    if constexpr (Counted()) {
      if (ptr_) {
        static_cast<const RawPtrTarget*>(ptr_)->raw_ptr_count_.fetch_sub(
            1, std::memory_order_release);
      }
    }
  }

  T* ptr_ = nullptr;
};

template <typename T>
using MayBeDangling = raw_ptr<T, RawPtrTraits::kMayDangle>;

}  // namespace base

#endif  // COMMON_BASE_MEMORY_RAW_PTR_H_
