// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/message_loop/message_pump_epoll.h"

#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>

#include "base/compiler_specific.h"
#include "base/time/time.h"

namespace base {
namespace {

uint32_t ToEpollEvents(int mode) {
  uint32_t events = 0;
  if (mode & MessagePumpEpoll::WATCH_READ) {
    events |= EPOLLIN;
  }
  if (mode & MessagePumpEpoll::WATCH_WRITE) {
    events |= EPOLLOUT;
  }
  return events;
}

}  // namespace

MessagePumpEpoll::FdWatchController::~FdWatchController() {
  StopWatchingFileDescriptor();
}

bool MessagePumpEpoll::FdWatchController::StopWatchingFileDescriptor() {
  return pump_ ? pump_->StopWatching(this) : false;
}

MessagePumpEpoll::MessagePumpEpoll()
    : epoll_fd_(epoll_create1(EPOLL_CLOEXEC)),
      wake_fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = wake_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
}

MessagePumpEpoll::~MessagePumpEpoll() {
  for (auto& [fd, controller] : controllers_) {
    controller->pump_ = nullptr;
  }
  close(wake_fd_);
  close(epoll_fd_);
}

void MessagePumpEpoll::Run(Delegate* delegate) {
  quit_ = false;
  epoll_event events[64];
  while (true) {
    const TimeTicks next = delegate->DoWork();
    if (quit_) {
      break;
    }
    timespec timeout{};
    timespec* timeout_ptr = nullptr;  // Wait forever.
    if (!next.is_null()) {
      TimeDelta wait = next - TimeTicks::Now();
      if (wait.is_negative()) {
        wait = TimeDelta();
      }
      const int64_t us = wait.InMicroseconds();
      timeout = {static_cast<time_t>(us / 1000000),
                 static_cast<long>((us % 1000000) * 1000)};
      timeout_ptr = &timeout;
    }
    const int n = epoll_pwait2(epoll_fd_, events, 64, timeout_ptr, nullptr);
    if (n > 0) {
      // MSan has no epoll_pwait2 interceptor; the kernel filled `events`.
      MSAN_UNPOISON(events, sizeof(events[0]) * static_cast<size_t>(n));
    }
    for (int i = 0; i < n; ++i) {
      if (events[i].data.fd == wake_fd_) {
        uint64_t count;
        [[maybe_unused]] ssize_t r = read(wake_fd_, &count, sizeof(count));
        continue;
      }
      Dispatch(events[i].data.fd, events[i].events);
    }
  }
}

void MessagePumpEpoll::Quit() {
  quit_ = true;
}

void MessagePumpEpoll::ScheduleWork() {
  const uint64_t one = 1;
  [[maybe_unused]] ssize_t n = write(wake_fd_, &one, sizeof(one));
}

bool MessagePumpEpoll::WatchFileDescriptor(int fd,
                                           bool persistent,
                                           int mode,
                                           FdWatchController* controller,
                                           FdWatcher* watcher) {
  auto it = controllers_.find(fd);
  if (it != controllers_.end() && it->second != controller) {
    return false;  // Another controller already watches this fd.
  }
  const int new_mode = (controller->pump_ ? controller->mode_ : 0) | mode;
  epoll_event ev{};
  ev.events = ToEpollEvents(new_mode);
  ev.data.fd = fd;
  const int op = it == controllers_.end() ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(epoll_fd_, op, fd, &ev) != 0) {
    return false;
  }
  controller->pump_ = this;
  controller->watcher_ = watcher;
  controller->fd_ = fd;
  controller->mode_ = new_mode;
  controller->persistent_ = persistent;
  controllers_[fd] = controller;
  return true;
}

bool MessagePumpEpoll::StopWatching(FdWatchController* controller) {
  auto it = controllers_.find(controller->fd_);
  if (it == controllers_.end() || it->second != controller) {
    return false;
  }
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, controller->fd_, nullptr);
  controllers_.erase(it);
  controller->pump_ = nullptr;
  controller->watcher_ = nullptr;
  controller->fd_ = -1;
  controller->mode_ = 0;
  return true;
}

void MessagePumpEpoll::Dispatch(int fd, uint32_t events) {
  // Errors and hangups are reported as readable/writable:
  // the next read/write returns the error or EOF.
  const bool error = events & (EPOLLERR | EPOLLHUP);
  for (int mode : {WATCH_WRITE, WATCH_READ}) {
    auto it = controllers_.find(fd);
    if (it == controllers_.end()) {
      return;  // Stopped by an earlier callback.
    }
    FdWatchController* controller = it->second.get();
    const bool ready =
        (controller->mode_ & mode) &&
        (error || (events & (mode == WATCH_READ ? EPOLLIN : EPOLLOUT)));
    if (!ready) {
      continue;
    }
    FdWatcher* watcher = controller->watcher_.get();
    if (!controller->persistent_) {
      StopWatching(controller);
    }
    if (mode == WATCH_READ) {
      watcher->OnFileCanReadWithoutBlocking(fd);
    } else {
      watcher->OnFileCanWriteWithoutBlocking(fd);
    }
  }
}

}  // namespace base
