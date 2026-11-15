// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <unistd.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/message_pump.h"
#include "base/platform_thread.h"
#include "base/thread.h"
#include "base/time.h"
#include "gtest/gtest.h"
#include "net/unix_socket_client.h"
#include "net/unix_socket_server.h"

namespace net {
namespace {

// Collects events from callbacks running on IO threads.
struct Recorder {
  std::mutex mutex;
  std::vector<std::string> lines;
  int connects = 0;
  int disconnects = 0;
  std::promise<void> got_line;
  bool signaled = false;

  void OnLine(std::string line) {
    std::lock_guard<std::mutex> lock(mutex);
    lines.push_back(std::move(line));
    if (!signaled) {
      signaled = true;
      got_line.set_value();
    }
  }
};

std::string TestSocketPath() {
  return "/tmp/wlb_unittest_" + std::to_string(getpid()) + ".sock";
}

// Runs `fn` on `io` and waits for it.
template <typename Fn>
void RunOn(base::Thread& io, Fn fn) {
  std::promise<void> done;
  io.task_runner()->PostTask([&] {
    fn();
    done.set_value();
  });
  done.get_future().wait();
}

TEST(UnixSocketTest, ClientAndServerExchangeLines) {
  const std::string path = TestSocketPath();
  base::Thread server_io("server"), client_io("client");
  ASSERT_TRUE(server_io.StartWithOptions({base::MessagePumpType::IO}));
  ASSERT_TRUE(client_io.StartWithOptions({base::MessagePumpType::IO}));
  Recorder at_server, at_client;
  std::unique_ptr<UnixSocketServer> server;
  std::unique_ptr<UnixSocketClient> client;
  std::promise<void> client_connected;

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
            [&] { client_connected.set_value(); },
            [&](std::string l) { at_client.OnLine(std::move(l)); },
            {}},
        base::Milliseconds(50));
    client->Start();
  });
  client_connected.get_future().wait();

  RunOn(client_io, [&] { EXPECT_TRUE(client->SendLine("hello server")); });
  ASSERT_EQ(
      at_server.got_line.get_future().wait_for(std::chrono::milliseconds(2000)),
      std::future_status::ready);
  RunOn(server_io, [&] { server->SendLine("hello client"); });
  ASSERT_EQ(
      at_client.got_line.get_future().wait_for(std::chrono::milliseconds(2000)),
      std::future_status::ready);
  EXPECT_EQ(at_server.lines, std::vector<std::string>{"hello server"});
  EXPECT_EQ(at_client.lines, std::vector<std::string>{"hello client"});

  RunOn(client_io, [&] { client.reset(); });
  RunOn(server_io, [&] { server.reset(); });
  EXPECT_NE(access(path.c_str(), F_OK), 0);  // Socket file removed.
}

TEST(UnixSocketTest, ClientReconnectsWhenServerStartsLater) {
  const std::string path = TestSocketPath();
  base::Thread server_io("server"), client_io("client");
  ASSERT_TRUE(server_io.StartWithOptions({base::MessagePumpType::IO}));
  ASSERT_TRUE(client_io.StartWithOptions({base::MessagePumpType::IO}));
  std::unique_ptr<UnixSocketServer> server;
  std::unique_ptr<UnixSocketClient> client;
  std::promise<void> connected;

  RunOn(client_io, [&] {
    client = std::make_unique<UnixSocketClient>(
        path,
        UnixSocketClient::Callbacks{[&] { connected.set_value(); }, {}, {}},
        base::Milliseconds(30));
    client->Start();  // No server yet: retries.
  });
  base::PlatformThread::Sleep(base::Milliseconds(100));
  RunOn(server_io, [&] {
    server =
        std::make_unique<UnixSocketServer>(path, UnixSocketServer::Callbacks{});
    ASSERT_TRUE(server->Listen());
  });
  EXPECT_EQ(connected.get_future().wait_for(std::chrono::milliseconds(2000)),
            std::future_status::ready);
  RunOn(client_io, [&] { client.reset(); });
  RunOn(server_io, [&] { server.reset(); });
}

TEST(UnixSocketTest, ServerDropsLinesWithoutClient) {
  const std::string path = TestSocketPath();
  base::Thread io("server");
  ASSERT_TRUE(io.StartWithOptions({base::MessagePumpType::IO}));
  std::unique_ptr<UnixSocketServer> server;
  RunOn(io, [&] {
    server =
        std::make_unique<UnixSocketServer>(path, UnixSocketServer::Callbacks{});
    ASSERT_TRUE(server->Listen());
    server->SendLine("nobody listening");
    EXPECT_EQ(server->stats().lines_dropped, 1u);
    server.reset();
  });
}

}  // namespace
}  // namespace net
