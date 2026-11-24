// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Next to brain_bridge.cc. mod-llm-brain.cmake keeps
// *_unittest.cc out of the server build; common/CMakeLists.txt builds it as a
// test.

#include "bridge/brain_bridge.h"

#include <poll.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/message_loop/message_pump.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/thread_annotations.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "net/frame_codec.h"
#include "net/unix_socket_client.h"

namespace llm_brain {
namespace {

constexpr base::TimeDelta kTimeout = base::Seconds(2);

std::string TestSocketPath() {
  return "/tmp/llm_brain_unittest_" + std::to_string(getpid()) + ".sock";
}

// Pumps the bridge's world-thread tasks (this test thread plays AzerothCore's
// world thread) until `done()` or timeout.
template <typename Done>
bool RunWorldUntil(BrainBridge& bridge, const Done& done) {
  const base::TimeTicks deadline = base::TimeTicks::Now() + kTimeout;
  while (!done()) {
    if (base::TimeTicks::Now() > deadline) {
      return false;
    }
    bridge.RunWorldTasks();
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }
  return true;
}

// A stand-in harness: a UnixSocketClient on its own IO thread.
class FakeHarness {
 public:
  explicit FakeHarness(const std::string& path) : io_("fake-harness") {
    io_.StartWithOptions({base::MessagePumpType::IO});
    RunOnIo([this, path] {
      client_ = std::make_unique<net::UnixSocketClient>(
          path,
          net::UnixSocketClient::Callbacks{[this] { connected_.Signal(); },
                                           [this](std::string frame) {
                                             base::AutoLock lock(lock_);
                                             frames_.push_back(
                                                 std::move(frame));
                                           },
                                           [this] { disconnected_.Signal(); }},
          base::Milliseconds(20));
      client_->Start();
    });
  }
  ~FakeHarness() {
    RunOnIo([this] { client_.reset(); });
  }

  bool WaitConnected() { return connected_.TimedWait(kTimeout); }
  bool WaitDisconnected() { return disconnected_.TimedWait(kTimeout); }
  void Send(const std::string& message) {
    RunOnIo([this, message] { EXPECT_TRUE(client_->SendFrame(message)); });
  }
  std::vector<std::string> frames() {
    base::AutoLock lock(lock_);
    return frames_;
  }

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
  std::unique_ptr<net::UnixSocketClient> client_;
  base::WaitableEvent connected_;
  base::WaitableEvent disconnected_;
  base::Lock lock_;
  std::vector<std::string> frames_ GUARDED_BY(lock_);
};

// Records world-thread callbacks (they all run on the test thread).
struct WorldRecorder {
  std::vector<std::string> messages;
  std::vector<bool> connection_changes;

  BrainBridge::Callbacks Callbacks() {
    return {[this](std::string m) { messages.push_back(std::move(m)); },
            [this](bool c) { connection_changes.push_back(c); }};
  }
};

TEST(BrainBridgeTest, ConnectionStateArrivesOnlyViaWorldTasks) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  FakeHarness harness(TestSocketPath());
  ASSERT_TRUE(harness.WaitConnected());
  // The IO thread knows, but nothing reached the world thread until it pumps.
  EXPECT_FALSE(bridge.connected());
  EXPECT_TRUE(world.connection_changes.empty());
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));
  EXPECT_EQ(world.connection_changes, std::vector<bool>{true});
}

TEST(BrainBridgeTest, MessagesFlowBothWays) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  FakeHarness harness(TestSocketPath());
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));

  bridge.Send("{\"type\":\"snapshot\"}");
  harness.Send("{\"type\":\"command\"}");
  ASSERT_TRUE(RunWorldUntil(bridge, [&] {
    return !world.messages.empty() && !harness.frames().empty();
  }));
  EXPECT_EQ(world.messages, std::vector<std::string>{"{\"type\":\"command\"}"});
  EXPECT_EQ(harness.frames(),
            std::vector<std::string>{"{\"type\":\"snapshot\"}"});
}

