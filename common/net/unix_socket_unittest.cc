// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <unistd.h>

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
#include "net/unix_socket_server.h"

namespace net {
namespace {

constexpr base::TimeDelta kTimeout = base::Seconds(2);

// Collects lines from callbacks running on IO threads.
class Recorder {
 public:
  void OnLine(std::string line) {
    {
      base::AutoLock lock(lock_);
      lines_.push_back(std::move(line));
    }
    got_line_.Signal();
  }
  bool WaitForLine() { return got_line_.TimedWait(kTimeout); }
  std::vector<std::string> lines() {
    base::AutoLock lock(lock_);
    return lines_;
  }

 private:
  base::Lock lock_;
  std::vector<std::string> lines_ GUARDED_BY(lock_);
  base::WaitableEvent got_line_;
};

std::string TestSocketPath() {
  return "/tmp/wlb_unittest_" + std::to_string(getpid()) + ".sock";
}

base::Thread::Options IoOptions() {
  return {base::MessagePumpType::IO};
}

// Runs `fn` on `io` and waits for it.
template <typename Fn>
void RunOn(base::Thread& io, Fn fn) {
  base::WaitableEvent done;
  io.task_runner()->PostTask([&] {
    fn();
    done.Signal();
  });
  done.Wait();
}

TEST(UnixSocketTest, ClientAndServerExchangeLines) {
  const std::string path = TestSocketPath();
  base::Thread server_io("server"), client_io("client");
  ASSERT_TRUE(server_io.StartWithOptions(IoOptions()));
  ASSERT_TRUE(client_io.StartWithOptions(IoOptions()));
  Recorder at_server, at_client;
  std::unique_ptr<UnixSocketServer> server;
  std::unique_ptr<UnixSocketClient> client;
  base::WaitableEvent client_connected;

  RunOn(server_io, [&] {
    server = std::make_unique<UnixSocketServer>(
        path,
        UnixSocketServer::Callbacks{
            {}, [&](std::string l) { at_server.OnLine(std::move(l)); }, {}});
    ASSERT_TRUE(server->Listen());
  });
  RunOn(client_io, [&] {
    client = std::make_unique<UnixSocketClient>(
        path,
        UnixSocketClient::Callbacks{
            [&] { client_connected.Signal(); },
            [&](std::string l) { at_client.OnLine(std::move(l)); },
            {}},
        base::Milliseconds(50));
    client->Start();
  });
  ASSERT_TRUE(client_connected.TimedWait(kTimeout));

  RunOn(client_io, [&] { EXPECT_TRUE(client->SendLine("hello server")); });
  ASSERT_TRUE(at_server.WaitForLine());
  RunOn(server_io, [&] { server->SendLine("hello client"); });
  ASSERT_TRUE(at_client.WaitForLine());
  EXPECT_EQ(at_server.lines(), std::vector<std::string>{"hello server"});
  EXPECT_EQ(at_client.lines(), std::vector<std::string>{"hello client"});

  RunOn(client_io, [&] { client.reset(); });
  RunOn(server_io, [&] { server.reset(); });
  EXPECT_NE(access(path.c_str(), F_OK), 0);  // Socket file removed.
}

TEST(UnixSocketTest, ClientReconnectsWhenServerStartsLater) {
  const std::string path = TestSocketPath();
  base::Thread server_io("server"), client_io("client");
  ASSERT_TRUE(server_io.StartWithOptions(IoOptions()));
  ASSERT_TRUE(client_io.StartWithOptions(IoOptions()));
  std::unique_ptr<UnixSocketServer> server;
  std::unique_ptr<UnixSocketClient> client;
  base::WaitableEvent connected;

  RunOn(client_io, [&] {
    client = std::make_unique<UnixSocketClient>(
        path, UnixSocketClient::Callbacks{[&] { connected.Signal(); }, {}, {}},
        base::Milliseconds(30));
    client->Start();  // No server yet: retries.
  });
  base::PlatformThread::Sleep(base::Milliseconds(100));
  RunOn(server_io, [&] {
    server =
        std::make_unique<UnixSocketServer>(path, UnixSocketServer::Callbacks{});
    ASSERT_TRUE(server->Listen());
  });
  EXPECT_TRUE(connected.TimedWait(kTimeout));
  RunOn(client_io, [&] { client.reset(); });
  RunOn(server_io, [&] { server.reset(); });
}

TEST(UnixSocketTest, ServerDropsLinesWithoutClient) {
  const std::string path = TestSocketPath();
  base::Thread io("server");
  ASSERT_TRUE(io.StartWithOptions(IoOptions()));
  RunOn(io, [&] {
    UnixSocketServer server(path, UnixSocketServer::Callbacks{});
    ASSERT_TRUE(server.Listen());
    server.SendLine("nobody listening");
    EXPECT_EQ(server.stats().lines_dropped, 1u);
  });
}

}  // namespace
}  // namespace net
