// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/threading/thread.h"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_epoll.h"
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

TEST(ThreadTest, CanRestartAfterStop) {
  Thread thread("test");
  for (int round = 0; round < 3; ++round) {
    ASSERT_TRUE(thread.Start());
    WaitableEvent done;
    EXPECT_TRUE(thread.task_runner()->PostTask([&] { done.Signal(); }));
    EXPECT_TRUE(done.TimedWait(Seconds(2)));
    thread.Stop();
  }
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

TEST(ThreadTest, StopWithConcurrentPostTask) {
  for (int round = 0; round < 20; ++round) {
    Thread target("target");
    ASSERT_TRUE(target.Start());
    auto runner = target.task_runner();
    std::atomic<bool> stop_posting{false};
    PlatformThreadHandle poster =
        PlatformThread::Create("poster", [&stop_posting, runner] {
          while (!stop_posting.load(std::memory_order_relaxed)) {
            runner->PostTask([] {});
          }
        });
    PlatformThread::Sleep(Milliseconds(1));
    target.Stop();
    stop_posting.store(true, std::memory_order_relaxed);
    PlatformThread::Join(poster);
  }
}

TEST(PlatformThreadTest, SleepNonPositiveReturnsImmediately) {
  const TimeTicks start = TimeTicks::Now();
  PlatformThread::Sleep(Microseconds(-1000));
  PlatformThread::Sleep(TimeDelta());
  EXPECT_LT(TimeTicks::Now() - start, Milliseconds(100));
}

class RecordingWatcher : public MessagePumpEpoll::FdWatcher {
 public:
  void OnFileCanReadWithoutBlocking(int fd) override {
    read_fd = fd;
    ++reads;
    on_read.Signal();
  }
  void OnFileCanWriteWithoutBlocking(int fd) override {
    write_fd = fd;
    ++writes;
    on_write.Signal();
  }
  int read_fd = -1;
  int write_fd = -1;
  int reads = 0;
  int writes = 0;
  WaitableEvent on_read;
  WaitableEvent on_write;
};

class HandoffWatcher : public MessagePumpEpoll::FdWatcher {
 public:
  HandoffWatcher(MessagePumpEpoll::FdWatchController* next_controller,
                 MessagePumpEpoll::FdWatcher* next_watcher)
      : next_controller_(next_controller), next_watcher_(next_watcher) {}

  void OnFileCanReadWithoutBlocking(int /*fd*/) override {}
  void OnFileCanWriteWithoutBlocking(int fd) override {
    handoff_ok = CurrentIOThread::Get()->WatchFileDescriptor(
        fd, /*persistent=*/false, MessagePumpForIO::WATCH_READ,
        next_controller_, next_watcher_);
    done.Signal();
  }

  bool handoff_ok = false;
  WaitableEvent done;

 private:
  MessagePumpEpoll::FdWatchController* next_controller_;
  MessagePumpEpoll::FdWatcher* next_watcher_;
};

TEST(MessagePumpEpollTest, OneShotReadWriteAndControllerReuse) {
  int p1[2] = {-1, -1};
  int p2[2] = {-1, -1};
  ASSERT_EQ(pipe(p1), 0);
  ASSERT_EQ(pipe(p2), 0);
  const char byte = 'x';
  ASSERT_EQ(write(p1[1], &byte, 1), 1);
  ASSERT_EQ(write(p2[1], &byte, 1), 1);

  RecordingWatcher watcher;
  RecordingWatcher rw_watcher;
  RecordingWatcher handoff_read_watcher;
  MessagePumpEpoll::FdWatchController controller;
  MessagePumpEpoll::FdWatchController next_controller;
  HandoffWatcher handoff_watcher(&next_controller, &handoff_read_watcher);
  Thread io("io");
  ASSERT_TRUE(io.StartWithOptions({MessagePumpType::IO}));

  io.task_runner()->PostTask([&] {
    // Watch p1[0], then immediately reuse the same controller for p2[0].
    EXPECT_TRUE(CurrentIOThread::Get()->WatchFileDescriptor(
        p1[0], /*persistent=*/true, MessagePumpForIO::WATCH_READ, &controller,
        &watcher));
    EXPECT_TRUE(CurrentIOThread::Get()->WatchFileDescriptor(
        p2[0], /*persistent=*/false, MessagePumpForIO::WATCH_READ, &controller,
        &watcher));
  });
  ASSERT_TRUE(watcher.on_read.TimedWait(Seconds(2)));

  WaitableEvent check1;
  io.task_runner()->PostTask([&] {
    EXPECT_EQ(watcher.read_fd, p2[0]);
    EXPECT_EQ(watcher.reads, 1);
    EXPECT_FALSE(controller.is_watching());
    check1.Signal();
  });
  ASSERT_TRUE(check1.TimedWait(Seconds(2)));

  // Non-persistent WATCH_READ_WRITE fires both write and read before stopping.
  int sp[2] = {-1, -1};
  ASSERT_EQ(
      socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sp),
      0);
  ASSERT_EQ(write(sp[1], &byte, 1), 1);
  io.task_runner()->PostTask([&] {
    EXPECT_TRUE(CurrentIOThread::Get()->WatchFileDescriptor(
        sp[0], /*persistent=*/false, MessagePumpForIO::WATCH_READ_WRITE,
        &controller, &rw_watcher));
  });
  ASSERT_TRUE(rw_watcher.on_write.TimedWait(Seconds(2)));
  ASSERT_TRUE(rw_watcher.on_read.TimedWait(Seconds(2)));

  WaitableEvent rw_check;
  io.task_runner()->PostTask([&] {
    EXPECT_EQ(rw_watcher.write_fd, sp[0]);
    EXPECT_EQ(rw_watcher.read_fd, sp[0]);
    EXPECT_EQ(rw_watcher.writes, 1);
    EXPECT_EQ(rw_watcher.reads, 1);
    EXPECT_FALSE(controller.is_watching());
    rw_check.Signal();
  });
  ASSERT_TRUE(rw_check.TimedWait(Seconds(2)));

  // One-shot WATCH_WRITE can hand off the same fd to a new FdWatchController
  // (as UnixSocketClient does on async EINPROGRESS connect ->
  // FramedConnection).
  io.task_runner()->PostTask([&] {
    EXPECT_TRUE(CurrentIOThread::Get()->WatchFileDescriptor(
        sp[0], /*persistent=*/false, MessagePumpForIO::WATCH_WRITE, &controller,
        &handoff_watcher));
  });
  ASSERT_TRUE(handoff_watcher.done.TimedWait(Seconds(2)));
  ASSERT_TRUE(handoff_read_watcher.on_read.TimedWait(Seconds(2)));
  EXPECT_TRUE(handoff_watcher.handoff_ok);
  EXPECT_EQ(handoff_read_watcher.read_fd, sp[0]);

  io.Stop();
  close(sp[0]);
  close(sp[1]);
  close(p1[0]);
  close(p1[1]);
  close(p2[0]);
  close(p2[1]);
}

}  // namespace
}  // namespace base
