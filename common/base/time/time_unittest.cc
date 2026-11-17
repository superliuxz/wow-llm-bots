// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/time/time.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace base {
namespace {

TEST(TimeDeltaTest, ConstructorsAndConversions) {
  EXPECT_EQ(Seconds(2).InMilliseconds(), 2000);
  EXPECT_EQ(Milliseconds(3).InMicroseconds(), 3000);
  EXPECT_EQ(Minutes(1), Seconds(60));
  EXPECT_DOUBLE_EQ(Milliseconds(1500).InSecondsF(), 1.5);
  EXPECT_TRUE(TimeDelta().is_zero());
  EXPECT_TRUE((Milliseconds(1) - Milliseconds(2)).is_negative());
}

TEST(TimeTicksTest, IsMonotonicAndSupportsArithmetic) {
  const TimeTicks a = TimeTicks::Now();
  const TimeTicks b = TimeTicks::Now();
  EXPECT_LE(a, b);
  EXPECT_EQ((a + Seconds(1)) - a, Seconds(1));
  EXPECT_FALSE(a.is_null());
}

TEST(TimeTicksTest, ArithmeticSaturatesInsteadOfOverflowing) {
  const TimeTicks now = TimeTicks::Now();
  // "Wait forever" is Now() + Max(): the far future, not a wrapped past.
  EXPECT_GT(now + TimeDelta::Max(), now);
  EXPECT_EQ(
      (now + TimeDelta::Max()) - now,
      TimeDelta::Max() - Microseconds((now - TimeTicks()).InMicroseconds()));
  EXPECT_EQ(TimeDelta::Max() + Seconds(1), TimeDelta::Max());
  EXPECT_EQ(TimeDelta::Max() * 2, TimeDelta::Max());
  EXPECT_TRUE((TimeDelta() - TimeDelta::Max() - Seconds(5)).is_negative());
  EXPECT_EQ(Minutes(INT64_MAX), TimeDelta::Max());
  EXPECT_GT(Time::FromMillisecondsSinceUnixEpoch(INT64_MAX), Time::Now());
}

TEST(TimeTest, NowIsWallClockTime) {
  // 2026-01-01T00:00:00Z: any real "now" is later.
  const Time earliest = Time::FromMillisecondsSinceUnixEpoch(1767225600000);
  const Time now = Time::Now();
  EXPECT_FALSE(now.is_null());
  EXPECT_GT(now, earliest);
}

TEST(TimeTest, MillisecondsSinceUnixEpochRoundTrip) {
  const Time t = Time::FromMillisecondsSinceUnixEpoch(1794729600123);
  EXPECT_EQ(t.InMillisecondsSinceUnixEpoch(), 1794729600123);
  EXPECT_EQ((t + Seconds(2)).InMillisecondsSinceUnixEpoch(), 1794729602123);
  EXPECT_EQ((t + Milliseconds(5)) - t, Milliseconds(5));
  EXPECT_TRUE(Time().is_null());
}

}  // namespace
}  // namespace base
