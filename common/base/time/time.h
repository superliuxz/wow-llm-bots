// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// TimeDelta, TimeTicks and Time: durations, monotonic time points and
// wall-clock time points, with microsecond resolution.

#ifndef COMMON_BASE_TIME_TIME_H_
#define COMMON_BASE_TIME_TIME_H_

#include <compare>  // IWYU pragma: keep (defaulted operator<=>)
#include <cstdint>

namespace base {
namespace internal {

// As in Chromium, time arithmetic saturates instead of overflowing:
// TimeTicks::Now() + TimeDelta::Max() is the far future, not undefined
// behavior.
constexpr int64_t SaturatedAdd(int64_t a, int64_t b) {
  int64_t sum = 0;
  if (__builtin_add_overflow(a, b, &sum)) {
    return b > 0 ? INT64_MAX : INT64_MIN;
  }
  return sum;
}
constexpr int64_t SaturatedSub(int64_t a, int64_t b) {
  int64_t difference = 0;
  if (__builtin_sub_overflow(a, b, &difference)) {
    return b < 0 ? INT64_MAX : INT64_MIN;
  }
  return difference;
}
constexpr int64_t SaturatedMul(int64_t a, int64_t b) {
  int64_t product = 0;
  if (__builtin_mul_overflow(a, b, &product)) {
    return (a < 0) != (b < 0) ? INT64_MIN : INT64_MAX;
  }
  return product;
}

}  // namespace internal

class TimeDelta {
 public:
  constexpr TimeDelta() = default;

  static constexpr TimeDelta FromMicroseconds(int64_t us) {
    return TimeDelta(us);
  }
  static constexpr TimeDelta Max() { return TimeDelta(INT64_MAX); }

  constexpr int64_t InMicroseconds() const { return us_; }
  constexpr int64_t InMilliseconds() const { return us_ / 1000; }
  constexpr double InSecondsF() const { return us_ / 1e6; }

  constexpr bool is_zero() const { return us_ == 0; }
  constexpr bool is_positive() const { return us_ > 0; }
  constexpr bool is_negative() const { return us_ < 0; }

  constexpr TimeDelta operator+(TimeDelta other) const {
    return TimeDelta(internal::SaturatedAdd(us_, other.us_));
  }
  constexpr TimeDelta operator-(TimeDelta other) const {
    return TimeDelta(internal::SaturatedSub(us_, other.us_));
  }
  constexpr TimeDelta operator*(int64_t n) const {
    return TimeDelta(internal::SaturatedMul(us_, n));
  }
  constexpr TimeDelta operator/(int64_t n) const { return TimeDelta(us_ / n); }
  constexpr auto operator<=>(const TimeDelta&) const = default;

 private:
  explicit constexpr TimeDelta(int64_t us) : us_(us) {}
  int64_t us_ = 0;
};

constexpr TimeDelta Microseconds(int64_t n) {
  return TimeDelta::FromMicroseconds(n);
}
constexpr TimeDelta Milliseconds(int64_t n) {
  return TimeDelta::FromMicroseconds(internal::SaturatedMul(n, 1000));
}
constexpr TimeDelta Seconds(int64_t n) {
  return TimeDelta::FromMicroseconds(internal::SaturatedMul(n, 1000000));
}
constexpr TimeDelta Minutes(int64_t n) {
  return Seconds(n * 60);
}

// A point on a monotonic clock (never jumps; unrelated to wall-clock time).
class TimeTicks {
 public:
  constexpr TimeTicks() = default;

  static TimeTicks Now();

  constexpr bool is_null() const { return us_ == 0; }

  constexpr TimeTicks operator+(TimeDelta delta) const {
    return TimeTicks(internal::SaturatedAdd(us_, delta.InMicroseconds()));
  }
  constexpr TimeTicks operator-(TimeDelta delta) const {
    return TimeTicks(internal::SaturatedSub(us_, delta.InMicroseconds()));
  }
  constexpr TimeDelta operator-(TimeTicks other) const {
    return Microseconds(internal::SaturatedSub(us_, other.us_));
  }
  constexpr auto operator<=>(const TimeTicks&) const = default;

 private:
  explicit constexpr TimeTicks(int64_t us) : us_(us) {}
  int64_t us_ = 0;
};

// A point on the wall clock (UTC). It can jump when the system clock is set:
// use it for timestamps that people and logs read, and TimeTicks to measure
// durations or schedule work.
class Time {
 public:
  constexpr Time() = default;

  static Time Now();
  static constexpr Time FromMillisecondsSinceUnixEpoch(int64_t ms) {
    return Time(ms * 1000);
  }

  constexpr int64_t InMillisecondsSinceUnixEpoch() const { return us_ / 1000; }
  constexpr bool is_null() const { return us_ == 0; }

  constexpr Time operator+(TimeDelta delta) const {
    return Time(internal::SaturatedAdd(us_, delta.InMicroseconds()));
  }
  constexpr Time operator-(TimeDelta delta) const {
    return Time(internal::SaturatedSub(us_, delta.InMicroseconds()));
  }
  constexpr TimeDelta operator-(Time other) const {
    return Microseconds(internal::SaturatedSub(us_, other.us_));
  }
  constexpr auto operator<=>(const Time&) const = default;

 private:
  // Microseconds since the Unix epoch.
  explicit constexpr Time(int64_t us) : us_(us) {}
  int64_t us_ = 0;
};

}  // namespace base

#endif  // COMMON_BASE_TIME_TIME_H_
