// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Asks llama-server a question: one chat request, one reply. llama-server is
// the local model server (tools/llm-server.sh); this speaks its
// OpenAI-compatible HTTP API. The request carries the plan grammar
// (plan.h), so the model can only write valid plans, and the slot to use, so
// a bot's prompt prefix can stay cached.
//
// CallLlama() blocks. LlmClient runs calls on a thread pool, which also bounds
// how many are in flight, and replies on the sequence that asked.

#ifndef HARNESS_LLM_CLIENT_H_
#define HARNESS_LLM_CLIENT_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/task/thread_pool.h"

namespace harness {

struct LlmEndpoint {
  std::string host = "127.0.0.1";
  int port = 8090;
  // How long to wait for the connection, and for the whole reply.
  int connect_timeout_s = 2;
  int reply_timeout_s = 60;
};

struct LlmRequest {
  std::string system;   // The rules and the bot's identity.
  std::string user;     // Why it was woken, and its screen.
  std::string grammar;  // llama.cpp GBNF; empty: unconstrained.
  int slot = -1;        // llama-server slot; -1: any.
  int max_tokens = 150;
  float temperature = 0.7f;
};

struct LlmReply {
  std::string text;
  int prompt_tokens = 0;      // All of the prompt.
  int prompt_computed = 0;    // The part the server had to process again.
  int completion_tokens = 0;  // What the model wrote.
  double prompt_ms = 0;       // The server's own timings.
  double predicted_ms = 0;
};

struct LlmResult {
  std::optional<LlmReply> reply;  // Empty on failure.
  std::string error;              // Why, when it failed.
  double round_trip_ms = 0;       // As the caller saw it.
};

// The JSON body of a chat request.
std::string BuildChatBody(const LlmRequest& request);

// Reads a chat reply body. On failure, `reply` is empty and `error` says why.
LlmResult ParseChatReply(std::string_view body);

// One blocking call.
LlmResult CallLlama(const LlmEndpoint& endpoint, const LlmRequest& request);

// Calls on `max_in_flight` worker threads.
class LlmClient {
 public:
  LlmClient(LlmEndpoint endpoint, size_t max_in_flight);
  LlmClient(const LlmClient&) = delete;
  LlmClient& operator=(const LlmClient&) = delete;
  ~LlmClient();  // Finishes the calls in progress.

  // Must be called from a task running on a sequence: `reply` runs there. The
  // call waits for a free worker if all are busy.
  void Ask(LlmRequest request, base::OnceCallback<void(LlmResult)> reply);

 private:
  const LlmEndpoint endpoint_;
  base::ThreadPool pool_;
};

}  // namespace harness

#endif  // HARNESS_LLM_CLIENT_H_
