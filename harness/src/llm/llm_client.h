// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Asks llama-server a question: one chat request, one reply. llama-server is
// the local model server (tools/wlb llm); this speaks its
// OpenAI-compatible HTTP API. The request carries the plan grammar
// (plan.h), so the model can only write valid plans, and the slot to use, so
// a bot's prompt prefix can stay cached.
//
// CallLlama() blocks and uses a connection of its own. LlmClient runs calls on
// a thread pool, which also bounds how many are in flight, replies on the
// sequence that asked, and keeps connections to llama-server open between
// calls (a new connection for every question is a handshake and a thread
// started on the server's side each time).

#ifndef HARNESS_LLM_LLM_CLIENT_H_
#define HARNESS_LLM_LLM_CLIENT_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "base/thread_annotations.h"

namespace harness {

class LlamaConnection;

struct LlmEndpoint {
  std::string host = "127.0.0.1";
  int port = 8090;
  // How long to wait for the connection, and for the whole reply.
  int connect_timeout_s = 2;
  int reply_timeout_s = 60;
};

struct LlmRequest {
  std::string system;   // The shared rules (identical across bots).
  std::string user;     // Identity, goal, why it was woken, and its screen.
  std::string grammar;  // llama.cpp GBNF; empty: unconstrained.
  int slot = -1;        // llama-server slot; -1: any.
  int max_tokens = 150;
  float temperature = 0.7f;
  bool stream = true;  // Stream SSE so line 1 (`do: ...`) arrives early.
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
  std::optional<LlmReply> reply;  // Empty on failure or cancellation.
  std::string error;              // Why, when it failed.
  bool cancelled = false;         // True when aborted via LlmCancelHandle.
  double first_line_ms = 0;       // When line 1 (`do: ...`) arrived.
  double round_trip_ms = 0;       // As the caller saw the whole call.
};

// Thread-safe handle that aborts an in-flight or queued LLM call from any
// thread (`httplib::Client::stop()` closes the active socket so llama-server
// stops generating on that slot immediately).
class LlmCancelHandle : public base::RefCountedThreadSafe<LlmCancelHandle> {
 public:
  LlmCancelHandle();
  LlmCancelHandle(const LlmCancelHandle&) = delete;
  LlmCancelHandle& operator=(const LlmCancelHandle&) = delete;

  void Cancel();
  bool cancelled() const;

 private:
  friend class base::RefCountedThreadSafe<LlmCancelHandle>;
  friend class LlamaConnection;
  ~LlmCancelHandle();

  bool Attach(LlamaConnection* connection);
  bool Detach();

  mutable base::Lock lock_;
  bool cancelled_ GUARDED_BY(lock_) = false;
  base::raw_ptr<LlamaConnection> active_ GUARDED_BY(lock_);
};

// Invoked once with line 1 (without the trailing '\n') as soon as the first
// newline of a streamed reply arrives.
using FirstLineCallback = base::OnceCallback<void(std::string_view)>;

// The JSON body of a chat request.
std::string BuildChatBody(const LlmRequest& request);

// Reads a non-streamed chat reply body. On failure, `reply` is empty and
// `error` says why.
LlmResult ParseChatReply(std::string_view body);

// A connection to llama-server that stays open between calls (keep-alive). If
// the server closes it while idle, the next call opens it again. One call at a
// time; Stop() may be called from another thread via LlmCancelHandle.
class LlamaConnection : public base::RawPtrTarget {
 public:
  explicit LlamaConnection(const LlmEndpoint& endpoint);
  LlamaConnection(const LlamaConnection&) = delete;
  LlamaConnection& operator=(const LlamaConnection&) = delete;
  ~LlamaConnection();

  LlmResult Call(const LlmRequest& request,
                 FirstLineCallback on_first_line = {},
                 base::scoped_refptr<LlmCancelHandle> cancel = nullptr);
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// One blocking call, on a connection of its own.
LlmResult CallLlama(const LlmEndpoint& endpoint,
                    const LlmRequest& request,
                    FirstLineCallback on_first_line = {},
                    base::scoped_refptr<LlmCancelHandle> cancel = nullptr);

// Idle connections, shared by the worker threads.
class ConnectionPool : public base::RefCountedThreadSafe<ConnectionPool> {
 public:
  explicit ConnectionPool(LlmEndpoint endpoint);

  // One blocking call on an idle connection (or a new one). A connection that
  // failed or was cancelled is closed, not reused: after a timeout or abort it
  // may hold half an answer.
  LlmResult Call(const LlmRequest& request,
                 FirstLineCallback on_first_line = {},
                 base::scoped_refptr<LlmCancelHandle> cancel = nullptr);

  // Aborts every currently active call in the pool.
  void CancelAll();

  // How many connections are open and idle.
  size_t idle() const;

 private:
  friend class base::RefCountedThreadSafe<ConnectionPool>;
  ~ConnectionPool();

  const LlmEndpoint endpoint_;
  mutable base::Lock lock_;
  bool closed_ GUARDED_BY(lock_) = false;
  std::vector<std::unique_ptr<LlamaConnection>> idle_ GUARDED_BY(lock_);
  std::vector<base::scoped_refptr<LlmCancelHandle>> active_ GUARDED_BY(lock_);
};

// Calls on `max_in_flight` worker threads.
class LlmClient {
 public:
  LlmClient(LlmEndpoint endpoint, size_t max_in_flight);
  LlmClient(const LlmClient&) = delete;
  LlmClient& operator=(const LlmClient&) = delete;
  ~LlmClient();  // Cancels and joins calls in progress.

  // Must be called from a task running on a sequence: `on_first_line` (if
  // given) and `reply` both run there, in order. Returns a handle whose
  // Cancel() aborts the call early.
  base::scoped_refptr<LlmCancelHandle> Ask(
      LlmRequest request,
      base::OnceCallback<void(LlmResult)> reply,
      FirstLineCallback on_first_line = {});

 private:
  // Declared before the thread pool, which is destroyed (and joined) first.
  const base::scoped_refptr<ConnectionPool> connections_;
  base::ThreadPool pool_;
};

}  // namespace harness

#endif  // HARNESS_LLM_LLM_CLIENT_H_
