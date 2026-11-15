// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/unix_socket_client.h"

#include <sys/socket.h>  // IWYU pragma: keep (SOL_SOCKET, SO_ERROR)
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/callback.h"
#include "base/message_loop/message_pump_epoll.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "net/framed_connection.h"

namespace net {

UnixSocketClient::UnixSocketClient(std::string path,
                                   Callbacks callbacks,
                                   base::TimeDelta retry_delay,
                                   size_t max_output_bytes)
    : path_(std::move(path)),
      callbacks_(std::move(callbacks)),
      retry_delay_(retry_delay),
      max_output_bytes_(max_output_bytes),
      io_task_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {}

UnixSocketClient::~UnixSocketClient() {
  Stop();
}

void UnixSocketClient::Start() {
  if (running_) {
    return;
  }
  running_ = true;
  Connect();
}

void UnixSocketClient::Stop() {
  running_ = false;
  weak_factory_.InvalidateWeakPtrs();  // Cancels a pending retry.
  connect_controller_.StopWatchingFileDescriptor();
  if (connecting_fd_ >= 0) {
    close(connecting_fd_);
    connecting_fd_ = -1;
  }
  connection_.reset();
}

bool UnixSocketClient::SendFrame(std::string_view payload) {
  return connection_ && connection_->SendFrame(payload);
}

void UnixSocketClient::Connect() {
  if (!running_) {
    return;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path_.size() >= sizeof(addr.sun_path)) {
    return;
  }
  std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    ScheduleRetry();
    return;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    OnConnected(fd);
    return;
  }
  if (errno == EINPROGRESS || errno == EAGAIN) {
    // Wait for the socket to become writable, then check SO_ERROR.
    connecting_fd_ = fd;
    if (base::CurrentIOThread::Get()->WatchFileDescriptor(
            fd, /*persistent=*/false, base::MessagePumpForIO::WATCH_WRITE,
            &connect_controller_, this)) {
      return;
    }
    connecting_fd_ = -1;
  }
  close(fd);  // ENOENT / ECONNREFUSED: no server yet.
  ScheduleRetry();
}

void UnixSocketClient::OnFileCanWriteWithoutBlocking(int fd) {
  connecting_fd_ = -1;
  int error = 0;
  socklen_t len = sizeof(error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
    OnConnected(fd);
    return;
  }
  close(fd);
  ScheduleRetry();
}

void UnixSocketClient::OnConnected(int fd) {
  connection_ = std::make_unique<FramedConnection>(
      fd,
      FramedConnection::Callbacks{
          callbacks_.on_frame,
          base::BindRepeating(&UnixSocketClient::OnConnectionClosed,
                              weak_factory_.GetWeakPtr())},
      max_output_bytes_);
  if (callbacks_.on_connected) {
    callbacks_.on_connected.Run();
  }
}

void UnixSocketClient::OnConnectionClosed() {
  // We're inside the connection's callback: destroy it after it returns.
  io_task_runner_->PostTask(
      [connection = connection_.release()] { delete connection; });
  if (callbacks_.on_disconnected) {
    callbacks_.on_disconnected.Run();
  }
  ScheduleRetry();
}

void UnixSocketClient::ScheduleRetry() {
  if (running_) {
    io_task_runner_->PostDelayedTask(
        base::BindOnce(&UnixSocketClient::Connect, weak_factory_.GetWeakPtr()),
        retry_delay_);
  }
}

}  // namespace net
