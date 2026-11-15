// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <memory>

#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_epoll.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"

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
      AutoLock lock(lock_);
      if (next.is_null()) {
        while (!work_scheduled_) {
          cv_.Wait();
        }
      } else {
        while (!work_scheduled_) {
          const TimeDelta wait = next - TimeTicks::Now();
          if (!wait.is_positive()) {
            break;
          }
          cv_.TimedWait(wait);
        }
      }
      work_scheduled_ = false;
    }
  }

  void Quit() override { quit_ = true; }

  void ScheduleWork() override {
    AutoLock lock(lock_);
    work_scheduled_ = true;
    cv_.Signal();
  }

 private:
  bool quit_ = false;  // Loop thread only.
  Lock lock_;
  ConditionVariable cv_{&lock_};
  bool work_scheduled_ GUARDED_BY(lock_) = false;
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
