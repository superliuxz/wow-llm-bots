// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/prompt.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "llm/llm_client.h"
#include "plan/plan.h"
#include "screen/screen_renderer.h"

namespace harness {

namespace {

// Per-slot context window in llama-server (`tools/wlblib/llm.py`: 4,096 tok).
// Estimate conservatively at 3 bytes/token (real English+numbers is ~3.5-4)
// so a capped prompt never overflows the slot.
constexpr size_t kSlotMaxTokens = 4096;
constexpr size_t kTemplateOverheadTokens = 128;
constexpr size_t kMinUserBudgetTokens = 256;
constexpr size_t kBytesPerTokenEstimate = 3;
constexpr size_t kMaxNotesBytes = 2000;

size_t EstimateTokens(std::string_view text) {
  return (text.size() + kBytesPerTokenEstimate - 1) / kBytesPerTokenEstimate;
}

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

std::string CapText(std::string_view text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return std::string(text);
  }
  constexpr std::string_view kSuffix = "\n[truncated]";
  if (max_bytes <= kSuffix.size()) {
    return std::string(text.substr(0, max_bytes));
  }
  return base::StrCat({text.substr(0, max_bytes - kSuffix.size()), kSuffix});
}

// Truncates `user` to `max_bytes` while preserving the trailing `also:` option
// block if present, so a huge screen can never drop the bot's live options.
std::string CapUserPreservingAlso(std::string_view user, size_t max_bytes) {
  if (user.size() <= max_bytes) {
    return std::string(user);
  }
  const size_t also_pos = user.rfind("\nalso:\n");
  if (also_pos != std::string_view::npos) {
    const std::string_view tail = user.substr(also_pos);
    constexpr std::string_view kGap = "\n[truncated]";
    if (tail.size() + kGap.size() + 32 < max_bytes) {
      const size_t head_budget = max_bytes - tail.size() - kGap.size();
      return base::StrCat({user.substr(0, head_budget), kGap, tail});
    }
  }
  return CapText(user, max_bytes);
}

}  // namespace

LlmRequest BuildPlanRequest(const PromptInput& input, const Screen& screen) {
  LlmRequest request;
  request.system = input.rules;

  const size_t reserved_tokens =
      EstimateTokens(request.system) +
      static_cast<size_t>(std::max(0, request.max_tokens)) +
      kTemplateOverheadTokens;
  const size_t user_budget_tokens =
      reserved_tokens < kSlotMaxTokens
          ? std::max(kMinUserBudgetTokens, kSlotMaxTokens - reserved_tokens)
          : kMinUserBudgetTokens;
  const size_t max_user_bytes = user_budget_tokens * kBytesPerTokenEstimate;

  std::string prefix = base::StrCat({"You are ", OneLine(input.bot_name), "."});
  if (!input.identity.empty()) {
    base::StrAppend(&prefix, {"\n\n", input.identity});
  }
  base::StrAppend(&prefix, {"\n\n"});
  if (!input.current_goal.empty()) {
    base::StrAppend(&prefix,
                    {"Current goal: ", OneLine(input.current_goal), ".\n"});
  }
  base::StrAppend(&prefix, {"Woken: ", OneLine(input.wake_reason), "."});
  if (!input.recent_events.empty()) {
    base::StrAppend(&prefix, {"\n\nSince your last look:"});
    for (const std::string& event : input.recent_events) {
      base::StrAppend(&prefix, {"\n- ", OneLine(event)});
    }
  }
  if (!input.active_orders.empty()) {
    base::StrAppend(&prefix, {"\n\nActive standing orders:"});
    for (const std::string& ord : input.active_orders) {
      base::StrAppend(&prefix, {"\n- ", OneLine(ord)});
    }
  }

  const std::string screen_block =
      base::StrCat({"\n\nYour screen now:\n", screen.text});

  // Drop remembered panels first when tight on token budget so they never
  // crowd out the live screen or `also:` options.
  std::string remembered_block;
  if (!input.remembered.empty() &&
      prefix.size() + screen_block.size() < max_user_bytes) {
    const size_t rem_budget =
        max_user_bytes - prefix.size() - screen_block.size();
    constexpr std::string_view kRemHeader =
        "\n\nYou remember (not on screen now):";
    if (rem_budget > kRemHeader.size() + 32) {
      std::string buf(kRemHeader);
      bool added_any = false;
      for (const std::string& line : input.remembered) {
        if (buf.size() + 1 + line.size() > rem_budget) {
          break;
        }
        buf.push_back('\n');
        buf.append(line);
        added_any = true;
      }
      if (added_any) {
        remembered_block = std::move(buf);
      }
    }
  }

  request.user = base::StrCat({prefix, remembered_block, screen_block});

  // Put boss notes after the live screen and only within remaining budget.
  if (!input.notes.empty() && request.user.size() < max_user_bytes) {
    const size_t remaining = max_user_bytes - request.user.size();
    constexpr std::string_view kHeader = "\n\nYour boss notes:\n";
    if (remaining > kHeader.size() + 32) {
      const size_t budget =
          std::min(kMaxNotesBytes, remaining - kHeader.size());
      base::StrAppend(&request.user, {kHeader, CapText(input.notes, budget)});
    }
  }
  if (request.user.size() > max_user_bytes) {
    request.user = CapUserPreservingAlso(request.user, max_user_bytes);
  }
  std::vector<std::string> hover_names;
  for (const Hoverable& h : screen.hoverables) {
    hover_names.push_back(h.name);
  }
  request.grammar =
      PlanGrammar(static_cast<int>(screen.options.size()), input.whisperers,
                  hover_names, input.order_names);
  request.slot = input.slot;
  request.temperature = input.temperature;
  return request;
}

}  // namespace harness
