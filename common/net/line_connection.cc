// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/line_connection.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "base/message_pump_epoll.h"
#include "base/thread.h"

namespace net {

LineConnection::LineConnection(int fd,
                               Callbacks callbacks,
                               size_t max_output_bytes)
    : fd_(fd),
      callbacks_(std::move(callbacks)),
      max_output_bytes_(max_output_bytes) {
  base::CurrentIOThread::Get()->WatchFileDescriptor(
      fd_, /*persistent=*/true, base::MessagePumpForIO::WATCH_READ,
      &controller_, this);
}

LineConnection::~LineConnection() {
  controller_.StopWatchingFileDescriptor();
  if (fd_ >= 0) {
    close(fd_);
  }
}

bool LineConnection::SendLine(const std::string& line) {
  if (fd_ < 0 || output_.size() - output_offset_ > max_output_bytes_) {
    return false;
  }
  output_.append(line);
  output_.push_back('\n');
  if (!watching_write_) {
    Flush();
  }
  return true;
}

void LineConnection::OnFileCanReadWithoutBlocking(int /*fd*/) {
  char buf[16384];
  while (fd_ >= 0) {
    const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
    if (n > 0) {
      for (std::string& line :
           framer_.Append(std::string_view(buf, static_cast<size_t>(n)))) {
        if (callbacks_.on_line) {
          callbacks_.on_line.Run(std::move(line));
        }
        if (fd_ < 0) {
          return;  // Closed from inside on_line.
        }
      }
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    Close();  // n == 0 (peer closed) or a real error.
    return;
  }
}

void LineConnection::OnFileCanWriteWithoutBlocking(int /*fd*/) {
  Flush();
}

void LineConnection::Flush() {
  while (fd_ >= 0 && output_offset_ < output_.size()) {
    const ssize_t n = send(fd_, output_.data() + output_offset_,
                           output_.size() - output_offset_, MSG_NOSIGNAL);
    if (n > 0) {
      output_offset_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (!watching_write_) {
        watching_write_ = base::CurrentIOThread::Get()->WatchFileDescriptor(
            fd_, /*persistent=*/true, base::MessagePumpForIO::WATCH_WRITE,
            &controller_, this);
      }
      return;
    }
    Close();
    return;
  }
  output_.clear();
  output_offset_ = 0;
  if (watching_write_) {
    // Narrow the watch back to read-only.
    controller_.StopWatchingFileDescriptor();
    base::CurrentIOThread::Get()->WatchFileDescriptor(
        fd_, /*persistent=*/true, base::MessagePumpForIO::WATCH_READ,
        &controller_, this);
    watching_write_ = false;
  }
}

void LineConnection::Close() {
  if (fd_ < 0) {
    return;
  }
  controller_.StopWatchingFileDescriptor();
  close(fd_);
  fd_ = -1;
  output_.clear();
  output_offset_ = 0;
  if (callbacks_.on_closed) {
    callbacks_.on_closed.Run();  // May destroy `this`; touch nothing after.
  }
}

}  // namespace net
