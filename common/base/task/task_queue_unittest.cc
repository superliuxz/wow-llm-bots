// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/task/task_queue.h"

#include <vector>

#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"

namespace base {
namespace {

TEST(TaskQueueTest, RunsTasksInPostingOrder) {
  auto queue = TaskQueue::Create();
  std::vector<int> order;
  for (int i = 0; i < 5; ++i) {
    queue->PostTask([&order, i] { order.push_back(i); });
  }
  EXPECT_EQ(queue->RunPendingTasks(), 5u);
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(TaskQueueTest, TasksPostedWhileRunningWaitForNextPump) {
  auto queue = TaskQueue::Create();
  int runs = 0;
  queue->PostTask([&] {
    ++runs;
    queue->PostTask([&] { ++runs; });
  });
  queue->RunPendingTasks();
  EXPECT_EQ(runs, 1);
  queue->RunPendingTasks();
  EXPECT_EQ(runs, 2);
}

TEST(TaskQueueTest, DelayedTaskRunsOnlyWhenDue) {
  auto queue = TaskQueue::Create();
  bool ran = false;
  queue->PostDelayedTask([&] { ran = true; }, Milliseconds(30));
  queue->RunPendingTasks();
  EXPECT_FALSE(ran);
  PlatformThread::Sleep(Milliseconds(40));
  queue->RunPendingTasks();
  EXPECT_TRUE(ran);
}

TEST(TaskQueueTest, CurrentDefaultAndSequenceChecks) {
  auto queue = TaskQueue::Create();
  EXPECT_FALSE(queue->RunsTasksInCurrentSequence());
  EXPECT_FALSE(SequencedTaskRunner::HasCurrentDefault());
  bool checked = false;
  queue->PostTask([&] {
    EXPECT_TRUE(queue->RunsTasksInCurrentSequence());
    EXPECT_EQ(SequencedTaskRunner::GetCurrentDefault().get(), queue.get());
    checked = true;
  });
  queue->RunPendingTasks();
  EXPECT_TRUE(checked);
  EXPECT_FALSE(SequencedTaskRunner::HasCurrentDefault());
}

TEST(TaskQueueTest, ShutdownRejectsAndDestroysTasks) {
  auto queue = TaskQueue::Create();
  struct Token : RefCountedThreadSafe<Token> {};
  auto token = MakeRefCounted<Token>();
  queue->PostTask([token] {});
  EXPECT_FALSE(token->HasOneRef());
  queue->Shutdown();
  EXPECT_TRUE(token->HasOneRef());
  EXPECT_FALSE(queue->PostTask([] {}));
}

TEST(TaskQueueTest, WakeupCallbackFiresOnPost) {
  auto queue = TaskQueue::Create();
  int wakeups = 0;
  queue->SetWakeupCallback([&] { ++wakeups; });
  queue->PostTask([] {});
  queue->PostDelayedTask([] {}, Milliseconds(10));
  EXPECT_EQ(wakeups, 2);
}

}  // namespace
}  // namespace base
