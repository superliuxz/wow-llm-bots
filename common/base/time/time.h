// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// TimeDelta and TimeTicks: durations and monotonic time points with microsecond
// resolution.

#ifndef COMMON_BASE_TIME_TIME_H_
#define COMMON_BASE_TIME_TIME_H_

#include <compare>  // IWYU pragma: keep (defaulted operator<=>)
#include <cstdint>

namespace base {

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
    return TimeDelta(us_ + other.us_);
  }
  constexpr TimeDelta operator-(TimeDelta other) const {
    return TimeDelta(us_ - other.us_);
  }
  constexpr TimeDelta operator*(int64_t n) const { return TimeDelta(us_ * n); }
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
  return TimeDelta::FromMicroseconds(n * 1000);
}
constexpr TimeDelta Seconds(int64_t n) {
  return TimeDelta::FromMicroseconds(n * 1000000);
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
    return TimeTicks(us_ + delta.InMicroseconds());
  }
  constexpr TimeTicks operator-(TimeDelta delta) const {
    return TimeTicks(us_ - delta.InMicroseconds());
  }
  constexpr TimeDelta operator-(TimeTicks other) const {
    return Microseconds(us_ - other.us_);
  }
  constexpr auto operator<=>(const TimeTicks&) const = default;

 private:
  explicit constexpr TimeTicks(int64_t us) : us_(us) {}
  int64_t us_ = 0;
};

}  // namespace base

#endif  // COMMON_BASE_TIME_TIME_H_
