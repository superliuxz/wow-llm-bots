// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/framed_connection.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/message_loop/message_pump.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "net/frame_codec.h"

namespace net {
namespace {

// A connection on an IO thread whose peer is a socket the test reads itself,
// at its own pace. The peer's receive buffer and our send buffer are tiny, so
// the connection backs up after a few frames.
class CongestedConnection {
 public:
  explicit CongestedConnection(size_t max_output_bytes)
      : io_("framed-connection-test") {
    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);
    int small = 4096;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    ::setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    peer_fd_ = fds[1];
    io_.StartWithOptions({base::MessagePumpType::IO});
    RunOnIo([&] {
      connection_ = std::make_unique<FramedConnection>(
          fds[0],
          FramedConnection::Callbacks{
              {},
              {},
              base::BindRepeating([](std::atomic<int>* n) { ++*n; },
                                  &drained_)},
          max_output_bytes);
    });
  }
  ~CongestedConnection() {
    RunOnIo([&] { connection_.reset(); });
    io_.Stop();
    ::close(peer_fd_);
  }

  bool Send(const std::string& payload) {
    bool sent = false;
    RunOnIo([&] { sent = connection_->SendFrame(payload); });
    return sent;
  }
  size_t Pending() {
    size_t n = 0;
    RunOnIo([&] { n = connection_->pending_output_bytes(); });
    return n;
  }
  size_t Buffered() {
    size_t n = 0;
    RunOnIo([&] { n = connection_->buffered_output_bytes(); });
    return n;
  }
  // The peer reads up to `max_bytes`, returning the frames now complete.
  std::vector<std::string> PeerRead(size_t max_bytes) {
    char buf[8192];
    const ssize_t n =
        ::recv(peer_fd_, buf, std::min(max_bytes, sizeof(buf)), MSG_DONTWAIT);
    if (n <= 0) {
      return {};
    }
    read_bytes_ += static_cast<size_t>(n);
    return decoder_.Append(std::string_view(buf, static_cast<size_t>(n)));
  }
  int drained() const { return drained_; }
  size_t read_bytes() const { return read_bytes_; }

 private:
  template <typename Fn>
  void RunOnIo(Fn fn) {
    base::WaitableEvent done;
    io_.task_runner()->PostTask([&] {
      fn();
      done.Signal();
    });
    done.Wait();
  }

  base::Thread io_;
  int peer_fd_ = -1;
  std::unique_ptr<FramedConnection> connection_;  // On the IO thread.
  FrameDecoder decoder_;
  std::atomic<int> drained_{0};
  size_t read_bytes_ = 0;
};

TEST(FramedConnectionTest, ABufferThatNeverFullyDrainsStaysBounded) {
  constexpr size_t kMaxOutput = 256 * 1024;
  CongestedConnection c(kMaxOutput);
  const std::string payload(1000, 'x');
  size_t worst = 0;
  // The peer reads about half of what is sent: the backlog never empties.
  for (int i = 0; i < 3000; ++i) {
    c.Send(payload);
    c.PeerRead(500);
    worst = std::max(worst, c.Buffered());
  }
  // The peer really did read much more than the buffer's size, so a buffer that
  // kept what it had sent would be far over the bound below. The sent part is
  // released once it is as big as the rest: at most twice the pending bytes.
  EXPECT_GT(c.read_bytes(), 3 * kMaxOutput);
  EXPECT_LE(worst, 2 * (kMaxOutput + payload.size() + 4));
}

TEST(FramedConnectionTest, FramesArriveIntactAndInOrderThroughTheCompaction) {
  CongestedConnection c(1 << 20);
  std::vector<std::string> received;
  std::vector<std::string> sent;
  for (int i = 0; i < 400; ++i) {
    // Different sizes, so frame boundaries fall everywhere.
    sent.push_back(std::to_string(i) + ":" +
                   std::string(50 + (i * 37) % 900, 'a' + i % 26));
    ASSERT_TRUE(c.Send(sent.back()));
    for (std::string& frame : c.PeerRead(700)) {
      received.push_back(std::move(frame));
    }
  }
  while (received.size() < sent.size()) {
    const std::vector<std::string> more = c.PeerRead(8192);
    if (more.empty()) {
      base::PlatformThread::Sleep(base::Milliseconds(1));
    }
    received.insert(received.end(), more.begin(), more.end());
  }
  EXPECT_EQ(received, sent);
}

TEST(FramedConnectionTest, DrainedIsReportedWhenABackedUpBufferEmpties) {
  CongestedConnection c(8 << 20);
  EXPECT_TRUE(c.Send("small"));  // Goes out at once: not backed up.
  EXPECT_EQ(c.Pending(), 0u);
  EXPECT_EQ(c.drained(), 0);
  // Back it up: the peer reads nothing meanwhile.
  while (c.Pending() == 0) {
    ASSERT_TRUE(c.Send(std::string(2000, 'y')));
  }
  EXPECT_EQ(c.drained(), 0);
  // The peer catches up.
  const base::TimeTicks give_up = base::TimeTicks::Now() + base::Seconds(5);
  while (c.drained() == 0 && base::TimeTicks::Now() < give_up) {
    c.PeerRead(8192);
  }
  EXPECT_EQ(c.drained(), 1);
  EXPECT_EQ(c.Pending(), 0u);
}

TEST(FramedConnectionTest, TheBufferIsBoundedByTheCapWhenThePeerReadsNothing) {
  CongestedConnection c(10000);
  int accepted = 0;
  for (int i = 0; i < 100; ++i) {
    accepted += c.Send(std::string(1000, 'z'));
  }
  EXPECT_LT(accepted, 100);  // Some were refused.
  EXPECT_LE(c.Pending(), 10000u + 1004u);
}

}  // namespace
}  // namespace net
