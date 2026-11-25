// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "runtime/module_connection.h"

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/message_loop/message_pump.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/thread_annotations.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "net/unix_socket_server.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

class FakeModuleServer {
 public:
  explicit FakeModuleServer(std::string path)
      : path_(std::move(path)), io_("fake-module-io") {}
  ~FakeModuleServer() { Stop(); }

  bool Start() {
    if (!io_.StartWithOptions({base::MessagePumpType::IO})) {
      return false;
    }
    base::WaitableEvent ready;
    bool ok = false;
    io_.task_runner()->PostTask([&] {
      server_ = new net::UnixSocketServer(
          path_, net::UnixSocketServer::Callbacks{
                     [this] { client_connected_.Signal(); },
                     [this](std::string frame) {
                       pb::Command cmd;
                       if (!pb::ReadBeve(frame, &cmd)) {
                         return;
                       }
                       base::AutoLock lock(lock_);
                       if (cmd.hello) {
                         hellos_.push_back(std::move(*cmd.hello));
                         got_hello_.Signal();
                       } else {
                         commands_.push_back(std::move(cmd));
                         got_command_.Signal();
                       }
                     },
                     {},
                     {}});
      ok = server_->Listen();
      ready.Signal();
    });
    ready.Wait();
    return ok;
  }

  void Stop() {
    if (!io_.IsRunning()) {
      return;
    }
    io_.task_runner()->PostTask(
        [s = std::move(server_)]() mutable { s.ClearAndDelete(); });
    io_.Stop();
  }

  void WaitForClient() { client_connected_.Wait(); }
  void WaitForCommand() { got_command_.Wait(); }
  void WaitForHello() { got_hello_.Wait(); }

  // Sent as this checkout's module would: with kWireVersion, unless the test
  // sets another.
  void Send(const pb::ModuleMessage& msg, bool keep_version = false) {
    pb::ModuleMessage versioned = msg;
    if (!keep_version) {
      versioned.version = pb::kWireVersion;
    }
    std::string bytes = pb::WriteBeve(versioned);
    io_.task_runner()->PostTask(
        [s = server_, bytes = std::move(bytes)] { s->SendFrame(bytes); });
  }

  std::vector<pb::Command> commands() {
    base::AutoLock lock(lock_);
    return commands_;
  }
  std::vector<pb::Hello> hellos() {
    base::AutoLock lock(lock_);
    return hellos_;
  }

 private:
  const std::string path_;
  base::Thread io_;
  base::raw_ptr<net::UnixSocketServer> server_ = nullptr;
  base::WaitableEvent client_connected_;
  base::WaitableEvent got_command_;
  base::WaitableEvent got_hello_;
  base::Lock lock_;
  std::vector<pb::Command> commands_ GUARDED_BY(lock_);
  std::vector<pb::Hello> hellos_ GUARDED_BY(lock_);
};

TEST(ModuleConnectionTest, ReceivesSnapshotsEventsSendsCommandsAndReconnects) {
  std::string dir = testing::TempDir() + "wlb-modconn-XXXXXX";
  ASSERT_NE(::mkdtemp(dir.data()), nullptr);
  const std::string sock = dir + "/brain.sock";

  base::Thread harness_seq("harness-seq");
  harness_seq.Start();

  base::WaitableEvent got_snap_1;
  base::WaitableEvent got_event;
  base::WaitableEvent got_snap_2;
  std::vector<uint32_t> seqs;
  std::string event_text;

  std::unique_ptr<ModuleConnection> conn;
  base::WaitableEvent conn_started;
  harness_seq.task_runner()->PostTask([&] {
    conn = std::make_unique<ModuleConnection>(
        sock,
        ModuleConnection::Callbacks{
            {},
            [&](pb::Snapshot snap, base::TimeTicks /*at*/) {
              seqs.push_back(snap.seq);
              if (seqs.size() == 1) {
                got_snap_1.Signal();
              } else if (seqs.size() == 2) {
                got_snap_2.Signal();
              }
            },
            [&](pb::ImmediateEvent ev, base::TimeTicks /*at*/) {
              if (ev.event.error) {
                event_text = ev.event.error->text;
              }
              got_event.Signal();
            },
            {}},
        /*retry_delay=*/base::Milliseconds(20));
    ASSERT_TRUE(conn->Start());
    conn_started.Signal();
  });
  conn_started.Wait();

  // Start server #1 AFTER the client already started -> tests retry connect!
  {
    FakeModuleServer s1(sock);
    ASSERT_TRUE(s1.Start());
    s1.WaitForClient();

    pb::ModuleMessage m1;
    pb::Snapshot& s1_snap = m1.snapshot.emplace();
    s1_snap.bot = "Testpriest";
    s1_snap.seq = 1;
    s1_snap.monotonic_us =
        (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
    s1.Send(m1);
    got_snap_1.Wait();

    pb::ModuleMessage me;
    pb::ImmediateEvent& imm = me.event.emplace();
    imm.bot = "Testpriest";
    imm.event.error.emplace().text = "oom";
    s1.Send(me);
    got_event.Wait();

    pb::Command cmd;
    cmd.bot = "Testpriest";
    cmd.command_id = 42;
    pb::Command::Choose& choose = cmd.choose.emplace();
    choose.seq = 1;
    choose.option = 2;
    harness_seq.task_runner()->PostTask([&] { conn->SendCommand(cmd); });
    s1.WaitForCommand();
    // The harness said hello first, as this build (a module without a hello
    // of its own, like this one, is still served).
    ASSERT_EQ(s1.hellos().size(), 1u);
    EXPECT_EQ(s1.hellos()[0], pb::MakeHello("harness"));
    const auto cmds = s1.commands();
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0].command_id, 42u);
    EXPECT_EQ(cmds[0].version, pb::kWireVersion);
    ASSERT_TRUE(cmds[0].choose.has_value());
    EXPECT_EQ(cmds[0].choose->option, 2u);
  }  // s1 stops and unlinks sock.

  // Start server #2 on the same socket -> client reconnects automatically!
  {
    FakeModuleServer s2(sock);
    ASSERT_TRUE(s2.Start());
    s2.WaitForClient();

    // A module of another wire version: dropped, and counted.
    pb::ModuleMessage old;
    old.version = pb::kWireVersion + 1;
    old.snapshot.emplace().seq = 99;
    s2.Send(old, /*keep_version=*/true);

    pb::ModuleMessage m2;
    pb::Snapshot& s2_snap = m2.snapshot.emplace();
    s2_snap.bot = "Testpriest";
    s2_snap.seq = 2;
    s2.Send(m2);
    got_snap_2.Wait();
    s2.WaitForHello();  // Again on the new connection.
    EXPECT_EQ(s2.hellos().size(), 1u);
  }

  ModuleConnection::DeliveryStats stats;
  base::WaitableEvent stopped;
  harness_seq.task_runner()->PostTask([&] {
    stats = conn->TakeDeliveryStats();
    conn.reset();
    stopped.Signal();
  });
  stopped.Wait();
  harness_seq.Stop();

  EXPECT_EQ(seqs, (std::vector<uint32_t>{1, 2}));
  EXPECT_EQ(event_text, "oom");
  EXPECT_EQ(stats.snapshots, 2u);
  EXPECT_EQ(stats.events, 1u);
  EXPECT_EQ(stats.receive_ages.size(), 1u);
  EXPECT_EQ(stats.wrong_version, 1u);

  ::rmdir(dir.c_str());
}

