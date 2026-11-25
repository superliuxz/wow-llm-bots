// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
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

// Collects frames from callbacks running on IO threads.
class Recorder {
 public:
  void OnFrame(std::string frame) {
    {
      base::AutoLock lock(lock_);
      frames_.push_back(std::move(frame));
    }
    got_frame_.Signal();
  }
  bool WaitForFrame() { return got_frame_.TimedWait(kTimeout); }
  std::vector<std::string> frames() {
    base::AutoLock lock(lock_);
    return frames_;
  }

 private:
  base::Lock lock_;
  std::vector<std::string> frames_ GUARDED_BY(lock_);
  base::WaitableEvent got_frame_;
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

TEST(UnixSocketTest, ClientAndServerExchangeFrames) {
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
        path, UnixSocketServer::Callbacks{
                  {},
                  [&](std::string f) { at_server.OnFrame(std::move(f)); },
                  {},
                  /*on_drained=*/{}});
    ASSERT_TRUE(server->Listen());
  });
  RunOn(client_io, [&] {
    client = std::make_unique<UnixSocketClient>(
        path,
        UnixSocketClient::Callbacks{
            [&] { client_connected.Signal(); },
            [&](std::string f) { at_client.OnFrame(std::move(f)); },
            {}},
        base::Milliseconds(50));
    client->Start();
  });
  ASSERT_TRUE(client_connected.TimedWait(kTimeout));

  // Binary payloads (serialized protos) contain newlines and zero bytes.
  const std::string binary("snap\nshot\0\xff", 11);
  RunOn(client_io, [&] { EXPECT_TRUE(client->SendFrame("hello server")); });
  ASSERT_TRUE(at_server.WaitForFrame());
  RunOn(server_io, [&] { server->SendFrame(binary); });
  ASSERT_TRUE(at_client.WaitForFrame());
  EXPECT_EQ(at_server.frames(), std::vector<std::string>{"hello server"});
  EXPECT_EQ(at_client.frames(), std::vector<std::string>{binary});

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

TEST(UnixSocketTest, ServerDropsFramesWithoutClient) {
  const std::string path = TestSocketPath();
  base::Thread io("server");
  ASSERT_TRUE(io.StartWithOptions(IoOptions()));
  RunOn(io, [&] {
    UnixSocketServer server(path, UnixSocketServer::Callbacks{});
    ASSERT_TRUE(server.Listen());
    server.SendFrame("nobody listening");
    EXPECT_EQ(server.stats().frames_dropped, 1u);
  });
}

TEST(UnixSocketTest, ServerClosesClientOnCorruptFrame) {
  const std::string path = TestSocketPath();
  base::Thread io("server");
  ASSERT_TRUE(io.StartWithOptions(IoOptions()));
  std::unique_ptr<UnixSocketServer> server;
  base::WaitableEvent connected, disconnected;
  RunOn(io, [&] {
    server = std::make_unique<UnixSocketServer>(
        path, UnixSocketServer::Callbacks{[&] { connected.Signal(); },
                                          {},
                                          [&] { disconnected.Signal(); },
                                          /*on_drained=*/{}});
    ASSERT_TRUE(server->Listen());
  });

  // A raw client that sends a length far over the limit.
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
            0);
  ASSERT_TRUE(connected.TimedWait(kTimeout));
  const char garbage[] = {'\xff', '\xff', '\xff', '\xff', 'x'};
  ASSERT_EQ(send(fd, garbage, sizeof(garbage), MSG_NOSIGNAL),
            static_cast<ssize_t>(sizeof(garbage)));
  EXPECT_TRUE(disconnected.TimedWait(kTimeout));
  RunOn(io, [&] { EXPECT_FALSE(server->connected()); });

  close(fd);
  RunOn(io, [&] { server.reset(); });
}

TEST(UnixSocketTest, ClientCanDestroySelfInOnDisconnected) {
  const std::string path = TestSocketPath();
  base::Thread server_io("server"), client_io("client");
  ASSERT_TRUE(server_io.StartWithOptions(IoOptions()));
  ASSERT_TRUE(client_io.StartWithOptions(IoOptions()));
  std::unique_ptr<UnixSocketServer> server;
  std::unique_ptr<UnixSocketClient> client;
  base::WaitableEvent connected, disconnected;

  RunOn(server_io, [&] {
    server =
        std::make_unique<UnixSocketServer>(path, UnixSocketServer::Callbacks{});
    ASSERT_TRUE(server->Listen());
  });
  RunOn(client_io, [&] {
    client = std::make_unique<UnixSocketClient>(
        path,
        UnixSocketClient::Callbacks{[&] { connected.Signal(); },
                                    {},
                                    [&] {
                                      client.reset();
                                      disconnected.Signal();
                                    }},
        base::Milliseconds(50));
    client->Start();
  });
  ASSERT_TRUE(connected.TimedWait(kTimeout));
  // Server drops connection -> client's on_disconnected destroys client.
  RunOn(server_io, [&] { server.reset(); });
  ASSERT_TRUE(disconnected.TimedWait(kTimeout));
}

