// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "run.h"

#include <stdlib.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/message_loop/message_pump.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "httplib.h"
#include "net/unix_socket_server.h"
#include "proto/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

TEST(RunArgsTest, DefaultsAndAllFlags) {
  std::string error;
  const auto def = ParseRunArgs({}, &error);
  ASSERT_TRUE(def) << error;
  EXPECT_EQ(def->socket_path, "out/server/run/brain.sock");
  EXPECT_EQ(def->rules_path, "content/RULES.md");
  EXPECT_EQ(def->slots, 16u);

  const auto custom =
      ParseRunArgs({"--socket=/tmp/s.sock", "--rules=r.md",
                    "--bots-dir=", "--host=127.0.0.2", "--port=9090",
                    "--slots=4", "--temperature=0", "--seconds=2.5"},
                   &error);
  ASSERT_TRUE(custom) << error;
  EXPECT_EQ(custom->socket_path, "/tmp/s.sock");
  EXPECT_EQ(custom->rules_path, "r.md");
  EXPECT_EQ(custom->bots_dir, "");
  EXPECT_EQ(custom->endpoint.host, "127.0.0.2");
  EXPECT_EQ(custom->endpoint.port, 9090);
  EXPECT_EQ(custom->slots, 4u);
  EXPECT_FLOAT_EQ(custom->temperature, 0.0f);
  EXPECT_DOUBLE_EQ(custom->seconds, 2.5);

  EXPECT_FALSE(ParseRunArgs({"unexpected"}, &error));
  EXPECT_FALSE(ParseRunArgs({"--slots=0"}, &error));
  EXPECT_FALSE(ParseRunArgs({"--port=99999"}, &error));
}

TEST(RunHarnessTest, EndToEndSocketToStreamingLlmToChooseCommand) {
  std::string dir = testing::TempDir() + "wlb-run-XXXXXX";
  ASSERT_NE(::mkdtemp(dir.data()), nullptr);
  const std::string sock = dir + "/brain.sock";

  // 1. Fake SSE llama-server returning `do: 1\nexpect: 1\nwatch: none\nlook:
  // 60`.
  httplib::Server http_srv;
  http_srv.set_tcp_nodelay(true);
  http_srv.Post("/v1/chat/completions", [](const httplib::Request&,
                                           httplib::Response& res) {
    res.set_chunked_content_provider(
        "text/event-stream", [](size_t, httplib::DataSink& sink) {
          const char kStream[] =
              "data: {\"choices\":[{\"delta\":{\"content\":"
              "\"do: 1\\n\"}}]}\n\n"
              "data: {\"choices\":[{\"delta\":{\"content\":"
              "\"expect: 1\\nwatch: none\\nlook: 60\"}}]}\n\n"
              "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":"
              "100,\"completion_tokens\":10}}\n\n"
              "data: [DONE]\n\n";
          sink.write(kStream, sizeof(kStream) - 1);
          sink.done();
          return true;
        });
  });
  const int port = http_srv.bind_to_any_port("127.0.0.1");
  base::Thread http_thread("fake-http");
  http_thread.Start();
  http_thread.task_runner()->PostTask([&] { http_srv.listen_after_bind(); });
  while (!http_srv.is_running()) {
    base::PlatformThread::Sleep(base::Milliseconds(1));
  }

  // 2. Fake Unix-socket game server sending a snapshot and awaiting Choose.
  base::Thread mod_io("fake-mod");
  ASSERT_TRUE(mod_io.StartWithOptions({base::MessagePumpType::IO}));
  base::raw_ptr<net::UnixSocketServer> mod_srv = nullptr;
  base::WaitableEvent mod_listening;
  base::WaitableEvent harness_connected;
  base::WaitableEvent got_choose;
  base::Lock cmd_lock;
  pb::Command received_cmd;

  mod_io.task_runner()->PostTask([&] {
    mod_srv = new net::UnixSocketServer(
        sock, net::UnixSocketServer::Callbacks{
                  [&] { harness_connected.Signal(); },
                  [&](std::string frame) {
                    pb::Command cmd;
                    if (pb::ReadBeve(frame, &cmd) && cmd.choose) {
                      base::AutoLock lock(cmd_lock);
                      received_cmd = std::move(cmd);
                      got_choose.Signal();
                    }
                  },
                  {},
                  {}});
    ASSERT_TRUE(mod_srv->Listen());
    mod_listening.Signal();
  });
  mod_listening.Wait();

  // 3. Start `RunHarness` on a background thread and stop it once Choose
  // arrives.
  RunOptions opts;
  opts.socket_path = sock;
  opts.rules_path = std::string(WLB_HARNESS_TESTDATA) + "/rules.txt";
  opts.bots_dir = "";
  opts.endpoint.port = port;
  opts.slots = 2;

  base::WaitableEvent stop_event;
  int exit_code = -1;
  base::Thread runner_thread("runner");
  runner_thread.Start();
  runner_thread.task_runner()->PostTask([&] {
    exit_code = RunHarness(opts, &stop_event, [](const std::string&) {});
  });

  harness_connected.Wait();

  pb::ModuleMessage msg;
  pb::Snapshot& snap = msg.snapshot.emplace();
  snap.bot = "Testpriest";
  snap.seq = 7;
  snap.monotonic_us =
      (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.self.health = 60;
  snap.base.self.health_max = 60;
  pb::Unit& boar = snap.base.nearby.units.emplace_back();
  boar.guid = "0xABCD";
  boar.name = "Mottled Boar";
  boar.kind = pb::Unit::NPC;
  boar.reaction = pb::Unit::NEUTRAL;
  boar.rel.dist_yd = 6.0f;
  pb::Option& o1 = snap.options.emplace_back();
  o1.text = "Attack Mottled Boar";
  o1.guid = "0xABCD";
  snap.options.emplace_back().text = "Keep doing what I'm doing";

  std::string payload = pb::WriteBeve(msg);
  mod_io.task_runner()->PostTask(
      [s = mod_srv, payload = std::move(payload)] { s->SendFrame(payload); });

  got_choose.Wait();
  stop_event.Signal();
  runner_thread.Stop();

  {
    base::AutoLock lock(cmd_lock);
    EXPECT_EQ(received_cmd.bot, "Testpriest");
    ASSERT_TRUE(received_cmd.choose.has_value());
    EXPECT_EQ(received_cmd.choose->seq, 7u);
    EXPECT_EQ(received_cmd.choose->option, 1u);
  }
  EXPECT_EQ(exit_code, 0);

  mod_io.task_runner()->PostTask(
      [s = std::move(mod_srv)]() mutable { s.ClearAndDelete(); });
  mod_io.Stop();
  http_srv.stop();
  http_thread.Stop();
  ::rmdir(dir.c_str());
}

}  // namespace
}  // namespace harness