TEST(BrainBridgeTest, SendWithoutHarnessIsDropped) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  bridge.Send("nobody is listening");  // Must not crash or queue forever.
  bridge.RunWorldTasks();
  EXPECT_FALSE(bridge.connected());
}

TEST(BrainBridgeTest, SecondConnectionRejectedWhileLiveHarnessConnected) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  FakeHarness first(TestSocketPath());
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));
  // A stray second client must not kick out the live harness.
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, TestSocketPath().c_str(),
               sizeof(addr.sun_path) - 1);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  char buf[16];
  EXPECT_EQ(::recv(fd, buf, sizeof(buf), 0), 0);  // Refused and closed.
  ::close(fd);

  bridge.Send("still to the first harness");
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return !first.frames().empty(); }));
  EXPECT_EQ(first.frames(),
            std::vector<std::string>{"still to the first harness"});
}

TEST(BrainBridgeTest, FramesFromAHarnessThatLeftAreNotApplied) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  auto first = std::make_unique<FakeHarness>(TestSocketPath());
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));
  // The old harness's commands wait for the world thread...
  first->Send("old 1");
  first->Send("old 2");
  base::PlatformThread::Sleep(base::Milliseconds(100));
  // ...while it disconnects and a new one connects, before the next world tick.
  first.reset();
  base::PlatformThread::Sleep(base::Milliseconds(50));
  FakeHarness second(TestSocketPath());
  ASSERT_TRUE(second.WaitConnected());
  second.Send("new");
  ASSERT_TRUE(RunWorldUntil(bridge, [&] {
    return !world.messages.empty() && world.messages.back() == "new";
  }));
  EXPECT_EQ(world.messages, std::vector<std::string>{"new"});
}

TEST(BrainBridgeTest, DropsInboundWhenWorldFallsBehind) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), /*max_pending_messages=*/5,
                     world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  FakeHarness harness(TestSocketPath());
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));
  for (int i = 0; i < 50; ++i) {
    harness.Send("msg" + std::to_string(i));
  }
  base::PlatformThread::Sleep(base::Milliseconds(200));  // World not pumping.
  bridge.RunWorldTasks();
  EXPECT_GT(world.messages.size(), 0u);
  EXPECT_LE(world.messages.size(), 5u);
  // Every message that didn't reach the world is counted (RH-M6).
  const BrainBridge::SendStats stats = bridge.TakeSendStats();
  EXPECT_EQ(world.messages.size() + stats.dropped_inbound, 50u);
}

TEST(BrainBridgeTest, StopRemovesSocketAndCanRestart) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  EXPECT_EQ(access(TestSocketPath().c_str(), F_OK), 0);
  bridge.Stop();
  EXPECT_NE(access(TestSocketPath().c_str(), F_OK), 0);
  ASSERT_TRUE(bridge.Start());
  FakeHarness harness(TestSocketPath());
  EXPECT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));
}

TEST(BrainBridgeTest, StartFailsOnInvalidPath) {
  WorldRecorder world;
  BrainBridge bridge(std::string(200, 'x'), 100, world.Callbacks());
  EXPECT_FALSE(bridge.Start());
  EXPECT_FALSE(bridge.connected());
}

TEST(BrainBridgeTest, DestroyWithPendingWorkIsSafe) {
  WorldRecorder world;
  auto bridge =
      std::make_unique<BrainBridge>(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge->Start());
  FakeHarness harness(TestSocketPath());
  ASSERT_TRUE(RunWorldUntil(*bridge, [&] { return bridge->connected(); }));
  for (int i = 0; i < 20; ++i) {
    bridge->Send("pending " + std::to_string(i));
    harness.Send("inbound " + std::to_string(i));
  }
  bridge.reset();  // raw_ptr would abort here if IoCore were still referenced.
  EXPECT_TRUE(harness.WaitDisconnected());
}

