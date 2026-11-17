// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "llm_client.h"

#include <chrono>
#include <cstddef>
#include <flat_set>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/thread_annotations.h"
#include "base/threading/thread.h"
#include "gtest/gtest.h"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace harness {
namespace {

using Json = nlohmann::json;

constexpr char kReplyBody[] = R"({
  "choices": [{"message": {"role": "assistant", "content": "do: 1\nlook: 30"}}],
  "usage": {"prompt_tokens": 812, "completion_tokens": 9},
  "timings": {"prompt_n": 240, "prompt_ms": 31.5, "predicted_ms": 64.25}
})";

LlmRequest SampleRequest() {
  LlmRequest request;
  request.system = "You are a player.";
  request.user = "Your screen:\nme: Tess";
  request.grammar = "root ::= \"x\"";
  request.slot = 3;
  request.max_tokens = 80;
  request.temperature = 0.5f;
  return request;
}

TEST(LlmClientTest, TheBodyCarriesEverythingTheServerNeeds) {
  const Json body = Json::parse(BuildChatBody(SampleRequest()));
  EXPECT_EQ(body["messages"][0]["role"], "system");
  EXPECT_EQ(body["messages"][0]["content"], "You are a player.");
  EXPECT_EQ(body["messages"][1]["role"], "user");
  EXPECT_EQ(body["messages"][1]["content"], "Your screen:\nme: Tess");
  EXPECT_EQ(body["grammar"], "root ::= \"x\"");
  EXPECT_EQ(body["id_slot"], 3);
  EXPECT_EQ(body["max_tokens"], 80);
  EXPECT_FLOAT_EQ(body["temperature"].get<float>(), 0.5f);
  EXPECT_EQ(body["cache_prompt"], true);
  EXPECT_EQ(body["stream"], true);
  EXPECT_EQ(body["stream_options"]["include_usage"], true);
  EXPECT_EQ(body["chat_template_kwargs"]["enable_thinking"], false);

  LlmRequest unstreamed = SampleRequest();
  unstreamed.stream = false;
  EXPECT_FALSE(Json::parse(BuildChatBody(unstreamed)).contains("stream"));
}

TEST(LlmClientTest, SlotAndGrammarAreLeftOutWhenNotGiven) {
  LlmRequest request = SampleRequest();
  request.slot = -1;
  request.grammar.clear();
  const Json body = Json::parse(BuildChatBody(request));
  EXPECT_FALSE(body.contains("id_slot"));
  EXPECT_FALSE(body.contains("grammar"));
}

TEST(LlmClientTest, InvalidUtf8InTheTextDoesNotThrow) {
  LlmRequest request = SampleRequest();
  request.user = "name: caf\xE9";  // Latin-1, not UTF-8.
  const Json body = Json::parse(BuildChatBody(request));
  EXPECT_NE(body["messages"][1]["content"].get<std::string>().find("caf"),
            std::string::npos);
}

TEST(LlmClientTest, ParsesTheReplyTextTokensAndTimings) {
  const LlmResult result = ParseChatReply(kReplyBody);
  ASSERT_TRUE(result.reply) << result.error;
  EXPECT_EQ(result.reply->text, "do: 1\nlook: 30");
  EXPECT_EQ(result.reply->prompt_tokens, 812);
  EXPECT_EQ(result.reply->completion_tokens, 9);
  EXPECT_EQ(result.reply->prompt_computed, 240);
  EXPECT_DOUBLE_EQ(result.reply->prompt_ms, 31.5);
  EXPECT_DOUBLE_EQ(result.reply->predicted_ms, 64.25);
}

TEST(LlmClientTest, ATextOnlyReplyIsEnough) {
  const LlmResult result =
      ParseChatReply(R"({"choices": [{"message": {"content": "hi"}}]})");
  ASSERT_TRUE(result.reply) << result.error;
  EXPECT_EQ(result.reply->text, "hi");
  EXPECT_EQ(result.reply->prompt_tokens, 0);
}

TEST(LlmClientTest, ReportsWhatIsWrongWithAReply) {
  EXPECT_FALSE(ParseChatReply("not json").reply);
  EXPECT_FALSE(ParseChatReply("[1, 2]").reply);
  EXPECT_FALSE(ParseChatReply("{}").reply);
  EXPECT_FALSE(ParseChatReply(R"({"choices": []})").reply);
  EXPECT_FALSE(ParseChatReply(R"({"choices": [{"message": {}}]})").reply);
  EXPECT_FALSE(
      ParseChatReply(R"({"choices": [{"message": {"content": 5}}]})").reply);
  EXPECT_NE(ParseChatReply("not json").error.find("not JSON"),
            std::string::npos);
}

