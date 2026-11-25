// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/unix_socket_server.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_epoll.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "net/framed_connection.h"

namespace net {

UnixSocketServer::UnixSocketServer(std::string path,
                                   Callbacks callbacks,
                                   size_t max_output_bytes,
                                   base::TimeDelta hung_after)
    : path_(std::move(path)),
      callbacks_(std::move(callbacks)),
      max_output_bytes_(max_output_bytes),
      hung_after_(hung_after) {}

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
      chmod(path_.c_str(), 0600) != 0 || listen(listen_fd_, SOMAXCONN) != 0 ||
      !base::CurrentIOThread::Get()->WatchFileDescriptor(
          listen_fd_, /*persistent=*/true, base::MessagePumpForIO::WATCH_READ,
          &listen_controller_, this)) {
    close(listen_fd_);
    unlink(path_.c_str());
    listen_fd_ = -1;
    return false;
  }
  return true;
}

bool UnixSocketServer::SendFrame(std::string_view payload) {
  if (!client_ || !client_->SendFrame(payload)) {
    ++stats_.frames_dropped;
    return false;
  }
  ++stats_.frames_sent;
  if (client_->pending_output_bytes() == 0) {
    client_alive_at_ = base::TimeTicks::Now();
  }
  return true;
}

bool UnixSocketServer::ClientHung() const {
  return client_ && client_->pending_output_bytes() > 0 &&
         base::TimeTicks::Now() - client_alive_at_ >= hung_after_;
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
    // Reject connections from other local users even during the narrow window
    // between bind() and chmod(0600), and reject any second connection while a
    // live harness is already attached.
    ucred cred{};
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        cred_len != sizeof(cred) || cred.uid != geteuid() ||
        (client_ != nullptr && !ClientHung())) {
      close(fd);
      ++stats_.connections_rejected;
      continue;
    }
    if (client_) {
      // The live client is hung: it makes way for the new one.
      client_.reset();
      ++stats_.connections_replaced;
      if (callbacks_.on_disconnected) {
        base::WeakPtr<UnixSocketServer> self = weak_factory_.GetWeakPtr();
        callbacks_.on_disconnected.Run();
        if (!self) {
          close(fd);
          return;
        }
      }
    }
    client_alive_at_ = base::TimeTicks::Now();
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
  client_alive_at_ = base::TimeTicks::Now();
  if (callbacks_.on_frame) {
    callbacks_.on_frame.Run(std::move(frame));
  }
}

void UnixSocketServer::OnClientDrained() {
  client_alive_at_ = base::TimeTicks::Now();
  if (callbacks_.on_drained) {
    callbacks_.on_drained.Run();
  }
}

void UnixSocketServer::OnClientClosed() {
  // We're inside the connection's callback: destroy it after it returns.
  // Transfer ownership via unique_ptr so TaskQueue::Shutdown() still frees it.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      [connection = std::move(client_)]() mutable { connection.reset(); });
  if (callbacks_.on_disconnected) {
    callbacks_.on_disconnected.Run();
  }
}

}  // namespace net
