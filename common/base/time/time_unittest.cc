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

}  // namespace
}  // namespace base
