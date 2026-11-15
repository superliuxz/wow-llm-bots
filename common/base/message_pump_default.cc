// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "base/message_pump.h"
#include "base/message_pump_epoll.h"
#include "base/time.h"

namespace base {
namespace {

class MessagePumpDefault : public MessagePump {
 public:
  void Run(Delegate* delegate) override {
    quit_ = false;
    while (!quit_) {
      const TimeTicks next = delegate->DoWork();
      if (quit_) {
        break;
      }
      std::unique_lock<std::mutex> lock(mutex_);
      auto woken = [this] { return work_scheduled_; };
      if (next.is_null()) {
        cv_.wait(lock, woken);
      } else {
        const TimeDelta wait = next - TimeTicks::Now();
        if (wait.is_positive()) {
          cv_.wait_for(lock, std::chrono::microseconds(wait.InMicroseconds()),
                       woken);
        }
      }
      work_scheduled_ = false;
    }
  }

  void Quit() override { quit_ = true; }

  void ScheduleWork() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      work_scheduled_ = true;
    }
    cv_.notify_one();
  }

 private:
  bool quit_ = false;  // Loop thread only.
  std::mutex mutex_;
  std::condition_variable cv_;
  bool work_scheduled_ = false;
};

}  // namespace

// static
std::unique_ptr<MessagePump> MessagePump::Create(MessagePumpType type) {
  if (type == MessagePumpType::IO) {
    return std::make_unique<MessagePumpForIO>();
  }
  return std::make_unique<MessagePumpDefault>();
}

}  // namespace base
