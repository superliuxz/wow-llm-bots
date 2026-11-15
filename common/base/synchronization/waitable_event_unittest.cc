// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/synchronization/waitable_event.h"

#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"

namespace base {
namespace {

TEST(WaitableEventTest, ManualResetStaysSignaled) {
  WaitableEvent event;
  EXPECT_FALSE(event.IsSignaled());
  event.Signal();
  EXPECT_TRUE(event.IsSignaled());
  EXPECT_TRUE(event.IsSignaled());
  event.Reset();
  EXPECT_FALSE(event.IsSignaled());
}

TEST(WaitableEventTest, AutoResetClearsAfterWait) {
  WaitableEvent event(WaitableEvent::ResetPolicy::AUTOMATIC);
  event.Signal();
  event.Wait();
  EXPECT_FALSE(event.IsSignaled());
}

TEST(WaitableEventTest, TimedWaitTimesOut) {
  WaitableEvent event;
  const TimeTicks start = TimeTicks::Now();
  EXPECT_FALSE(event.TimedWait(Milliseconds(30)));
  EXPECT_GE(TimeTicks::Now() - start, Milliseconds(25));
}

TEST(WaitableEventTest, SignaledFromAnotherThread) {
  Thread thread("signaler");
  thread.Start();
  WaitableEvent event;
  thread.task_runner()->PostDelayedTask([&] { event.Signal(); },
                                        Milliseconds(10));
  EXPECT_TRUE(event.TimedWait(Seconds(2)));
}

}  // namespace
}  // namespace base
