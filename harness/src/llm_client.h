// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Asks llama-server a question: one chat request, one reply. llama-server is
// the local model server (tools/llm-server.sh); this speaks its
// OpenAI-compatible HTTP API. The request carries the plan grammar
// (plan.h), so the model can only write valid plans, and the slot to use, so
// a bot's prompt prefix can stay cached.
//
// CallLlama() blocks and uses a connection of its own. LlmClient runs calls on
// a thread pool, which also bounds how many are in flight, replies on the
// sequence that asked, and keeps connections to llama-server open between
// calls (a new connection for every question is a handshake and a thread
// started on the server's side each time).

#ifndef HARNESS_LLM_CLIENT_H_
#define HARNESS_LLM_CLIENT_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "base/thread_annotations.h"

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

// A connection to llama-server that stays open between calls (keep-alive). If
// the server closes it while idle, the next call opens it again. One call at a
// time; not thread-safe.
class LlamaConnection {
 public:
  explicit LlamaConnection(const LlmEndpoint& endpoint);
  LlamaConnection(const LlamaConnection&) = delete;
  LlamaConnection& operator=(const LlamaConnection&) = delete;
  ~LlamaConnection();

  LlmResult Call(const LlmRequest& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// One blocking call, on a connection of its own.
LlmResult CallLlama(const LlmEndpoint& endpoint, const LlmRequest& request);

// Idle connections, shared by the worker threads.
class ConnectionPool : public base::RefCountedThreadSafe<ConnectionPool> {
 public:
  explicit ConnectionPool(LlmEndpoint endpoint);

  // One blocking call on an idle connection (or a new one). A connection that
  // failed is closed, not reused: after a timeout it may hold half an answer.
  LlmResult Call(const LlmRequest& request);

  // How many connections are open and idle.
  size_t idle() const;

 private:
  friend class base::RefCountedThreadSafe<ConnectionPool>;
  ~ConnectionPool();

  const LlmEndpoint endpoint_;
  mutable base::Lock lock_;
  std::vector<std::unique_ptr<LlamaConnection>> idle_ GUARDED_BY(lock_);
};

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
  // Declared before the thread pool, which is destroyed (and joined) first.
  const base::scoped_refptr<ConnectionPool> connections_;
  base::ThreadPool pool_;
};

}  // namespace harness

#endif  // HARNESS_LLM_CLIENT_H_
