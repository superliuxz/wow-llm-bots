// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_NET_UNIX_SOCKET_CLIENT_H_
#define COMMON_NET_UNIX_SOCKET_CLIENT_H_

#include <cstddef>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump_epoll.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "net/line_connection.h"

namespace net {

// Line-oriented Unix socket client that keeps reconnecting (every
// `retry_delay`) until Stop() or destruction. Create, use and destroy on one
// IO thread (a base::Thread with MessagePumpType::IO).
class UnixSocketClient : public base::MessagePumpForIO::FdWatcher {
 public:
  struct Callbacks {
    base::RepeatingClosure on_connected;
    base::RepeatingCallback<void(std::string)> on_line;
    base::RepeatingClosure on_disconnected;
  };

  UnixSocketClient(std::string path,
                   Callbacks callbacks,
                   base::TimeDelta retry_delay = base::Seconds(1),
                   size_t max_output_bytes = 4 << 20);
  UnixSocketClient(const UnixSocketClient&) = delete;
  UnixSocketClient& operator=(const UnixSocketClient&) = delete;
  ~UnixSocketClient() override;

  void Start();
  void Stop();
  // Returns false (line dropped) when not connected or backed up.
  bool SendLine(const std::string& line);
  bool connected() const { return connection_ != nullptr; }

 private:
  // base::MessagePumpForIO::FdWatcher (a connect() in progress):
  void OnFileCanReadWithoutBlocking(int /*fd*/) override {}
  void OnFileCanWriteWithoutBlocking(int fd) override;

  void Connect();
  void OnConnected(int fd);
  void OnConnectionClosed();
  void ScheduleRetry();

  const std::string path_;
  Callbacks callbacks_;
  const base::TimeDelta retry_delay_;
  const size_t max_output_bytes_;
  base::scoped_refptr<base::SequencedTaskRunner> io_task_runner_;
  bool running_ = false;
  int connecting_fd_ = -1;
  base::MessagePumpForIO::FdWatchController connect_controller_;
  std::unique_ptr<LineConnection> connection_;
  base::WeakPtrFactory<UnixSocketClient> weak_factory_{this};
};

}  // namespace net

#endif  // COMMON_NET_UNIX_SOCKET_CLIENT_H_
