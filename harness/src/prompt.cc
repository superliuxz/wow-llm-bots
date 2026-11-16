// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "prompt.h"

#include <string>

#include "absl/strings/str_cat.h"
#include "llm_client.h"
#include "plan.h"
#include "screen_renderer.h"

namespace harness {

LlmRequest BuildPlanRequest(const PromptInput& input, const Screen& screen) {
  LlmRequest request;
  request.system = absl::StrCat(input.rules, "\nYou are ", input.bot_name, ".");
  if (!input.identity.empty()) {
    absl::StrAppend(&request.system, "\n\n", input.identity);
  }
  request.user = absl::StrCat("Woken: ", input.wake_reason,
                              ".\n\nYour screen now:\n", screen.text);
  request.grammar = PlanGrammar(static_cast<int>(screen.options.size()));
  request.slot = input.slot;
  return request;
}

}  // namespace harness
