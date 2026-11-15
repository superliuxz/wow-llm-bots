// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/thread.h"

#include <atomic>
#include <future>
#include <utility>

#include "base/platform_thread.h"
#include "base/post_task_and_reply.h"
#include "base/sequenced_task_runner.h"
#include "base/thread_pool.h"
#include "base/time.h"
#include "gtest/gtest.h"

namespace base {
namespace {

TEST(ThreadTest, RunsPostedTaskOnItsThread) {
  Thread thread("test");
  thread.Start();
  std::promise<int> ran_on;
  thread.task_runner()->PostTask(
      [&] { ran_on.set_value(PlatformThread::CurrentId()); });
  EXPECT_NE(ran_on.get_future().get(), PlatformThread::CurrentId());
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
  std::promise<void> done;
  const TimeTicks start = TimeTicks::Now();
  thread.task_runner()->PostDelayedTask([&] { done.set_value(); },
                                        Milliseconds(50));
  done.get_future().wait();
  EXPECT_GE(TimeTicks::Now() - start, Milliseconds(45));
}

TEST(ThreadPoolTest, ReplyComesBackOnOriginSequence) {
  Thread origin("origin");
  origin.Start();
  ThreadPool pool("pool", 2);
  std::promise<std::pair<int, bool>> result;
  origin.task_runner()->PostTask([&] {
    auto origin_runner = SequencedTaskRunner::GetCurrentDefault();
    PostTaskAndReplyWithResult(
        pool, [] { return 6 * 7; },
        [&, origin_runner](int value) {
          result.set_value(
              {value, origin_runner->RunsTasksInCurrentSequence()});
        });
  });
  auto [value, on_origin] = result.get_future().get();
  EXPECT_EQ(value, 42);
  EXPECT_TRUE(on_origin);
}

}  // namespace
}  // namespace base
