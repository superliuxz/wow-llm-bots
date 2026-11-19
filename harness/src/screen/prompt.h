// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Builds the question for a bot's brain: the shared rules alone as the system
// prompt, then the bot's name, its identity, its current goal, why it was woken
// and its screen as the user message, with the plan grammar for the screen's
// options. Keeping `system` byte-for-byte identical across every bot lets
// llama-server's start-of-user checkpoint cache `RULES.md` across shared slots
// on hybrid recurrent models, while ordering `user` from static (`bot_name`,
// `identity`, `current_goal`) to changing (`wake_reason`, `screen`) reuses KV
// prefix when a bot hits the same slot again.

#ifndef HARNESS_SCREEN_PROMPT_H_
#define HARNESS_SCREEN_PROMPT_H_

#include <string>
#include <vector>

#include "llm/llm_client.h"
#include "screen/screen_renderer.h"

namespace harness {

struct PromptInput {
  std::string rules;  // content/RULES.md
  std::string bot_name;
  std::string identity;      // var/bots/<name>/identity.md; may be empty
  std::string current_goal;  // Empty when the bot has no goal yet.
  std::string wake_reason;   // "plan done", "attacked", ...
  // Events noted since the bot's last look (chat, loot, errors).
  std::vector<std::string> recent_events;
  // Players who recently whispered the bot, newest first: who a `tell` line
  // may answer (PlanGrammar()).
  std::vector<std::string> whisperers;
  // Panels the bot has since closed, as it last saw them, each led by its age
  // ("bags, seen 40 s ago:"): memory, not the screen
  // (docs/plans/options-and-panels.md).
  std::vector<std::string> remembered;
  // Boss / encounter notes known to this bot (may be empty).
  std::string notes;
  // Known mechanic, spell, item, and unit names that `order:` lines may use
  // (empty keeps `order:` lines out of the grammar and prompt).
  std::vector<std::string> order_names;
  int slot = -1;  // llama-server slot, or -1
  float temperature = 0.7f;
};

// The request that asks for a plan for this screen. The plan grammar allows
// only the screen's own option numbers.
LlmRequest BuildPlanRequest(const PromptInput& input, const Screen& screen);

}  // namespace harness

#endif  // HARNESS_SCREEN_PROMPT_H_
