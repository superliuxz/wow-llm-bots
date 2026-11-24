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

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "llm/llm_client.h"
#include "screen/screen_renderer.h"

namespace wlb::snapshot {
struct Snapshot;
}  // namespace wlb::snapshot

namespace harness {

struct PromptInput {
  std::string rules;  // content/RULES.md
  std::string bot_name;
  std::string identity;      // var/bots/<name>/identity.md; may be empty
  std::string current_goal;  // Empty when the bot has no goal yet.
  // 1-line breadcrumb of the last action failure/interruption (if any).
  std::string last_outcome;
  std::string wake_reason;  // "plan done", "attacked", ...
  // Events noted since the bot's last look (chat, loot, errors).
  std::vector<std::string> recent_events;
  // Players who recently whispered the bot, newest first: who a `tell` line
  // may answer (PlanGrammar()).
  std::vector<std::string> whisperers;
  // Panels the bot has since closed, as it last saw them, each led by its age
  // ("bags, seen 40 s ago:"): memory, not the screen
  // (docs/specs/options.md).
  std::vector<std::string> remembered;
  // Compact Markdown world/quest/zone guide selected from Atlas+Wiki knowledge.
  std::string knowledge;
  // Boss / encounter notes known to this bot (may be empty).
  std::string notes;
  // Dynamic per-player relationship notes for players currently present or
  // talking to this bot (placed after static notes/guide to preserve KV cache).
  std::string relationships;
  // Standing orders currently in force (formatted strings; may be empty).
  std::vector<std::string> active_orders;
  // Known mechanic, spell, item, and unit names that `order:` lines may use
  // (empty keeps `order:` lines out of the grammar and prompt).
  std::vector<std::string> order_names;
  int slot = -1;  // llama-server slot, or -1
  float temperature = 0.7f;
};

// Selects the current boss's `## <Boss>` section(s) from a multi-boss notes
// file based on `snapshot.ext.encounter->boss` and nearby living non-friendly
// NPCs, or caps at a complete section/line boundary within the notes budget so
// large raid files (ICC, Ulduar) are never cut mid-order and stay in sync with
// `CollectOrderNames`.
std::string SelectRelevantNotes(std::string_view full_notes,
                                const wlb::snapshot::Snapshot& snapshot);

// The request that asks for a plan for this screen. The plan grammar allows
// only the screen's own option numbers.
LlmRequest BuildPlanRequest(const PromptInput& input, const Screen& screen);

// Token/byte breakdown of one assembled plan prompt for diagnostics.
struct PromptBreakdown {
  size_t system_bytes = 0;
  size_t prefix_bytes = 0;
  size_t relationships_bytes = 0;
  size_t remembered_bytes = 0;
  size_t screen_bytes = 0;
  size_t knowledge_bytes = 0;
  size_t notes_bytes = 0;
  size_t user_bytes = 0;
  size_t est_system_tokens = 0;
  size_t est_user_tokens = 0;
  size_t est_total_tokens = 0;
  size_t slot_budget_tokens = 0;
};

PromptBreakdown InspectPrompt(const PromptInput& input, const Screen& screen);
std::string FormatPromptInspection(const PromptInput& input,
                                   const Screen& screen);

}  // namespace harness

#endif  // HARNESS_SCREEN_PROMPT_H_
