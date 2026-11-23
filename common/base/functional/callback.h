// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Minimal OnceCallback / RepeatingCallback / BindOnce / BindRepeating. Plain
// C++20 so it can be included from AzerothCore modules.

#ifndef COMMON_BASE_FUNCTIONAL_CALLBACK_H_
#define COMMON_BASE_FUNCTIONAL_CALLBACK_H_

#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"

namespace base {

template <typename Signature>
class OnceCallback;
template <typename Signature>
class RepeatingCallback;

// Move-only callable that may be run at most once (Run consumes it).
template <typename R, typename... Args>
class OnceCallback<R(Args...)> {
 public:
  OnceCallback() = default;
  template <typename F,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, OnceCallback> &&
                std::is_invocable_r_v<R, std::decay_t<F>&, Args...>>>
  OnceCallback(F&& f)  // NOLINT(google-explicit-constructor)
      : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}

  OnceCallback(OnceCallback&&) noexcept = default;
  OnceCallback& operator=(OnceCallback&&) noexcept = default;
  OnceCallback(const OnceCallback&) = delete;
  OnceCallback& operator=(const OnceCallback&) = delete;

  explicit operator bool() const { return static_cast<bool>(impl_); }
  bool is_null() const { return !impl_; }

  R Run(Args... args) && {
    std::unique_ptr<Concept> impl = std::move(impl_);
    return impl->Invoke(std::forward<Args>(args)...);
  }

 private:
  struct Concept {
    virtual ~Concept() = default;
    virtual R Invoke(Args&&... args) = 0;
  };
  template <typename F>
  struct Model : Concept {
    explicit Model(F f) : fn(std::move(f)) {}
    R Invoke(Args&&... args) override {
      return std::invoke(fn, std::forward<Args>(args)...);
    }
    F fn;
  };
  std::unique_ptr<Concept> impl_;
};

// Copyable callable that may be run many times.
template <typename R, typename... Args>
class RepeatingCallback<R(Args...)> {
 public:
  RepeatingCallback() = default;
  template <typename F,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, RepeatingCallback> &&
                std::is_invocable_r_v<R, std::decay_t<F>&, Args...>>>
  RepeatingCallback(F&& f)  // NOLINT(google-explicit-constructor)
      : impl_(MakeRefCounted<Model<std::decay_t<F>>>(std::forward<F>(f))) {}

  explicit operator bool() const { return static_cast<bool>(impl_); }
  bool is_null() const { return !impl_; }

  R Run(Args... args) const {
    // Pin impl_ locally in case Invoke destroys this RepeatingCallback.
    scoped_refptr<Concept> impl = impl_;
    return impl->Invoke(std::forward<Args>(args)...);
  }

 private:
  struct Concept : RefCountedThreadSafe<Concept> {
    virtual ~Concept() = default;
    virtual R Invoke(Args&&... args) = 0;
  };
  template <typename F>
  struct Model : Concept {
    explicit Model(F f) : fn(std::move(f)) {}
    R Invoke(Args&&... args) override {
      return std::invoke(fn, std::forward<Args>(args)...);
    }
    F fn;
  };
  scoped_refptr<Concept> impl_;
};

using OnceClosure = OnceCallback<void()>;
using RepeatingClosure = RepeatingCallback<void()>;

namespace internal {

template <typename T>
struct IsWeakPtr : std::false_type {};
template <typename T>
struct IsWeakPtr<WeakPtr<T>> : std::true_type {};

template <typename... Ts>
constexpr bool kFirstIsWeakPtr = false;
template <typename First, typename... Rest>
constexpr bool kFirstIsWeakPtr<First, Rest...> =
    IsWeakPtr<std::decay_t<First>>::value;

}  // namespace internal

// Binds leading arguments; the result converts to OnceCallback<...>. When the
// first bound argument is a WeakPtr (method on an object), the call is skipped
// once the WeakPtr is invalidated.
template <typename F, typename... BoundArgs>
auto BindOnce(F&& fn, BoundArgs&&... bound) {
  if constexpr (internal::kFirstIsWeakPtr<BoundArgs...>) {
    return [fn = std::forward<F>(fn),
            bound = std::make_tuple(std::forward<BoundArgs>(bound)...)](
               auto&&... unbound) mutable {
      std::apply(
          [&](auto& weak, auto&... rest) {
            if (weak) {
              std::invoke(fn, weak.get(), std::move(rest)...,
                          std::forward<decltype(unbound)>(unbound)...);
            }
          },
          bound);
    };
  } else {
    return [fn = std::forward<F>(fn),
            bound = std::make_tuple(std::forward<BoundArgs>(bound)...)](
               auto&&... unbound) mutable -> decltype(auto) {
      return std::apply(
          [&](auto&... b) -> decltype(auto) {
            return std::invoke(std::move(fn), std::move(b)...,
                               std::forward<decltype(unbound)>(unbound)...);
          },
          bound);
    };
  }
}

// Binds leading arguments; the result converts to RepeatingCallback<...>.
// Bound arguments are copied into each call. WeakPtr receivers as in BindOnce.
template <typename F, typename... BoundArgs>
auto BindRepeating(F&& fn, BoundArgs&&... bound) {
  if constexpr (internal::kFirstIsWeakPtr<BoundArgs...>) {
    return [fn = std::forward<F>(fn),
            bound = std::make_tuple(std::forward<BoundArgs>(bound)...)](
               auto&&... unbound) {
      std::apply(
          [&](const auto& weak, const auto&... rest) {
            if (weak) {
              std::invoke(fn, weak.get(), rest...,
                          std::forward<decltype(unbound)>(unbound)...);
            }
          },
          bound);
    };
  } else {
    return [fn = std::forward<F>(fn),
            bound = std::make_tuple(std::forward<BoundArgs>(bound)...)](
               auto&&... unbound) -> decltype(auto) {
      return std::apply(
          [&](const auto&... b) -> decltype(auto) {
            return std::invoke(fn, b...,
                               std::forward<decltype(unbound)>(unbound)...);
          },
          bound);
    };
  }
}

}  // namespace base

#endif  // COMMON_BASE_FUNCTIONAL_CALLBACK_H_