TEST(UnixSocketTest, SocketIsOwnerOnlyAndRejectsSecondClientWhileLive) {
  const std::string path = TestSocketPath();
  base::Thread server_io("server"), client_io("client");
  ASSERT_TRUE(server_io.StartWithOptions(IoOptions()));
  ASSERT_TRUE(client_io.StartWithOptions(IoOptions()));
  std::unique_ptr<UnixSocketServer> server;
  std::unique_ptr<UnixSocketClient> first;
  base::WaitableEvent first_connected, first_disconnected;
  Recorder at_first;

  RunOn(server_io, [&] {
    server =
        std::make_unique<UnixSocketServer>(path, UnixSocketServer::Callbacks{});
    ASSERT_TRUE(server->Listen());
    struct stat st{};
    ASSERT_EQ(stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
  });

  RunOn(client_io, [&] {
    first = std::make_unique<UnixSocketClient>(
        path,
        UnixSocketClient::Callbacks{
            [&] { first_connected.Signal(); },
            [&](std::string f) { at_first.OnFrame(std::move(f)); },
            [&] { first_disconnected.Signal(); }},
        base::Milliseconds(50));
    first->Start();
  });
  ASSERT_TRUE(first_connected.TimedWait(kTimeout));

  // A second raw client connecting while `first` is live is refused and closed
  // without kicking `first` off.
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(fd, 0);
  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)),
            0);
  char buf[16];
  // Server immediately closes the rejected socket -> EOF.
  EXPECT_EQ(recv(fd, buf, sizeof(buf), 0), 0);
  close(fd);

  RunOn(server_io, [&] {
    EXPECT_TRUE(server->connected());
    EXPECT_EQ(server->stats().connections, 1u);
    EXPECT_EQ(server->stats().connections_rejected, 1u);
    EXPECT_TRUE(server->SendFrame("still alive"));
  });
  ASSERT_TRUE(at_first.WaitForFrame());
  EXPECT_EQ(at_first.frames(), std::vector<std::string>{"still alive"});
  EXPECT_FALSE(first_disconnected.TimedWait(base::Milliseconds(50)));

  RunOn(client_io, [&] { first.reset(); });
  RunOn(server_io, [&] { server.reset(); });
}

TEST(UnixSocketTest, HungClientIsReplacedByTheNextOne) {
  const std::string path = TestSocketPath();
  base::Thread server_io("server");
  ASSERT_TRUE(server_io.StartWithOptions(IoOptions()));
  std::unique_ptr<UnixSocketServer> server;
  base::WaitableEvent disconnected;
  RunOn(server_io, [&] {
    server = std::make_unique<UnixSocketServer>(
        path,
        UnixSocketServer::Callbacks{
            .on_disconnected = base::BindRepeating(
                [](base::WaitableEvent* e) { e->Signal(); }, &disconnected)},
        /*max_output_bytes=*/64 << 20, /*hung_after=*/base::TimeDelta());
    ASSERT_TRUE(server->Listen());
  });
  const auto connect_raw = [&] {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    EXPECT_EQ(
        connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
    return fd;
  };
  // A client that never reads: the server's output backs up.
  const int hung = connect_raw();
  bool backed_up = false;
  for (int i = 0; i < 200 && !backed_up; ++i) {
    RunOn(server_io, [&] {
      if (!server->connected()) {
        return;
      }
      server->SendFrame(std::string(64 * 1024, 'x'));
      backed_up = server->pending_output_bytes() > 0;
    });
  }
  ASSERT_TRUE(backed_up);
  // The next client takes its place instead of being refused.
  const int next = connect_raw();
  ASSERT_TRUE(disconnected.TimedWait(kTimeout));
  RunOn(server_io, [&] {
    EXPECT_TRUE(server->connected());
    EXPECT_EQ(server->stats().connections, 2u);
    EXPECT_EQ(server->stats().connections_replaced, 1u);
    EXPECT_EQ(server->stats().connections_rejected, 0u);
  });
  close(hung);
  close(next);
  RunOn(server_io, [&] { server.reset(); });
}

}  // namespace
}  // namespace net
