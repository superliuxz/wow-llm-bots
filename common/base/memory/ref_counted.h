// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Intrusive reference counting: RefCountedThreadSafe and scoped_refptr.

#ifndef COMMON_BASE_MEMORY_REF_COUNTED_H_
#define COMMON_BASE_MEMORY_REF_COUNTED_H_

#include <atomic>
#include <cstddef>
#include <utility>

namespace base {

// Base class for thread-safe ref-counted objects. Hold them in scoped_refptr.
template <typename T>
class RefCountedThreadSafe {
 public:
  RefCountedThreadSafe(const RefCountedThreadSafe&) = delete;
  RefCountedThreadSafe& operator=(const RefCountedThreadSafe&) = delete;

  void AddRef() const { count_.fetch_add(1, std::memory_order_relaxed); }
  void Release() const {
    if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete static_cast<const T*>(this);
    }
  }
  bool HasOneRef() const { return count_.load(std::memory_order_acquire) == 1; }

 protected:
  RefCountedThreadSafe() = default;
  ~RefCountedThreadSafe() = default;

 private:
  mutable std::atomic<int> count_{0};
};

template <typename T>
class scoped_refptr {
 public:
  constexpr scoped_refptr() = default;
  constexpr scoped_refptr(std::nullptr_t) {}  // NOLINT
  explicit scoped_refptr(T* p) : ptr_(p) {
    if (ptr_) {
      ptr_->AddRef();
    }
  }
  scoped_refptr(const scoped_refptr& other) : scoped_refptr(other.ptr_) {}
  scoped_refptr(scoped_refptr&& other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)) {}
  template <typename U>
  scoped_refptr(const scoped_refptr<U>& other)  // NOLINT: upcast
      : scoped_refptr(other.get()) {}
  template <typename U>
  scoped_refptr(scoped_refptr<U>&& other) noexcept  // NOLINT: upcast
      : ptr_(other.release()) {}
  ~scoped_refptr() {
    if (ptr_) {
      ptr_->Release();
    }
  }

  scoped_refptr& operator=(scoped_refptr other) noexcept {
    std::swap(ptr_, other.ptr_);
    return *this;
  }

  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  T& operator*() const { return *ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  bool operator==(const scoped_refptr& other) const = default;

  // Gives up ownership without releasing.
  T* release() { return std::exchange(ptr_, nullptr); }
  void reset() { scoped_refptr().swap(*this); }
  void swap(scoped_refptr& other) noexcept { std::swap(ptr_, other.ptr_); }

 private:
  T* ptr_ = nullptr;
};

template <typename T, typename... Args>
scoped_refptr<T> MakeRefCounted(Args&&... args) {
  return scoped_refptr<T>(new T(std::forward<Args>(args)...));
}

}  // namespace base

#endif  // COMMON_BASE_MEMORY_REF_COUNTED_H_
