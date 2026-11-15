// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_NET_UNIX_SOCKET_SERVER_H_
#define COMMON_NET_UNIX_SOCKET_SERVER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_epoll.h"
#include "net/line_connection.h"

namespace net {

// Line-oriented Unix socket server with at most one client: a new connection
// replaces the old one. Create, use and destroy on one IO thread (a
// base::Thread with MessagePumpType::IO).
class UnixSocketServer : public base::MessagePumpForIO::FdWatcher {
 public:
  struct Callbacks {
    base::RepeatingClosure on_connected;
    base::RepeatingCallback<void(std::string)> on_line;
    base::RepeatingClosure on_disconnected;
  };
  struct Stats {
    uint64_t lines_sent = 0;
    uint64_t lines_received = 0;
    uint64_t lines_dropped = 0;  // Not connected, or output backed up.
    uint64_t connections = 0;
  };

  UnixSocketServer(std::string path,
                   Callbacks callbacks,
                   size_t max_output_bytes = 4 << 20);
  UnixSocketServer(const UnixSocketServer&) = delete;
  UnixSocketServer& operator=(const UnixSocketServer&) = delete;
  ~UnixSocketServer() override;  // Closes everything, removes the socket file.

  // Removes a stale socket file, binds and listens. Returns false on failure.
  bool Listen();
  // Queues `line` for the client; dropped if none or backed up.
  void SendLine(const std::string& line);
  bool connected() const { return client_ != nullptr; }
  const Stats& stats() const { return stats_; }

 private:
  // base::MessagePumpForIO::FdWatcher (the listening socket):
  void OnFileCanReadWithoutBlocking(int fd) override;
  void OnFileCanWriteWithoutBlocking(int /*fd*/) override {}

  void OnClientLine(std::string line);
  void OnClientClosed();

  const std::string path_;
  Callbacks callbacks_;
  const size_t max_output_bytes_;
  int listen_fd_ = -1;
  base::MessagePumpForIO::FdWatchController listen_controller_;
  std::unique_ptr<LineConnection> client_;
  Stats stats_;
  base::WeakPtrFactory<UnixSocketServer> weak_factory_{this};
};

}  // namespace net

#endif  // COMMON_NET_UNIX_SOCKET_SERVER_H_
