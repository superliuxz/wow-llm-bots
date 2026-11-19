// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/prompt.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/strcat.h"
#include "llm/llm_client.h"
#include "plan/plan.h"
#include "screen/screen_renderer.h"

namespace harness {

namespace {

constexpr size_t kMaxNotesBytes = 2000;
constexpr size_t kMaxPromptUserBytes = 12000;

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
  if (!input.active_orders.empty()) {
    base::StrAppend(&request.user, {"\n\nActive standing orders:"});
    for (const std::string& ord : input.active_orders) {
      base::StrAppend(&request.user, {"\n- ", OneLine(ord)});
    }
  }
  if (!input.remembered.empty()) {
    base::StrAppend(&request.user, {"\n\nYou remember (not on screen now):"});
    for (const std::string& line : input.remembered) {
      base::StrAppend(&request.user, {"\n", line});
    }
  }
  // Put the screen before boss notes so notes never crowd out live options, and
  // cap both the notes and the total user prompt size.
  base::StrAppend(&request.user, {"\n\nYour screen now:\n", screen.text});
  if (!input.notes.empty() && request.user.size() < kMaxPromptUserBytes) {
    const size_t remaining = kMaxPromptUserBytes - request.user.size();
    constexpr std::string_view kHeader = "\n\nYour boss notes:\n";
    if (remaining > kHeader.size() + 32) {
      const size_t budget =
          std::min(kMaxNotesBytes, remaining - kHeader.size());
      base::StrAppend(&request.user, {kHeader, CapText(input.notes, budget)});
    }
  }
  if (request.user.size() > kMaxPromptUserBytes) {
    request.user = CapText(request.user, kMaxPromptUserBytes);
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
