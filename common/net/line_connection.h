// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_NET_LINE_CONNECTION_H_
#define COMMON_NET_LINE_CONNECTION_H_

#include <cstddef>
#include <string>

#include "base/callback.h"
#include "base/message_pump_epoll.h"
#include "net/line_framer.h"

namespace net {

// One connected, non-blocking stream socket carrying '\n'-terminated lines.
// Reads are framed into lines; writes are buffered and flushed when the socket
// is writable. Create, use and destroy on one IO thread.
class LineConnection : public base::MessagePumpForIO::FdWatcher {
 public:
  struct Callbacks {
    base::RepeatingCallback<void(std::string)> on_line;
    base::RepeatingClosure on_closed;  // Peer closed or error; fd closed.
  };

  // Takes ownership of `fd` (must be non-blocking) and starts reading.
  LineConnection(int fd, Callbacks callbacks, size_t max_output_bytes);
  LineConnection(const LineConnection&) = delete;
  LineConnection& operator=(const LineConnection&) = delete;
  ~LineConnection() override;  // Closes the fd without calling on_closed.

  // Queues `line` + '\n'. Returns false (dropped) if the output buffer is over
  // `max_output_bytes` or the connection is closed.
  bool SendLine(const std::string& line);
  bool is_open() const { return fd_ >= 0; }

 private:
  // base::MessagePumpForIO::FdWatcher:
  void OnFileCanReadWithoutBlocking(int fd) override;
  void OnFileCanWriteWithoutBlocking(int fd) override;

  void Flush();
  void Close();  // Closes the fd and runs on_closed.

  int fd_;
  Callbacks callbacks_;
  const size_t max_output_bytes_;
  LineFramer framer_;
  std::string output_;
  size_t output_offset_ = 0;
  bool watching_write_ = false;
  base::MessagePumpForIO::FdWatchController controller_;
};

}  // namespace net

#endif  // COMMON_NET_LINE_CONNECTION_H_
