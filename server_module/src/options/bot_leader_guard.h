// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTIONS_BOT_LEADER_GUARD_H_
#define MOD_LLM_BRAIN_OPTIONS_BOT_LEADER_GUARD_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/time/time.h"

namespace llm_brain {

// Actions a bot group leader may take autonomously in an all-bot group, but
// must not perform without a human group member asking/approving when grouped
// with a human (M7).
enum class BotLeaderActionKind {
  kLfgQueue,
  kReadyCheck,
  kInvite,
};

// How long a human's chat request/approval stays valid for a bot leader.
inline constexpr base::TimeDelta kBotLeaderApprovalWindow = base::Minutes(2);

struct BotLeaderApprovalState {
  std::optional<base::TimeTicks> lfg_approved_at;
  std::optional<base::TimeTicks> ready_check_approved_at;
  std::optional<base::TimeTicks> invite_approved_at;
  // When non-empty, only inviting this player (case-insensitive) is approved.
  std::string invite_approved_name;

  bool operator==(const BotLeaderApprovalState&) const = default;
};

// Inspects a chat line from a human group member and records (or revokes, on
// negated lines like "don't queue" / "no ready check" / "don't invite")
// approval for bot-leader LFG queueing, ready checks, or invites.
// Returns true if `state` changed.
bool RecordHumanLeaderApproval(BotLeaderApprovalState& state,
                               std::string_view text,
                               base::TimeTicks now);

// Returns true when a bot leader is permitted to offer or execute `kind`.
// Always true when `!group_has_human` (solo or all-bot group). When
// `group_has_human` is true, requires an unexpired human approval in
// `approvals` (and for `kInvite`, a matching `target_name` if a specific
// player was named).
bool CanBotLeaderPerformAction(BotLeaderActionKind kind,
                               bool group_has_human,
                               const BotLeaderApprovalState& approvals,
                               base::TimeTicks now,
                               std::string_view target_name = {});

// Consumes a one-shot approval after the action executes.
void ConsumeBotLeaderApproval(BotLeaderApprovalState& state,
                              BotLeaderActionKind kind);

// Unconditionally clears all leader approvals (group leave, map change,
// logout, disconnect).
void ClearBotLeaderApprovals(BotLeaderApprovalState& state);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_BOT_LEADER_GUARD_H_
