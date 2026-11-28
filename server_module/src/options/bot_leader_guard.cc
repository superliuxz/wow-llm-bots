// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/bot_leader_guard.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/string_util.h"
#include "base/time/time.h"

namespace llm_brain {
namespace {

std::vector<std::string> TokenizeLower(std::string_view text) {
  std::vector<std::string> tokens;
  std::string cur;
  for (const char c : text) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) {
      cur.push_back(static_cast<char>(std::tolower(uc)));
    } else if (c == '\'') {
      continue;  // "don't" -> "dont"
    } else if (!cur.empty()) {
      tokens.push_back(std::move(cur));
      cur.clear();
    }
  }
  if (!cur.empty()) {
    tokens.push_back(std::move(cur));
  }
  return tokens;
}

bool HasNegationBefore(const std::vector<std::string>& tokens, size_t idx) {
  constexpr std::string_view kNegations[] = {
      "no", "not", "dont", "do", "never", "stop", "cancel", "without"};
  const size_t start = idx > 3 ? idx - 3 : 0;
  for (size_t i = start; i < idx; ++i) {
    if (tokens[i] == "do") {
      if (i + 1 < idx && tokens[i + 1] == "not") {
        return true;
      }
      continue;
    }
    if (std::find(std::begin(kNegations), std::end(kNegations), tokens[i]) !=
        std::end(kNegations)) {
      return true;
    }
  }
  return false;
}

bool IsInviteTargetFiller(std::string_view w) {
  constexpr std::string_view kFiller[] = {
      "me",      "us",     "him",  "her",   "them",   "someone", "somebody",
      "anyone",  "people", "more", "a",     "an",     "the",     "pls",
      "plz",     "please", "now",  "to",    "group",  "party",   "raid",
      "tank",    "healer", "dps",  "for",   "friend", "my",      "our",
      "another", "other",  "one",  "extra", "fill",
  };
  return std::find(std::begin(kFiller), std::end(kFiller), w) !=
         std::end(kFiller);
}

bool IsFresh(const std::optional<base::TimeTicks>& at, base::TimeTicks now) {
  return at.has_value() && now >= *at &&
         (now - *at) <= kBotLeaderApprovalWindow;
}

}  // namespace

bool RecordHumanLeaderApproval(BotLeaderApprovalState& state,
                               std::string_view text,
                               base::TimeTicks now) {
  const std::vector<std::string> tokens = TokenizeLower(text);
  if (tokens.empty()) {
    return false;
  }
  const BotLeaderApprovalState before = state;

  // 1. LFG / Random Dungeon queue.
  std::optional<size_t> lfg_idx;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i] == "lfg" || tokens[i] == "rdf" || tokens[i] == "queue" ||
        tokens[i] == "requeue") {
      lfg_idx = i;
      break;
    }
    if (i + 1 < tokens.size() &&
        ((tokens[i] == "random" && tokens[i + 1] == "dungeon") ||
         (tokens[i] == "dungeon" && tokens[i + 1] == "finder"))) {
      lfg_idx = i;
      break;
    }
  }
  if (lfg_idx.has_value()) {
    if (HasNegationBefore(tokens, *lfg_idx)) {
      state.lfg_approved_at.reset();
    } else {
      state.lfg_approved_at = now;
    }
  }

  // 2. Ready check.
  std::optional<size_t> rc_idx;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i] == "readycheck" || tokens[i] == "rc") {
      rc_idx = i;
      break;
    }
    if (i + 1 < tokens.size() && tokens[i] == "ready" &&
        tokens[i + 1] == "check") {
      rc_idx = i;
      break;
    }
  }
  if (rc_idx.has_value()) {
    if (HasNegationBefore(tokens, *rc_idx)) {
      state.ready_check_approved_at.reset();
    } else {
      state.ready_check_approved_at = now;
    }
  }

  // 3. Group invite (optional specific target name).
  std::optional<size_t> inv_idx;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i] == "invite" || tokens[i] == "inv") {
      inv_idx = i;
      break;
    }
  }
  if (inv_idx.has_value()) {
    if (HasNegationBefore(tokens, *inv_idx)) {
      state.invite_approved_at.reset();
      state.invite_approved_name.clear();
    } else {
      state.invite_approved_at = now;
      state.invite_approved_name.clear();
      if (*inv_idx + 1 < tokens.size()) {
        const std::string& next = tokens[*inv_idx + 1];
        if (next.size() >= 2 && !IsInviteTargetFiller(next)) {
          state.invite_approved_name = next;
        }
      }
    }
  }

  return state != before;
}

bool CanBotLeaderPerformAction(BotLeaderActionKind kind,
                               bool group_has_human,
                               const BotLeaderApprovalState& approvals,
                               base::TimeTicks now,
                               std::string_view target_name) {
  if (!group_has_human) {
    return true;
  }
  switch (kind) {
    case BotLeaderActionKind::kLfgQueue:
      return IsFresh(approvals.lfg_approved_at, now);
    case BotLeaderActionKind::kReadyCheck:
      return IsFresh(approvals.ready_check_approved_at, now);
    case BotLeaderActionKind::kInvite:
      if (!IsFresh(approvals.invite_approved_at, now)) {
        return false;
      }
      if (approvals.invite_approved_name.empty()) {
        return true;
      }
      return !target_name.empty() &&
             base::EqualsCaseInsensitiveASCII(approvals.invite_approved_name,
                                              target_name);
  }
  return false;
}

void ConsumeBotLeaderApproval(BotLeaderApprovalState& state,
                              BotLeaderActionKind kind) {
  switch (kind) {
    case BotLeaderActionKind::kLfgQueue:
      state.lfg_approved_at.reset();
      break;
    case BotLeaderActionKind::kReadyCheck:
      state.ready_check_approved_at.reset();
      break;
    case BotLeaderActionKind::kInvite:
      state.invite_approved_at.reset();
      state.invite_approved_name.clear();
      break;
  }
}

void ClearBotLeaderApprovals(BotLeaderApprovalState& state) {
  state = {};
}

}  // namespace llm_brain
