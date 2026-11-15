// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/threading/thread.h"

#include <atomic>

#include "base/synchronization/waitable_event.h"
#include "base/task/post_task_and_reply.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"

namespace base {
namespace {

TEST(ThreadTest, RunsPostedTaskOnItsThread) {
  Thread thread("test");
  thread.Start();
  WaitableEvent done;
  int ran_on = 0;
  thread.task_runner()->PostTask([&] {
    ran_on = PlatformThread::CurrentId();
    done.Signal();
  });
  done.Wait();
  EXPECT_NE(ran_on, PlatformThread::CurrentId());
}

TEST(ThreadTest, StopRunsAlreadyPostedTasks) {
  Thread thread("test");
  thread.Start();
  std::atomic<int> runs{0};
  for (int i = 0; i < 100; ++i) {
    thread.task_runner()->PostTask([&] { ++runs; });
  }
  thread.Stop();
  EXPECT_EQ(runs.load(), 100);
  EXPECT_FALSE(thread.task_runner()->PostTask([] {}));
}

TEST(ThreadTest, DelayedTaskRuns) {
  Thread thread("test");
  thread.Start();
  WaitableEvent done;
  const TimeTicks start = TimeTicks::Now();
  thread.task_runner()->PostDelayedTask([&] { done.Signal(); },
                                        Milliseconds(50));
  done.Wait();
  EXPECT_GE(TimeTicks::Now() - start, Milliseconds(45));
}

TEST(ThreadPoolTest, ReplyComesBackOnOriginSequence) {
  Thread origin("origin");
  origin.Start();
  ThreadPool pool("pool", 2);
  WaitableEvent done;
  int value = 0;
  bool on_origin = false;
  origin.task_runner()->PostTask([&] {
    auto origin_runner = SequencedTaskRunner::GetCurrentDefault();
    PostTaskAndReplyWithResult(
        pool, [] { return 6 * 7; },
        [&, origin_runner](int result) {
          value = result;
          on_origin = origin_runner->RunsTasksInCurrentSequence();
          done.Signal();
        });
  });
  done.Wait();
  EXPECT_EQ(value, 42);
  EXPECT_TRUE(on_origin);
}

}  // namespace
}  // namespace base
