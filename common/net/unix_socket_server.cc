// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/unix_socket_server.h"

#include <sys/socket.h>
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
#include "net/framed_connection.h"

namespace net {

UnixSocketServer::UnixSocketServer(std::string path,
                                   Callbacks callbacks,
                                   size_t max_output_bytes)
    : path_(std::move(path)),
      callbacks_(std::move(callbacks)),
      max_output_bytes_(max_output_bytes) {}

UnixSocketServer::~UnixSocketServer() {
  client_.reset();
  listen_controller_.StopWatchingFileDescriptor();
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    unlink(path_.c_str());
  }
}

bool UnixSocketServer::Listen() {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path_.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
  listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    return false;
  }
  unlink(path_.c_str());  // A stale file from a crash would block bind().
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(listen_fd_, SOMAXCONN) != 0 ||
      !base::CurrentIOThread::Get()->WatchFileDescriptor(
          listen_fd_, /*persistent=*/true, base::MessagePumpForIO::WATCH_READ,
          &listen_controller_, this)) {
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  return true;
}

void UnixSocketServer::SendFrame(std::string_view payload) {
  if (!client_ || !client_->SendFrame(payload)) {
    ++stats_.frames_dropped;
    return;
  }
  ++stats_.frames_sent;
}

void UnixSocketServer::OnFileCanReadWithoutBlocking(int /*fd*/) {
  while (true) {
    const int fd =
        accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;  // EAGAIN: no more pending connections (or a transient error).
    }
    const bool had_client = client_ != nullptr;
    client_.reset();  // A new harness replaces the old one.
    if (had_client && callbacks_.on_disconnected) {
      callbacks_.on_disconnected.Run();
    }
    client_ = std::make_unique<FramedConnection>(
        fd,
        FramedConnection::Callbacks{
            base::BindRepeating(&UnixSocketServer::OnClientFrame,
                                weak_factory_.GetWeakPtr()),
            base::BindRepeating(&UnixSocketServer::OnClientClosed,
                                weak_factory_.GetWeakPtr()),
            base::BindRepeating(&UnixSocketServer::OnClientDrained,
                                weak_factory_.GetWeakPtr())},
        max_output_bytes_);
    ++stats_.connections;
    if (callbacks_.on_connected) {
      callbacks_.on_connected.Run();
    }
  }
}

void UnixSocketServer::OnClientFrame(std::string frame) {
  ++stats_.frames_received;
  if (callbacks_.on_frame) {
    callbacks_.on_frame.Run(std::move(frame));
  }
}

void UnixSocketServer::OnClientDrained() {
  if (callbacks_.on_drained) {
    callbacks_.on_drained.Run();
  }
}

void UnixSocketServer::OnClientClosed() {
  // We're inside the connection's callback: destroy it after it returns.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      [connection = client_.release()] { delete connection; });
  if (callbacks_.on_disconnected) {
    callbacks_.on_disconnected.Run();
  }
}

}  // namespace net
