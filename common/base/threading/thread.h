// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_THREADING_THREAD_H_
#define COMMON_BASE_THREADING_THREAD_H_

#include <memory>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_epoll.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_queue.h"
#include "base/threading/platform_thread.h"

namespace base {

// A thread running a sequenced task loop on a message pump, like Chromium's
// base::Thread. Use Options{MessagePumpType::IO} for a thread that can watch
// sockets (see CurrentIOThread).
class Thread {
 public:
  struct Options {
    MessagePumpType message_pump_type = MessagePumpType::DEFAULT;
  };

  explicit Thread(std::string name);
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  ~Thread();

  bool Start() { return StartWithOptions(Options()); }
  bool StartWithOptions(const Options& options);
  // Runs tasks already posted, then joins. Later posts are dropped.
  void Stop();

  scoped_refptr<SequencedTaskRunner> task_runner() const { return queue_; }
  const std::string& name() const { return name_; }
  bool IsRunning() const { return !handle_.is_null(); }

 private:
  class Delegate;

  const std::string name_;
  scoped_refptr<TaskQueue> queue_;
  std::unique_ptr<MessagePump> pump_;
  std::unique_ptr<Delegate> delegate_;
  PlatformThreadHandle handle_;
};

// Access to the IO message pump of the current thread, like Chromium's
// base::CurrentIOThread:
//   CurrentIOThread::Get()->WatchFileDescriptor(fd, true, WATCH_READ, ...);
class CurrentIOThread {
 public:
  // True when called on a base::Thread started with MessagePumpType::IO.
  static bool IsSet();
  // Only valid on such a thread.
  static CurrentIOThread* Get();

  bool WatchFileDescriptor(int fd,
                           bool persistent,
                           int mode,
                           MessagePumpForIO::FdWatchController* controller,
                           MessagePumpForIO::FdWatcher* watcher);

 private:
  friend class Thread;
  explicit CurrentIOThread(MessagePumpForIO* pump) : pump_(pump) {}
  MessagePumpForIO* const pump_;
};

}  // namespace base

#endif  // COMMON_BASE_THREADING_THREAD_H_