// A module of another build: its hello refuses the connection (reported
// once), nothing after it is delivered or sent; a module of this build on the
// next connection is served again.
TEST(ModuleConnectionTest, RefusesAModuleOfAnotherBuildUntilReconnect) {
  std::string dir = testing::TempDir() + "wlb-modconn-XXXXXX";
  ASSERT_NE(::mkdtemp(dir.data()), nullptr);
  const std::string sock = dir + "/brain.sock";

  base::Thread harness_seq("harness-seq");
  harness_seq.Start();
  base::WaitableEvent refused;
  base::WaitableEvent got_snap;
  std::vector<std::string> reasons;
  std::vector<uint32_t> seqs;
  std::unique_ptr<ModuleConnection> conn;
  base::WaitableEvent conn_started;
  harness_seq.task_runner()->PostTask([&] {
    conn = std::make_unique<ModuleConnection>(
        sock,
        ModuleConnection::Callbacks{
            {},
            [&](pb::Snapshot snap, base::TimeTicks /*at*/) {
              seqs.push_back(snap.seq);
              got_snap.Signal();
            },
            {},
            [&](std::string reason) {
              reasons.push_back(std::move(reason));
              refused.Signal();
            }},
        /*retry_delay=*/base::Milliseconds(20));
    ASSERT_TRUE(conn->Start());
    conn_started.Signal();
  });
  conn_started.Wait();

  pb::Command cmd;
  cmd.bot = "Testpriest";
  cmd.command_id = 7;
  cmd.say.emplace().text = "hi";
  {
    FakeModuleServer stale(sock);
    ASSERT_TRUE(stale.Start());
    stale.WaitForClient();
    pb::ModuleMessage hello;
    hello.hello = pb::MakeHello("module");
    hello.hello->schema_hash = "0123456789abcdef";
    stale.Send(hello);
    stale.Send(hello);  // Reported once per connection.
    refused.Wait();
    pb::ModuleMessage snap;
    snap.snapshot.emplace().seq = 5;
    stale.Send(snap);
    harness_seq.task_runner()->PostTask([&] { conn->SendCommand(cmd); });
    base::PlatformThread::Sleep(base::Milliseconds(100));
    stale.WaitForHello();
    EXPECT_TRUE(stale.commands().empty());
  }
  {
    FakeModuleServer fresh(sock);
    ASSERT_TRUE(fresh.Start());
    fresh.WaitForClient();
    pb::ModuleMessage hello;
    hello.hello = pb::MakeHello("module");
    fresh.Send(hello);
    pb::ModuleMessage snap;
    snap.snapshot.emplace().seq = 6;
    fresh.Send(snap);
    got_snap.Wait();
    harness_seq.task_runner()->PostTask([&] { conn->SendCommand(cmd); });
    fresh.WaitForCommand();
    EXPECT_EQ(fresh.commands().size(), 1u);
  }

  base::WaitableEvent stopped;
  harness_seq.task_runner()->PostTask([&] {
    conn.reset();
    stopped.Signal();
  });
  stopped.Wait();
  harness_seq.Stop();

  ASSERT_EQ(reasons.size(), 1u);
  EXPECT_NE(reasons[0].find("module has wire schema 0123456789abcdef"),
            std::string::npos)
      << reasons[0];
  EXPECT_EQ(seqs, std::vector<uint32_t>{6});
  ::rmdir(dir.c_str());
}

}  // namespace
}  // namespace harness
