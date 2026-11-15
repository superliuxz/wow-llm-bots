// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Next to brain_bridge.cc. mod-llm-brain.cmake keeps
// *_unittest.cc out of the server build; common/CMakeLists.txt builds it as a
// test.

#include "brain_bridge.h"

#include <unistd.h>

#include <functional>
#include <memory>
#include <string>
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
#include "net/unix_socket_client.h"

namespace llm_brain {
namespace {

constexpr base::TimeDelta kTimeout = base::Seconds(2);

std::string TestSocketPath() {
  return "/tmp/llm_brain_unittest_" + std::to_string(getpid()) + ".sock";
}

// Pumps the bridge's world-thread tasks (this test thread plays AzerothCore's
// world thread) until `done()` or timeout.
bool RunWorldUntil(BrainBridge& bridge, const std::function<bool()>& done) {
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

TEST(BrainBridgeTest, NewHarnessReplacesOldOne) {
  WorldRecorder world;
  BrainBridge bridge(TestSocketPath(), 100, world.Callbacks());
  ASSERT_TRUE(bridge.Start());
  FakeHarness first(TestSocketPath());
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return bridge.connected(); }));
  FakeHarness second(TestSocketPath());
  ASSERT_TRUE(second.WaitConnected());
  EXPECT_TRUE(first.WaitDisconnected());

  bridge.Send("to the new harness");
  ASSERT_TRUE(RunWorldUntil(bridge, [&] { return !second.frames().empty(); }));
  EXPECT_TRUE(first.frames().empty());
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

}  // namespace
}  // namespace llm_brain
