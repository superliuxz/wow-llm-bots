// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "llm_client.h"

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/str_cat.h"
#include "base/functional/callback.h"
#include "base/task/post_task_and_reply.h"
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

}  // namespace

std::string BuildChatBody(const LlmRequest& request) {
  Json body;
  body["messages"] =
      Json::array({{{"role", "system"}, {"content", request.system}},
                   {{"role", "user"}, {"content", request.user}}});
  body["max_tokens"] = request.max_tokens;
  body["temperature"] = request.temperature;
  body["cache_prompt"] = true;
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
  if (const Json* usage = Find(json, "usage")) {
    reply.prompt_tokens = IntField(*usage, "prompt_tokens");
    reply.completion_tokens = IntField(*usage, "completion_tokens");
  }
  if (const Json* timings = Find(json, "timings")) {
    reply.prompt_computed = IntField(*timings, "prompt_n");
    reply.prompt_ms = DoubleField(*timings, "prompt_ms");
    reply.predicted_ms = DoubleField(*timings, "predicted_ms");
  }
  result.reply = std::move(reply);
  return result;
}

LlmResult CallLlama(const LlmEndpoint& endpoint, const LlmRequest& request) {
  const base::TimeTicks start = base::TimeTicks::Now();
  // httplib's client reports failures as values here; it is third-party code.
  httplib::Client client(endpoint.host, endpoint.port);
  client.set_connection_timeout(endpoint.connect_timeout_s, 0);
  client.set_read_timeout(endpoint.reply_timeout_s, 0);
  client.set_write_timeout(endpoint.reply_timeout_s, 0);
  const httplib::Result response = client.Post(
      "/v1/chat/completions", BuildChatBody(request), "application/json");
  LlmResult result;
  if (!response) {
    result.error =
        absl::StrCat("could not reach llama-server at ", endpoint.host, ":",
                     endpoint.port, ": ", httplib::to_string(response.error()));
  } else if (response->status != 200) {
    result.error = absl::StrCat("llama-server answered ", response->status,
                                ": ", response->body.substr(0, 200));
  } else {
    result = ParseChatReply(response->body);
  }
  result.round_trip_ms =
      (base::TimeTicks::Now() - start).InMicroseconds() / 1000.0;
  return result;
}

LlmClient::LlmClient(LlmEndpoint endpoint, size_t max_in_flight)
    : endpoint_(std::move(endpoint)), pool_("llm-client", max_in_flight) {}

LlmClient::~LlmClient() = default;

void LlmClient::Ask(LlmRequest request,
                    base::OnceCallback<void(LlmResult)> reply) {
  base::PostTaskAndReplyWithResult(
      pool_,
      [endpoint = endpoint_, request = std::move(request)]() {
        return CallLlama(endpoint, request);
      },
      [reply = std::move(reply)](LlmResult result) mutable {
        std::move(reply).Run(std::move(result));
      });
}

}  // namespace harness
