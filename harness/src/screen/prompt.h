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
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llm/llm_client.h"
#include "screen/screen_renderer.h"

namespace wlb::snapshot {
struct Snapshot;
}  // namespace wlb::snapshot

namespace harness {

class WorldKnowledge;

// One chat turn in short-term per-player conversation memory (source-tagged in
// the prompt so player speech can never inject trusted instructions).
struct PlayerChatTurn {
  bool from_bot = false;
  std::string channel;  // e.g. "whisper", "party", "raid", "say"
  std::string text;
};

struct PlayerChatHistory {
  std::string player;
  std::vector<PlayerChatTurn> turns;
};

struct PromptInput {
  std::string rules;  // content/RULES.md
  std::string bot_name;
  std::string identity;      // var/bots/<name>/identity.md; may be empty
  std::string memory;        // var/bots/<name>/memory.md condensed/recent notes
  std::string current_goal;  // Empty when the bot has no goal yet.
  // 1-line breadcrumb of the last action failure/interruption (if any).
  std::string last_outcome;
  // Deterministic roster-derived coordination facts when in combat, during an
  // encounter, or in a raid (M10).
  std::string coordination;
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
  // Short-term per-player chat exchanges for present/active players (rendered
  // with explicit untrusted source-tagging after relationships).
  std::vector<PlayerChatHistory> player_chat_memory;
  // Standing orders currently in force (formatted strings; may be empty).
  std::vector<std::string> active_orders;
  // Known mechanic, spell, item, and unit names that `order:` lines may use
  // (empty keeps `order:` lines out of the grammar and prompt).
  std::vector<std::string> order_names;
  // Who has a minimap ping on the screen (`ping:`): `ping` is offered as a
  // thing in `order:` / `act:` lines only while this isn't empty.
  std::vector<std::string> pingers;
  // Who the bot has a trade window open with while its money > 0: `trade gold`
  // is offered in `act:` lines only while this isn't empty.
  std::vector<std::string> traders;
  // True when a player's chat, minimap ping, or open trade may ask for a
  // one-shot `act:` line; false omits `act:` from the grammar.
  bool allow_act = false;
  int slot = -1;  // llama-server slot, or -1
  float temperature = 0.7f;
};

// Selects the current boss's `## <Boss>` section(s) from a multi-boss notes
// file based on `snapshot.ext.encounter->boss` and nearby living non-friendly
// NPCs, or caps at a complete section/line boundary within the notes budget so
// large raid files (ICC, Ulduar) are never cut mid-order and stay in sync with
// `CollectOrderNames`.
std::string SelectRelevantNotes(std::string_view full_notes,
                                const wlb::snapshot::Snapshot& snapshot,
                                bool* matched_boss = nullptr);

// Merges curated instance/boss notes with optional per-bot personal notes
// (`var/bots/<bot>/notes.md`) within the notes prompt budget (M6). Curated
// boss notes always stay active and take priority; non-empty personal notes
// are appended cleanly into remaining budget rather than silently replacing
// curated notes.
std::string MergeBossNotes(std::string_view curated_notes,
                           std::string_view personal_notes,
                           const wlb::snapshot::Snapshot& snapshot,
                           bool* matched_boss = nullptr);

// Formats raw `memory.md` (optional `## Earlier summary` + newest `max_recent`
// lines) for `PromptInput::memory`.
std::string FormatMemoryForPrompt(std::string_view raw_memory,
                                  size_t max_recent = 12);

// Filters raw `relationships.md` to sections matching `present_players` within
// the relationships prompt budget.
std::string SelectRelationshipsForPrompt(
    std::string_view raw_relationships,
    const std::vector<std::string>& present_players);

// Shared inputs for assembling a live or offline `PromptInput` through one
// code path (`bot_loop.cc` and `decide.cc`).
struct PromptAssemblyContext {
  std::string rules;
  std::string bot_name;
  std::string identity;
  std::string raw_memory;
  std::string raw_relationships;
  std::vector<PlayerChatHistory> player_chat_memory;
  std::string current_goal;
  std::string last_outcome;
  std::string wake_reason;
  bool party_reply_muted = false;
  std::vector<std::string> recent_events;
  std::vector<std::string> whisperers;
  std::vector<std::string> extra_present_players;
  std::vector<std::string> remembered;
  std::string raw_notes;
  std::string raw_personal_notes;
  std::vector<std::string> active_orders;
  std::vector<std::string> extra_order_names;
  int slot = -1;
  float temperature = 0.7f;
};

// Deterministic roster-derived coordination facts for grouped bots (M10).
// Pure geometry and roster indexing (Core Rule 1): every bot in the same group
// computes the same sorted roster and receives a unique spread slot/angle,
// interrupt rotation index (among baseline interrupt-capable classes), and
// balanced soak group ("A" or "B").
struct GroupCoordination {
  int spread_slot = 0;         // 1-based slot index (1..spread_total).
  int spread_total = 0;        // Total members in the group.
  int spread_angle_deg = 0;    // 0..359 deg (0=N, 90=E, 180=S, 270=W).
  std::string spread_compass;  // N, NE, E, SE, S, SW, W, NW.
  int spread_ring = 0;         // 0 for <=8 members; 1 (inner) / 2 (outer) >8.
  int interrupt_order = 0;     // 1-based order among interrupters, or 0.
  int interrupt_total = 0;     // Total interrupt-capable members in the group.
  std::string soak_group;      // "A" or "B".
  uint32_t subgroup = 0;       // 1-based raid subgroup when multi-subgroup.

  bool operator==(const GroupCoordination&) const = default;
};

// Computes deterministic coordination facts for `bot_name` (or `snapshot.bot`)
// from the group roster in `snapshot`. Returns `std::nullopt` when solo.
std::optional<GroupCoordination> ComputeGroupCoordination(
    const wlb::snapshot::Snapshot& snapshot,
    std::string_view bot_name = {});

// Compact single-line summary of `coord` for the brain prompt.
std::string FormatGroupCoordination(const GroupCoordination& coord);

// Assembles a complete `PromptInput` from `ctx` and `snapshot` (memory,
// relationships for present players, party-mute suffix, whisperers + party
// members, relevant boss notes, world guide, and standing-order names).
PromptInput AssemblePromptInput(const PromptAssemblyContext& ctx,
                                const wlb::snapshot::Snapshot& snapshot,
                                const WorldKnowledge* knowledge = nullptr);

// The request that asks for a plan for this screen. The plan grammar allows
// only the screen's own option numbers.
LlmRequest BuildPlanRequest(const PromptInput& input, const Screen& screen);

// Token/byte breakdown of one assembled plan prompt for diagnostics.
struct PromptBreakdown {
  size_t system_bytes = 0;
  size_t prefix_bytes = 0;
  size_t relationships_bytes = 0;
  size_t player_chat_bytes = 0;
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
