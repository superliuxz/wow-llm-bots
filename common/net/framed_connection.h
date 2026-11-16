// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_NET_FRAMED_CONNECTION_H_
#define COMMON_NET_FRAMED_CONNECTION_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/message_loop/message_pump_epoll.h"
#include "net/frame_codec.h"

namespace net {

// One connected, non-blocking stream socket carrying length-prefixed frames
// (net/frame_codec.h). Writes are buffered and flushed when the socket is
// writable. A corrupt frame header closes the connection. Create, use and
// destroy on one IO thread.
class FramedConnection : public base::MessagePumpForIO::FdWatcher {
 public:
  struct Callbacks {
    base::RepeatingCallback<void(std::string)> on_frame;
    // Peer closed, error or corrupt frame; fd closed.
    base::RepeatingClosure on_closed;
    // The output buffer was backed up (the socket was full) and has just
    // emptied: the peer has caught up. Not called when everything went out at
    // once. SendFrame() may be called from here.
    base::RepeatingClosure on_drained;
  };

  // Takes ownership of `fd` (must be non-blocking) and starts reading.
  FramedConnection(int fd, Callbacks callbacks, size_t max_output_bytes);
  FramedConnection(const FramedConnection&) = delete;
  FramedConnection& operator=(const FramedConnection&) = delete;
  ~FramedConnection() override;  // Closes the fd without calling on_closed.

  // Queues `payload` as one frame. Returns false (dropped) if the output buffer
  // is over `max_output_bytes`, the payload is over kMaxFramePayloadBytes, or
  // the connection is closed.
  bool SendFrame(std::string_view payload);
  bool is_open() const { return fd_ >= 0; }

  // Bytes queued and not yet accepted by the socket. Zero: the next frame goes
  // out at once.
  size_t pending_output_bytes() const {
    return output_.size() - output_offset_;
  }
  // Bytes the output buffer holds, including a sent part not yet released (it
  // is released once it is as big as the rest, so never more than twice the
  // pending bytes, however much has been sent).
  size_t buffered_output_bytes() const { return output_.size(); }

 private:
  // base::MessagePumpForIO::FdWatcher:
  void OnFileCanReadWithoutBlocking(int fd) override;
  void OnFileCanWriteWithoutBlocking(int fd) override;

  void Flush();
  void Close();  // Closes the fd and runs on_closed.

  int fd_;
  Callbacks callbacks_;
  const size_t max_output_bytes_;
  FrameDecoder decoder_;
  std::string output_;
  size_t output_offset_ = 0;
  bool watching_write_ = false;
  base::MessagePumpForIO::FdWatchController controller_;
};

}  // namespace net

#endif  // COMMON_NET_FRAMED_CONNECTION_H_
