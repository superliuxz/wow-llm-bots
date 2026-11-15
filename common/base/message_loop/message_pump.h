// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_MESSAGE_LOOP_MESSAGE_PUMP_H_
#define COMMON_BASE_MESSAGE_LOOP_MESSAGE_PUMP_H_

#include <memory>

#include "base/time/time.h"

namespace base {

enum class MessagePumpType {
  DEFAULT,  // Waits on a condition variable between tasks.
  IO,       // epoll loop; file descriptors can be watched on this thread.
};

// The loop a base::Thread runs, like Chromium's base::MessagePump. Run() calls
// back into the delegate to do work and sleeps until ScheduleWork() or the
// next delayed run time.
class MessagePump {
 public:
  class Delegate {
   public:
    virtual ~Delegate() = default;
    // Runs ready tasks. Returns the next delayed run time (null if none).
    virtual TimeTicks DoWork() = 0;
  };

  static std::unique_ptr<MessagePump> Create(MessagePumpType type);

  virtual ~MessagePump() = default;
  // Runs until Quit() is called from inside the loop.
  virtual void Run(Delegate* delegate) = 0;
  // Called from inside the loop (from a task).
  virtual void Quit() = 0;
  // Thread-safe: wakes the loop so it calls DoWork() soon.
  virtual void ScheduleWork() = 0;
};

}  // namespace base

#endif  // COMMON_BASE_MESSAGE_LOOP_MESSAGE_PUMP_H_
