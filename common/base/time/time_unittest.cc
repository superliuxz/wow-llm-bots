// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/time/time.h"

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
