// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "prompt.h"

#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
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
  request.system =
      absl::StrCat(input.rules, "\nYou are ", OneLine(input.bot_name), ".");
  if (!input.identity.empty()) {
    absl::StrAppend(&request.system, "\n\n", input.identity);
  }
  request.user = absl::StrCat("Woken: ", OneLine(input.wake_reason),
                              ".\n\nYour screen now:\n", screen.text);
  request.grammar = PlanGrammar(static_cast<int>(screen.options.size()));
  request.slot = input.slot;
  request.temperature = input.temperature;
  return request;
}

}  // namespace harness
