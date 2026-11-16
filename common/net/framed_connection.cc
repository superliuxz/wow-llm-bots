// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/framed_connection.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "base/message_loop/message_pump_epoll.h"
#include "base/threading/thread.h"
#include "net/frame_codec.h"

namespace net {
namespace {

// The sent part of the output buffer is released once it is at least this big
// (and at least half the buffer).
constexpr size_t kCompactBytes = 64 * 1024;

}  // namespace

FramedConnection::FramedConnection(int fd,
                                   Callbacks callbacks,
                                   size_t max_output_bytes)
    : fd_(fd),
      callbacks_(std::move(callbacks)),
      max_output_bytes_(max_output_bytes) {
  base::CurrentIOThread::Get()->WatchFileDescriptor(
      fd_, /*persistent=*/true, base::MessagePumpForIO::WATCH_READ,
      &controller_, this);
}

FramedConnection::~FramedConnection() {
  controller_.StopWatchingFileDescriptor();
  if (fd_ >= 0) {
    close(fd_);
  }
}

bool FramedConnection::SendFrame(std::string_view payload) {
  if (fd_ < 0 || output_.size() - output_offset_ > max_output_bytes_) {
    return false;
  }
  // Release what was already sent. The buffer is only reset when it empties
  // completely, so a peer that is always a little behind would otherwise make
  // it keep every byte ever sent. Done when at least half of it is sent, so the
  // copy of the rest is paid for by the bytes released.
  if (output_offset_ >= kCompactBytes && output_offset_ >= output_.size() / 2) {
    output_.erase(0, output_offset_);
    output_offset_ = 0;
  }
  if (!AppendFrame(payload, &output_)) {
    return false;
  }
  if (!watching_write_) {
    Flush();
  }
  return true;
}

void FramedConnection::OnFileCanReadWithoutBlocking(int /*fd*/) {
  char buf[16384];
  while (fd_ >= 0) {
    const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
    if (n > 0) {
      for (std::string& frame :
           decoder_.Append(std::string_view(buf, static_cast<size_t>(n)))) {
        if (callbacks_.on_frame) {
          callbacks_.on_frame.Run(std::move(frame));
        }
        if (fd_ < 0) {
          return;  // Closed from inside on_frame.
        }
      }
      if (decoder_.failed()) {
        Close();  // Corrupt length: the stream can't be resynchronized.
        return;
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

void FramedConnection::OnFileCanWriteWithoutBlocking(int /*fd*/) {
  Flush();
}

void FramedConnection::Flush() {
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
    // The peer has caught up. The callback may send more.
    if (callbacks_.on_drained) {
      callbacks_.on_drained.Run();
    }
  }
}

void FramedConnection::Close() {
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
