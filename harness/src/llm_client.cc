// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "llm_client.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task/post_task_and_reply.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace harness {
namespace {

using Json = nlohmann::json;

const Json* Find(const Json& json, const char* key) {
  const auto it = json.find(key);
  return it == json.end() ? nullptr : &*it;
}

// An integer field, or 0 when missing or not a number.
int IntField(const Json& json, const char* key) {
  const Json* value = Find(json, key);
  return value && value->is_number() ? value->get<int>() : 0;
}

double DoubleField(const Json& json, const char* key) {
  const Json* value = Find(json, key);
  return value && value->is_number() ? value->get<double>() : 0;
}

void ReadUsageAndTimings(const Json& json, LlmReply* reply) {
  if (const Json* usage = Find(json, "usage"); usage && usage->is_object()) {
    reply->prompt_tokens = IntField(*usage, "prompt_tokens");
    reply->completion_tokens = IntField(*usage, "completion_tokens");
  }
  if (const Json* timings = Find(json, "timings");
      timings && timings->is_object()) {
    reply->prompt_computed = IntField(*timings, "prompt_n");
    reply->prompt_ms = DoubleField(*timings, "prompt_ms");
    reply->predicted_ms = DoubleField(*timings, "predicted_ms");
  }
}

}  // namespace

std::string BuildChatBody(const LlmRequest& request) {
  Json body;
  body["messages"] =
      Json::array({{{"role", "system"}, {"content", request.system}},
                   {{"role", "user"}, {"content", request.user}}});
  body["max_tokens"] = request.max_tokens;
  body["temperature"] = request.temperature;
  body["cache_prompt"] = true;
  if (request.stream) {
    body["stream"] = true;
    body["stream_options"] = {{"include_usage", true}};
  }
  // Our plans are not reasoning: no thinking tokens (ignored by models
  // without a thinking mode).
  body["chat_template_kwargs"] = {{"enable_thinking", false}};
  if (request.slot >= 0) {
    body["id_slot"] = request.slot;
  }
  if (!request.grammar.empty()) {
    body["grammar"] = request.grammar;
  }
  // Text the model saw may not be valid UTF-8 after cuts: replace, don't throw.
  return body.dump(-1, ' ', false, Json::error_handler_t::replace);
}

LlmResult ParseChatReply(std::string_view body) {
  LlmResult result;
  const Json json = Json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (json.is_discarded() || !json.is_object()) {
    result.error = "the reply is not JSON";
    return result;
  }
  const Json* choices = Find(json, "choices");
  if (!choices || !choices->is_array() || choices->empty()) {
    result.error = "the reply has no choices";
    return result;
  }
  const Json* message = Find((*choices)[0], "message");
  const Json* content = message ? Find(*message, "content") : nullptr;
  if (!content || !content->is_string()) {
    result.error = "the reply has no message text";
    return result;
  }
  LlmReply reply;
  reply.text = content->get<std::string>();
  ReadUsageAndTimings(json, &reply);
  result.reply = std::move(reply);
  return result;
}

// httplib's client reports failures as values here; it is third-party code.
struct LlamaConnection::Impl {
  explicit Impl(const LlmEndpoint& endpoint)
      : endpoint(endpoint), client(endpoint.host, endpoint.port) {
    client.set_connection_timeout(endpoint.connect_timeout_s, 0);
    client.set_read_timeout(endpoint.reply_timeout_s, 0);
    client.set_write_timeout(endpoint.reply_timeout_s, 0);
    client.set_keep_alive(true);
    client.set_tcp_nodelay(true);
  }

  const LlmEndpoint endpoint;
  httplib::Client client;
};

LlamaConnection::LlamaConnection(const LlmEndpoint& endpoint)
    : impl_(std::make_unique<Impl>(endpoint)) {}

LlamaConnection::~LlamaConnection() = default;

