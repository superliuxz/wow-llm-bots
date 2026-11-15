// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_MESSAGE_LOOP_MESSAGE_PUMP_EPOLL_H_
#define COMMON_BASE_MESSAGE_LOOP_MESSAGE_PUMP_EPOLL_H_

#include <cstdint>
#include <unordered_map>

#include "base/memory/raw_ptr.h"
#include "base/message_loop/message_pump.h"

namespace base {

// IO message pump on epoll, like Chromium's MessagePumpEpoll (Linux
// MessagePumpForIO). Tasks wake it through an eventfd; delayed tasks set the
// epoll_pwait2 timeout. File descriptors are watched with
// WatchFileDescriptor(), usually via CurrentIOThread::Get().
class MessagePumpEpoll : public MessagePump {
 public:
  enum Mode {
    WATCH_READ = 1 << 0,
    WATCH_WRITE = 1 << 1,
    WATCH_READ_WRITE = WATCH_READ | WATCH_WRITE,
  };

  // Receives readiness notifications. Runs on the pump's thread.
  class FdWatcher : public RawPtrTarget {
   public:
    virtual void OnFileCanReadWithoutBlocking(int fd) = 0;
    virtual void OnFileCanWriteWithoutBlocking(int fd) = 0;

   protected:
    virtual ~FdWatcher() = default;
  };

  // Owns one watch. Destroying it (or StopWatchingFileDescriptor) stops it.
  class FdWatchController : public RawPtrTarget {
   public:
    FdWatchController() = default;
    FdWatchController(const FdWatchController&) = delete;
    FdWatchController& operator=(const FdWatchController&) = delete;
    ~FdWatchController();

    bool StopWatchingFileDescriptor();
    bool is_watching() const { return pump_ != nullptr; }

   private:
    friend class MessagePumpEpoll;
    raw_ptr<MessagePumpEpoll> pump_ = nullptr;
    raw_ptr<FdWatcher> watcher_ = nullptr;
    int fd_ = -1;
    int mode_ = 0;
    bool persistent_ = false;
  };

  MessagePumpEpoll();
  MessagePumpEpoll(const MessagePumpEpoll&) = delete;
  MessagePumpEpoll& operator=(const MessagePumpEpoll&) = delete;
  ~MessagePumpEpoll() override;

  // MessagePump:
  void Run(Delegate* delegate) override;
  void Quit() override;
  void ScheduleWork() override;

  // Starts (or widens, if `controller` already watches `fd`) a watch. A
  // non-persistent watch stops after the first notification. Must be called on
  // the pump's thread. Returns false on failure.
  bool WatchFileDescriptor(int fd,
                           bool persistent,
                           int mode,
                           FdWatchController* controller,
                           FdWatcher* watcher);

 private:
  bool StopWatching(FdWatchController* controller);
  void Dispatch(int fd, uint32_t events);

  const int epoll_fd_;
  const int wake_fd_;
  bool quit_ = false;
  // Watched fd -> its controller. Looked up at dispatch time, so a controller
  // destroyed by an earlier callback in the same epoll batch is never touched.
  std::unordered_map<int, raw_ptr<FdWatchController>> controllers_;
};

using MessagePumpForIO = MessagePumpEpoll;

}  // namespace base

#endif  // COMMON_BASE_MESSAGE_LOOP_MESSAGE_PUMP_EPOLL_H_
