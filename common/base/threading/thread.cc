// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/threading/thread.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include "base/memory/raw_ptr.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_epoll.h"
#include "base/task/task_queue.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"

namespace base {
namespace {
thread_local CurrentIOThread* g_current_io_thread = nullptr;
}  // namespace

// Runs the thread's task queue from inside its message pump.
class Thread::Delegate : public MessagePump::Delegate {
 public:
  explicit Delegate(TaskQueue* queue) : queue_(queue) {}
  TimeTicks DoWork() override {
    queue_->RunPendingTasks();
    std::optional<TimeTicks> next = queue_->NextDelayedRunTime();
    return next ? *next : TimeTicks();
  }

 private:
  const raw_ptr<TaskQueue> queue_;
};

Thread::Thread(std::string name)
    : name_(std::move(name)), queue_(TaskQueue::Create()) {}

Thread::~Thread() {
  Stop();
}

bool Thread::StartWithOptions(const Options& options) {
  if (IsRunning()) {
    return true;
  }
  pump_ = MessagePump::Create(options.message_pump_type);
  delegate_ = std::make_unique<Delegate>(queue_.get());
  MessagePump* pump = pump_.get();
  queue_->SetWakeupCallback([pump] { pump->ScheduleWork(); });
  MessagePumpForIO* io_pump = options.message_pump_type == MessagePumpType::IO
                                  ? static_cast<MessagePumpForIO*>(pump)
                                  : nullptr;
  handle_ = PlatformThread::Create(
      name_, [pump, io_pump, delegate = delegate_.get(), queue = queue_.get()] {
        TaskQueue::ScopedBindToCurrentThread bind(queue);
        CurrentIOThread current(io_pump);
        g_current_io_thread = io_pump ? &current : nullptr;
        pump->Run(delegate);
        g_current_io_thread = nullptr;
      });
  return IsRunning();
}

void Thread::Stop() {
  if (!IsRunning()) {
    return;
  }
  // Quit from inside the loop so already-posted tasks run first.
  queue_->PostTask([pump = pump_.get()] { pump->Quit(); });
  PlatformThread::Join(handle_);
  queue_->Shutdown();
  queue_->SetWakeupCallback({});
  delegate_.reset();
  pump_.reset();
}

// static
bool CurrentIOThread::IsSet() {
  return g_current_io_thread != nullptr;
}

// static
CurrentIOThread* CurrentIOThread::Get() {
  return g_current_io_thread;
}

bool CurrentIOThread::WatchFileDescriptor(
    int fd,
    bool persistent,
    int mode,
    MessagePumpForIO::FdWatchController* controller,
    MessagePumpForIO::FdWatcher* watcher) {
  return pump_->WatchFileDescriptor(fd, persistent, mode, controller, watcher);
}

}  // namespace base