// A harness that connects and reads only when told: a slow or stalled one. Its
// receive buffer is tiny, so what the bridge sends piles up on the bridge's
// side.
class RawClient {
 public:
  explicit RawClient(const std::string& path) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int small = 4096;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    EXPECT_EQ(
        ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
        0);
  }
  ~RawClient() { ::close(fd_); }

  // Reads until `count` frames have arrived or `timeout` passes.
  std::vector<std::string> ReadFrames(size_t count, base::TimeDelta timeout) {
    std::vector<std::string> frames;
    const base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
    char buffer[65536];
    while (frames.size() < count && base::TimeTicks::Now() < deadline) {
      pollfd poll_fd{fd_, POLLIN, 0};
      if (::poll(&poll_fd, 1, 50) <= 0) {
        continue;
      }
      const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
      if (n <= 0) {
        break;
      }
      for (std::string& frame :
           decoder_.Append(std::string_view(buffer, static_cast<size_t>(n)))) {
        frames.push_back(std::move(frame));
      }
    }
    return frames;
  }

 private:
  int fd_ = -1;
  net::FrameDecoder decoder_;
};

TEST(BrainBridgeTest, LatestMessagesGoOutAtOnceAndInOrderWhenThereIsRoom) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  FakeHarness harness(TestSocketPath());
  ASSERT_TRUE(harness.WaitConnected());
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));
  bridge.Send("event 1");
  bridge.SendLatest("tess", "snapshot 1");
  bridge.Send("event 2");
  bridge.SendLatest("tess", "snapshot 2");
  ASSERT_TRUE(
      RunWorldUntil(bridge, [&] { return harness.frames().size() >= 4; }));
  EXPECT_EQ(harness.frames(),
            (std::vector<std::string>{"event 1", "snapshot 1", "event 2",
                                      "snapshot 2"}));
  EXPECT_EQ(bridge.TakeSendStats().replaced, 0u);
}

TEST(BrainBridgeTest, ASlowHarnessGetsTheNewestSnapshotOfEachBotAndEveryEvent) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  RawClient harness(TestSocketPath());
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));

  // Events fill the socket (it holds a few hundred KB), so it is backed up.
  std::vector<std::string> events;
  for (int i = 0; i < 20; ++i) {
    events.push_back(std::to_string(i) + std::string(100000, 'e'));
    bridge.Send(events.back());
  }
  // The harness is not reading. A thousand snapshots each for two bots.
  for (int i = 0; i < 1000; ++i) {
    bridge.SendLatest("tess", "tess-" + std::to_string(i));
    bridge.SendLatest("gorn", "gorn-" + std::to_string(i));
  }
  uint64_t replaced = 0;
  std::vector<base::TimeDelta> delays;
  ASSERT_TRUE(RunWorldUntil(bridge, [&] {
    BrainBridge::SendStats stats = bridge.TakeSendStats();
    replaced += stats.replaced;
    delays.insert(delays.end(), stats.delays.begin(), stats.delays.end());
    return replaced >= 1998;
  }));
  EXPECT_EQ(replaced, 1998u);  // All but the newest of each bot.

  // The harness catches up: every event, then only the newest snapshots.
  const std::vector<std::string> frames =
      harness.ReadFrames(events.size() + 2, base::Seconds(5));
  ASSERT_EQ(frames.size(), events.size() + 2);
  EXPECT_EQ(std::vector<std::string>(frames.begin(), frames.begin() + 20),
            events);
  EXPECT_EQ(frames[20], "tess-999");
  EXPECT_EQ(frames[21], "gorn-999");
  // Nothing else follows.
  EXPECT_TRUE(harness.ReadFrames(1, base::Milliseconds(200)).empty());
  // Every frame that was sent was timed (the replaced ones were never sent).
  ASSERT_TRUE(RunWorldUntil(bridge, [&] {
    BrainBridge::SendStats stats = bridge.TakeSendStats();
    delays.insert(delays.end(), stats.delays.begin(), stats.delays.end());
    return delays.size() >= 22;
  }));
  EXPECT_EQ(delays.size(), 22u);
}

}  // namespace
}  // namespace llm_brain
