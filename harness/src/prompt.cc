// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "prompt.h"

#include <string>
#include <string_view>

#include "base/strings/strcat.h"
#include "llm_client.h"
#include "plan.h"
#include "screen_renderer.h"

namespace harness {

namespace {

// Text from the game or the harness that goes into one line of the prompt: a
// line break in it could write a fake line ("4. Attack ...").
std::string OneLine(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return out;
}

}  // namespace

LlmRequest BuildPlanRequest(const PromptInput& input, const Screen& screen) {
  LlmRequest request;
  request.system = input.rules;
  request.user = base::StrCat({"You are ", OneLine(input.bot_name), "."});
  if (!input.identity.empty()) {
    base::StrAppend(&request.user, {"\n\n", input.identity});
  }
  base::StrAppend(&request.user, {"\n\n"});
  if (!input.current_goal.empty()) {
    base::StrAppend(&request.user,
                    {"Current goal: ", OneLine(input.current_goal), ".\n"});
  }
  base::StrAppend(&request.user, {"Woken: ", OneLine(input.wake_reason), "."});
  if (!input.recent_events.empty()) {
    base::StrAppend(&request.user, {"\n\nSince your last look:"});
    for (const std::string& event : input.recent_events) {
      base::StrAppend(&request.user, {"\n- ", OneLine(event)});
    }
  }
  base::StrAppend(&request.user, {"\n\nYour screen now:\n", screen.text});
  request.grammar = PlanGrammar(static_cast<int>(screen.options.size()));
  request.slot = input.slot;
  request.temperature = input.temperature;
  return request;
}

}  // namespace harness