LlmResult LlamaConnection::Call(const LlmRequest& request,
                                FirstLineCallback on_first_line) {
  const base::TimeTicks start = base::TimeTicks::Now();
  const LlmEndpoint& endpoint = impl_->endpoint;

  int status = 0;
  bool is_event_stream = false;
  std::string raw_body;
  std::string sse_pending;
  LlmReply streamed_reply;
  bool saw_chunk = false;
  bool saw_done = false;
  bool sent_first_line = false;
  double first_line_ms = 0;
  std::string stream_error;

  auto handle_sse_line = [&](std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (!absl::ConsumePrefix(&line, "data:")) {
      return;
    }
    absl::ConsumePrefix(&line, " ");
    if (line == "[DONE]") {
      saw_done = true;
      return;
    }
    if (!stream_error.empty()) {
      return;
    }
    const Json json = Json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object()) {
      stream_error = "an SSE chunk is not JSON";
      return;
    }
    if (const Json* err = Find(json, "error"); err && err->is_object()) {
      const Json* msg = Find(*err, "message");
      stream_error = msg && msg->is_string() ? msg->get<std::string>()
                                             : "llama-server error in stream";
      return;
    }
    saw_chunk = true;
    if (const Json* choices = Find(json, "choices");
        choices && choices->is_array() && !choices->empty()) {
      const Json* delta = Find((*choices)[0], "delta");
      const Json* content = delta ? Find(*delta, "content") : nullptr;
      if (!content) {
        const Json* message = Find((*choices)[0], "message");
        content = message ? Find(*message, "content") : nullptr;
      }
      if (content && content->is_string()) {
        streamed_reply.text.append(content->get_ref<const std::string&>());
      }
    }
    ReadUsageAndTimings(json, &streamed_reply);
    if (!sent_first_line) {
      const size_t nl = streamed_reply.text.find('\n');
      if (nl != std::string::npos) {
        sent_first_line = true;
        first_line_ms =
            (base::TimeTicks::Now() - start).InMicroseconds() / 1000.0;
        if (on_first_line) {
          std::move(on_first_line)
              .Run(std::string_view(streamed_reply.text).substr(0, nl));
        }
      }
    }
  };

  httplib::Request http_req;
  http_req.method = "POST";
  http_req.path = "/v1/chat/completions";
  http_req.body = BuildChatBody(request);
  http_req.set_header("Content-Type", "application/json");
  http_req.response_handler = [&](const httplib::Response& res) {
    status = res.status;
    is_event_stream =
        res.get_header_value("Content-Type").find("text/event-stream") !=
        std::string::npos;
    return true;
  };
  http_req.content_receiver = [&](const char* data, size_t len,
                                  uint64_t /*offset*/, uint64_t /*total*/) {
    if (status != 200) {
      if (raw_body.size() < 200) {
        raw_body.append(data, std::min<size_t>(len, 200 - raw_body.size()));
      }
      return true;
    }
    if (!is_event_stream) {
      raw_body.append(data, len);
      return true;
    }
    sse_pending.append(data, len);
    size_t pos = 0;
    while (true) {
      const size_t nl = sse_pending.find('\n', pos);
      if (nl == std::string::npos) {
        break;
      }
      handle_sse_line(std::string_view(sse_pending).substr(pos, nl - pos));
      pos = nl + 1;
    }
    if (pos > 0) {
      sse_pending.erase(0, pos);
    }
    return true;
  };

  const httplib::Result response = impl_->client.send(http_req);
  const double end_ms =
      (base::TimeTicks::Now() - start).InMicroseconds() / 1000.0;

  LlmResult result;
  if (!response) {
    result.error =
        absl::StrCat("could not reach llama-server at ", endpoint.host, ":",
                     endpoint.port, ": ", httplib::to_string(response.error()));
  } else if (response->status != 200) {
    result.error = absl::StrCat("llama-server answered ", response->status,
                                ": ", raw_body);
  } else if (is_event_stream) {
    if (!sse_pending.empty()) {
      handle_sse_line(sse_pending);
    }
    if (!stream_error.empty()) {
      result.error = stream_error;
    } else if (!saw_chunk || !saw_done) {
      result.error = "the reply stream ended before [DONE]";
    } else {
      if (!sent_first_line) {
        first_line_ms = end_ms;
        if (on_first_line) {
          std::move(on_first_line).Run(streamed_reply.text);
        }
      }
      result.first_line_ms = first_line_ms;
      result.reply = std::move(streamed_reply);
    }
  } else {
    result = ParseChatReply(raw_body);
    if (result.reply) {
      result.first_line_ms = end_ms;
      if (on_first_line) {
        const size_t nl = result.reply->text.find('\n');
        std::move(on_first_line)
            .Run(std::string_view(result.reply->text).substr(0, nl));
      }
    }
  }
  result.round_trip_ms = end_ms;
  return result;
}

LlmResult CallLlama(const LlmEndpoint& endpoint,
                    const LlmRequest& request,
                    FirstLineCallback on_first_line) {
  return LlamaConnection(endpoint).Call(request, std::move(on_first_line));
}

ConnectionPool::ConnectionPool(LlmEndpoint endpoint)
    : endpoint_(std::move(endpoint)) {}

ConnectionPool::~ConnectionPool() = default;

LlmResult ConnectionPool::Call(const LlmRequest& request,
                               FirstLineCallback on_first_line) {
  std::unique_ptr<LlamaConnection> connection;
  {
    base::AutoLock lock(lock_);
    if (!idle_.empty()) {
      connection = std::move(idle_.back());
      idle_.pop_back();
    }
  }
  if (!connection) {
    connection = std::make_unique<LlamaConnection>(endpoint_);
  }
  LlmResult result = connection->Call(request, std::move(on_first_line));
  if (result.reply) {
    base::AutoLock lock(lock_);
    idle_.push_back(std::move(connection));
  }
  return result;
}

size_t ConnectionPool::idle() const {
  base::AutoLock lock(lock_);
  return idle_.size();
}

LlmClient::LlmClient(LlmEndpoint endpoint, size_t max_in_flight)
    : connections_(base::MakeRefCounted<ConnectionPool>(std::move(endpoint))),
      pool_("llm-client", max_in_flight) {}

LlmClient::~LlmClient() = default;

void LlmClient::Ask(LlmRequest request,
                    base::OnceCallback<void(LlmResult)> reply,
                    FirstLineCallback on_first_line) {
  FirstLineCallback posted_first_line;
  if (on_first_line) {
    if (auto origin = base::SequencedTaskRunner::GetCurrentDefault()) {
      posted_first_line =
          [origin = std::move(origin),
           cb = std::move(on_first_line)](std::string_view line) mutable {
            origin->PostTask(
                [cb = std::move(cb), line = std::string(line)]() mutable {
                  std::move(cb).Run(line);
                });
          };
    }
  }
  base::PostTaskAndReplyWithResult(
      pool_,
      [connections = connections_, request = std::move(request),
       posted_first_line = std::move(posted_first_line)]() mutable {
        return connections->Call(request, std::move(posted_first_line));
      },
      [reply = std::move(reply)](LlmResult result) mutable {
        std::move(reply).Run(std::move(result));
      });
}

}  // namespace harness