// A stand-in for llama-server on a free port of this machine.
class FakeLlamaServer {
 public:
  FakeLlamaServer() : thread_("fake-llama") {
    server_.set_tcp_nodelay(true);
    server_.Post("/v1/chat/completions", [this](const httplib::Request& request,
                                                httplib::Response& response) {
      std::vector<std::string> chunks;
      int tail_delay_ms = 0;
      {
        base::AutoLock lock(lock_);
        last_body_ = request.body;
        client_ports_.insert(request.remote_port);
        ++calls_;
        chunks = stream_chunks_;
        tail_delay_ms = stream_tail_delay_ms_;
      }
      if (delay_ms_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
      }
      response.status = status_;
      if (status_ != 200) {
        response.set_content("oops", "application/json");
        return;
      }
      if (!chunks.empty()) {
        response.set_chunked_content_provider(
            "text/event-stream",
            [chunks, tail_delay_ms](size_t /*offset*/,
                                    httplib::DataSink& sink) {
              for (size_t i = 0; i < chunks.size(); ++i) {
                if (i == 1 && tail_delay_ms > 0) {
                  std::this_thread::sleep_for(
                      std::chrono::milliseconds(tail_delay_ms));
                }
                sink.write(chunks[i].data(), chunks[i].size());
              }
              sink.done();
              return true;
            });
        return;
      }
      response.set_content(kReplyBody, "application/json");
    });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_.Start();
    thread_.task_runner()->PostTask([this] { server_.listen_after_bind(); });
    while (!server_.is_running()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  ~FakeLlamaServer() {
    server_.stop();
    thread_.Stop();
  }

  LlmEndpoint endpoint() const {
    LlmEndpoint endpoint;
    endpoint.port = port_;
    endpoint.reply_timeout_s = 5;
    return endpoint;
  }
  std::string last_body() {
    base::AutoLock lock(lock_);
    return last_body_;
  }
  int calls() {
    base::AutoLock lock(lock_);
    return calls_;
  }
  void set_status(int status) { status_ = status; }
  void set_stream_chunks(std::vector<std::string> chunks,
                         int tail_delay_ms = 0) {
    base::AutoLock lock(lock_);
    stream_chunks_ = std::move(chunks);
    stream_tail_delay_ms_ = tail_delay_ms;
  }
  void set_keep_alive_timeout_s(int seconds) {
    server_.set_keep_alive_timeout(seconds);
  }
  // The client ports the server has seen: one port is one TCP connection.
  size_t distinct_client_ports() {
    base::AutoLock lock(lock_);
    return client_ports_.size();
  }

 private:
  httplib::Server server_;
  base::Thread thread_;
  int port_ = 0;
  base::Lock lock_;
  std::string last_body_ GUARDED_BY(lock_);
  int calls_ GUARDED_BY(lock_) = 0;
  std::flat_set<int> client_ports_ GUARDED_BY(lock_);
  std::vector<std::string> stream_chunks_ GUARDED_BY(lock_);
  int stream_tail_delay_ms_ GUARDED_BY(lock_) = 0;
  int status_ = 200;
  int delay_ms_ = 0;
};

TEST(LlmClientTest, CallsTheServerAndReturnsItsReply) {
  FakeLlamaServer server;
  const LlmResult result = CallLlama(server.endpoint(), SampleRequest());
  ASSERT_TRUE(result.reply) << result.error;
  EXPECT_EQ(result.reply->text, "do: 1\nlook: 30");
  EXPECT_GT(result.round_trip_ms, 0);
  EXPECT_EQ(Json::parse(server.last_body())["id_slot"], 3);
  EXPECT_EQ(server.calls(), 1);
}

TEST(LlmClientTest, AServerErrorIsAFailureWithItsStatus) {
  FakeLlamaServer server;
  server.set_status(500);
  const LlmResult result = CallLlama(server.endpoint(), SampleRequest());
  EXPECT_FALSE(result.reply);
  EXPECT_NE(result.error.find("500"), std::string::npos) << result.error;
}

TEST(LlmClientTest, NobodyListeningIsAFailureNotACrash) {
  LlmEndpoint endpoint;
  {
    // Take a free port, then close it.
    FakeLlamaServer server;
    endpoint = server.endpoint();
  }
  endpoint.connect_timeout_s = 1;
  const LlmResult result = CallLlama(endpoint, SampleRequest());
  EXPECT_FALSE(result.reply);
  EXPECT_NE(result.error.find("could not reach llama-server"),
            std::string::npos)
      << result.error;
}

// Asks and waits for the reply, from a sequence of our own.
std::optional<LlmReply> AskAndWait(LlmClient& client, base::Thread& origin) {
  base::WaitableEvent done;
  std::optional<LlmReply> reply;
  origin.task_runner()->PostTask([&] {
    client.Ask(SampleRequest(), [&](LlmResult result) {
      reply = result.reply;
      done.Signal();
    });
  });
  done.Wait();
  return reply;
}

TEST(LlmClientTest, ConnectionsAreKeptOpenBetweenCalls) {
  FakeLlamaServer server;
  LlmClient client(server.endpoint(), 2);
  base::Thread origin("origin");
  origin.Start();
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(AskAndWait(client, origin));
  }
  EXPECT_EQ(server.calls(), 6);
  EXPECT_EQ(server.distinct_client_ports(), 1u);  // One connection for all.
}

TEST(LlmClientTest, ACallThatFailedDoesNotSpoilTheNext) {
  FakeLlamaServer server;
  auto pool = base::MakeRefCounted<ConnectionPool>(server.endpoint());
  ASSERT_TRUE(pool->Call(SampleRequest()).reply);
  EXPECT_EQ(pool->idle(), 1u);
  server.set_status(500);
  EXPECT_FALSE(pool->Call(SampleRequest()).reply);
  EXPECT_EQ(pool->idle(), 0u);  // The failed connection was closed.
  server.set_status(200);
  EXPECT_TRUE(pool->Call(SampleRequest()).reply);
  EXPECT_EQ(pool->idle(), 1u);
}

TEST(LlmClientTest, AnIdleConnectionTheServerClosedIsOpenedAgain) {
  FakeLlamaServer server;
  server.set_keep_alive_timeout_s(1);  // The server hangs up on idle clients.
  auto pool = base::MakeRefCounted<ConnectionPool>(server.endpoint());
  ASSERT_TRUE(pool->Call(SampleRequest()).reply);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  const LlmResult result = pool->Call(SampleRequest());
  EXPECT_TRUE(result.reply) << result.error;
}

TEST(LlmClientTest, AskRepliesOnTheCallersSequence) {
  FakeLlamaServer server;
  LlmClient client(server.endpoint(), 2);
  base::Thread origin("origin");
  origin.Start();
  base::WaitableEvent done;
  bool on_origin = false;
  std::string text;
  origin.task_runner()->PostTask([&] {
    auto runner = base::SequencedTaskRunner::GetCurrentDefault();
    client.Ask(SampleRequest(), [&, runner](LlmResult result) {
      on_origin = runner->RunsTasksInCurrentSequence();
      text = result.reply ? result.reply->text : result.error;
      done.Signal();
    });
  });
  done.Wait();
  EXPECT_TRUE(on_origin);
  EXPECT_EQ(text, "do: 1\nlook: 30");
}

TEST(LlmClientTest, StreamsSseAndFiresFirstLineBeforeTailFinishes) {
  FakeLlamaServer server;
  // Chunk 0 completes line 1; chunks 1..3 sleep 80 ms, split an SSE line
  // mid-JSON across two writes, and finish with usage/timings + [DONE].
  server.set_stream_chunks(
      {
          "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
          "data: {\"choices\":[{\"delta\":{\"content\":\"do: 1x3\\n\"}}]}\n\n",
          "data: {\"choices\":[{\"delta\":{\"content\":\"expect: none\\n",
          "watch: none\\nlook: 60\"}}]}\n\n"
          "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":812,"
          "\"completion_tokens\":14},\"timings\":{\"prompt_n\":48,"
          "\"prompt_ms\":12.5,\"predicted_ms\":55.0}}\n\n",
          "data: [DONE]\n\n",
      },
      /*tail_delay_ms=*/80);

  LlmClient client(server.endpoint(), 2);
  base::Thread origin("origin");
  origin.Start();

  for (int i = 0; i < 3; ++i) {
    base::WaitableEvent done;
    std::vector<std::string> events;
    LlmResult got;
    bool first_line_on_origin = false;
    origin.task_runner()->PostTask([&] {
      auto runner = base::SequencedTaskRunner::GetCurrentDefault();
      client.Ask(
          SampleRequest(),
          [&](LlmResult result) {
            events.push_back("reply");
            got = std::move(result);
            done.Signal();
          },
          [&, runner](std::string_view line) {
            first_line_on_origin = runner->RunsTasksInCurrentSequence();
            events.push_back("first:" + std::string(line));
          });
    });
    done.Wait();
    ASSERT_TRUE(got.reply) << got.error;
    EXPECT_TRUE(first_line_on_origin);
    EXPECT_EQ(events, (std::vector<std::string>{"first:do: 1x3", "reply"}));
    EXPECT_EQ(got.reply->text, "do: 1x3\nexpect: none\nwatch: none\nlook: 60");
    EXPECT_EQ(got.reply->prompt_tokens, 812);
    EXPECT_EQ(got.reply->completion_tokens, 14);
    EXPECT_EQ(got.reply->prompt_computed, 48);
    EXPECT_DOUBLE_EQ(got.reply->prompt_ms, 12.5);
    EXPECT_DOUBLE_EQ(got.reply->predicted_ms, 55.0);
    EXPECT_GT(got.first_line_ms, 0);
    EXPECT_LT(got.first_line_ms + 40.0, got.round_trip_ms);
  }
  // Keep-alive still reuses one TCP connection across streamed SSE calls.
  EXPECT_EQ(server.distinct_client_ports(), 1u);
}

TEST(LlmClientTest, ATruncatedSseStreamWithoutDoneFailsAndDropsConnection) {
  FakeLlamaServer server;
  server.set_stream_chunks({
      "data: {\"choices\":[{\"delta\":{\"content\":\"do: 1\\n\"}}]}\n\n",
  });
  auto pool = base::MakeRefCounted<ConnectionPool>(server.endpoint());
  const LlmResult bad = pool->Call(SampleRequest());
  EXPECT_FALSE(bad.reply);
  EXPECT_NE(bad.error.find("[DONE]"), std::string::npos) << bad.error;
  EXPECT_EQ(pool->idle(), 0u);
}

}  // namespace
}  // namespace harness
