// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Builds the question for a bot's brain: the shared rules and the bot's own
// identity as the system prompt, why it was woken and its screen as the user
// message, and the plan grammar for the screen's options. The rules come first
// and are identical for every bot, then the bot's identity, then the changing
// screen: the server can keep the unchanged start of the prompt cached.

#ifndef HARNESS_PROMPT_H_
#define HARNESS_PROMPT_H_

#include <string>

#include "llm_client.h"
#include "screen_renderer.h"

namespace harness {

struct PromptInput {
  std::string rules;  // content/RULES.md
  std::string bot_name;
  std::string identity;     // var/bots/<name>/identity.md; may be empty
  std::string wake_reason;  // "plan done", "attacked", ...
  int slot = -1;            // llama-server slot, or -1
};

// The request that asks for a plan for this screen. The plan grammar allows
// only the screen's own option numbers.
LlmRequest BuildPlanRequest(const PromptInput& input, const Screen& screen);

}  // namespace harness

#endif  // HARNESS_PROMPT_H_
