// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "module_connection.h"

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
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "net/unix_socket_server.h"
#include "proto/snapshot.h"

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
                       if (pb::ReadBeve(frame, &cmd)) {
                         base::AutoLock lock(lock_);
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

  void Send(const pb::ModuleMessage& msg) {
    std::string bytes = pb::WriteBeve(msg);
    io_.task_runner()->PostTask(
        [s = server_, bytes = std::move(bytes)] { s->SendFrame(bytes); });
  }

  std::vector<pb::Command> commands() {
    base::AutoLock lock(lock_);
    return commands_;
  }

 private:
  const std::string path_;
  base::Thread io_;
  base::raw_ptr<net::UnixSocketServer> server_ = nullptr;
  base::WaitableEvent client_connected_;
  base::WaitableEvent got_command_;
  base::Lock lock_;
  std::vector<pb::Command> commands_ GUARDED_BY(lock_);
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
            }},
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
    const auto cmds = s1.commands();
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0].command_id, 42u);
    ASSERT_TRUE(cmds[0].choose.has_value());
    EXPECT_EQ(cmds[0].choose->option, 2u);
  }  // s1 stops and unlinks sock.

  // Start server #2 on the same socket -> client reconnects automatically!
  {
    FakeModuleServer s2(sock);
    ASSERT_TRUE(s2.Start());
    s2.WaitForClient();

    pb::ModuleMessage m2;
    pb::Snapshot& s2_snap = m2.snapshot.emplace();
    s2_snap.bot = "Testpriest";
    s2_snap.seq = 2;
    s2.Send(m2);
    got_snap_2.Wait();
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

  ::rmdir(dir.c_str());
}

}  // namespace
}  // namespace harness
