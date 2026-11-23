// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/prompt.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "llm/llm_client.h"
#include "plan/plan.h"
#include "screen/screen_renderer.h"
#include "snapshot/snapshot.h"

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

// True when `line` is a numbered option line (e.g. `"  12. Attack ..."`),
// which must stay visible in the prompt so the grammar and screen agree.
bool IsNumberedOptionLine(std::string_view line) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') {
    ++i;
  }
  if (i == 0 || i >= line.size() || line[i] < '0' || line[i] > '9') {
    return false;
  }
  while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
    ++i;
  }
  return i + 1 < line.size() && line[i] == '.' && line[i + 1] == ' ';
}

// Truncates `user` to `max_bytes` by first dropping non-option description
// lines inside `Your screen now:` (keeping numbered option lines and `also:`)
// so a crowded unit list does not hide its last options from the brain while
// they remain in the grammar.
std::string CapUserPreservingAlso(std::string_view user, size_t max_bytes) {
  if (user.size() <= max_bytes) {
    return std::string(user);
  }
  constexpr std::string_view kScreenMarker = "\n\nYour screen now:\n";
  const size_t screen_pos = user.find(kScreenMarker);
  if (screen_pos != std::string_view::npos) {
    const std::string_view head =
        user.substr(0, screen_pos + kScreenMarker.size());
    const std::string_view body =
        user.substr(screen_pos + kScreenMarker.size());
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos <= body.size()) {
      const size_t nl = body.find('\n', pos);
      if (nl == std::string_view::npos) {
        lines.push_back(body.substr(pos));
        break;
      }
      lines.push_back(body.substr(pos, nl - pos));
      pos = nl + 1;
    }
    std::vector<bool> keep(lines.size(), true);
    size_t cur_size = user.size();
    // Drop non-option description lines inside the screen body from bottom up
    // (preserving `also:` and all numbered option lines).
    for (size_t i = lines.size(); i > 0 && cur_size > max_bytes; --i) {
      const std::string_view l = lines[i - 1];
      if (l == "also:" || IsNumberedOptionLine(l)) {
        continue;
      }
      keep[i - 1] = false;
      cur_size -= l.size() + 1;
    }
    std::string rebuilt(head);
    bool first = true;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (!keep[i]) {
        continue;
      }
      if (!first) {
        rebuilt.push_back('\n');
      }
      rebuilt.append(lines[i]);
      first = false;
    }
    if (rebuilt.size() <= max_bytes) {
      return rebuilt;
    }
    user = rebuilt;
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
  return CapText(user, max_bytes);
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return false;
  }
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (base::EqualsCaseInsensitiveASCII(haystack.substr(i, needle.size()),
                                         needle)) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string SelectRelevantNotes(std::string_view full_notes,
                                const wlb::snapshot::Snapshot& snapshot) {
  if (full_notes.empty()) {
    return "";
  }
  // If the entire file fits within the notes budget (39 of 41 dungeon/raid
  // files), keep it intact so companion/shared/instance sections are never
  // dropped.
  if (full_notes.size() <= kMaxNotesBytes) {
    return std::string(full_notes);
  }
  // Parse into optional preamble (`# Instance ...`) + `## <Heading>` sections.
  struct Section {
    std::string_view heading;
    std::string_view body;
  };
  std::string_view preamble;
  std::vector<Section> sections;
  size_t pos = 0;
  size_t cur_start = 0;
  std::string_view cur_heading;
  bool in_section = false;
  while (pos <= full_notes.size()) {
    const size_t nl = full_notes.find('\n', pos);
    const size_t line_end =
        (nl == std::string_view::npos) ? full_notes.size() : nl;
    const std::string_view line = full_notes.substr(pos, line_end - pos);
    if (line.starts_with("## ")) {
      if (in_section) {
        sections.push_back(
            {cur_heading, full_notes.substr(cur_start, pos - cur_start)});
      } else {
        preamble = full_notes.substr(0, pos);
      }
      cur_heading = base::TrimWhitespaceASCII(line.substr(3), base::TRIM_ALL);
      cur_start = pos;
      in_section = true;
    }
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  if (in_section) {
    sections.push_back({cur_heading, full_notes.substr(cur_start)});
  }

  if (sections.empty()) {
    return CapText(full_notes, kMaxNotesBytes);
  }

  // Collect active boss / hostile NPC unit names (plus encounter boss) to match
  // the active boss section(s). Never match friendly players, pets, totems, or
  // short (< 3 char) unit names (including friendly `self.unit.target`).
  std::vector<std::string_view> active_units;
  if (snapshot.ext.encounter && snapshot.ext.encounter->boss &&
      !snapshot.ext.encounter->boss->name.empty()) {
    active_units.push_back(snapshot.ext.encounter->boss->name);
  }
  for (const wlb::snapshot::Unit& u : snapshot.base.nearby.units) {
    if (!u.dead && u.kind == wlb::snapshot::Unit::NPC &&
        u.reaction != wlb::snapshot::Unit::FRIENDLY && u.name.size() >= 3) {
      active_units.push_back(u.name);
    }
  }

  std::vector<const Section*> matched;
  for (const Section& sec : sections) {
    if (sec.heading.empty()) {
      continue;
    }
    bool hit = false;
    for (std::string_view unit_name : active_units) {
      if (base::EqualsCaseInsensitiveASCII(sec.heading, unit_name) ||
          (sec.heading.size() >= 4 && unit_name.size() >= 4 &&
           (ContainsIgnoreCase(unit_name, sec.heading) ||
            ContainsIgnoreCase(sec.heading, unit_name)))) {
        hit = true;
        break;
      }
    }
    if (hit) {
      matched.push_back(&sec);
    }
  }

  if (!matched.empty()) {
    std::string out(preamble);
    bool added_section = false;
    for (const Section* sec : matched) {
      if (out.size() + sec->body.size() > kMaxNotesBytes && added_section) {
        break;
      }
      out.append(sec->body);
      added_section = true;
    }
    if (out.size() > kMaxNotesBytes) {
      return CapText(out, kMaxNotesBytes);
    }
    return out;
  }

  // No specific boss in range yet: stop on a complete `## ` section boundary so
  // we never cut mid-order (and never return only a bare preamble with zero
  // sections).
  std::string out(preamble);
  bool added_section = false;
  for (const Section& sec : sections) {
    if (out.size() + sec.body.size() > kMaxNotesBytes) {
      break;
    }
    out.append(sec.body);
    added_section = true;
  }
  return added_section ? out : CapText(full_notes, kMaxNotesBytes);
}

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
  if (!input.last_outcome.empty()) {
    base::StrAppend(&prefix,
                    {"Last outcome: ", OneLine(input.last_outcome), ".\n"});
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

  // Put world guide and boss notes after the live screen and only within
  // remaining budget so live screen & options are never crowded out.
  if (!input.knowledge.empty() && request.user.size() < max_user_bytes) {
    const size_t remaining = max_user_bytes - request.user.size();
    constexpr std::string_view kHeader = "\n\nWorld guide:\n";
    if (remaining > kHeader.size() + 32) {
      constexpr size_t kMaxKnowledgePromptBytes = 1200;
      const size_t budget =
          std::min(kMaxKnowledgePromptBytes, remaining - kHeader.size());
      base::StrAppend(&request.user,
                      {kHeader, CapText(input.knowledge, budget)});
    }
  }
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

PromptBreakdown InspectPrompt(const PromptInput& input, const Screen& screen) {
  const LlmRequest req = BuildPlanRequest(input, screen);
  PromptBreakdown b;
  b.system_bytes = req.system.size();
  b.user_bytes = req.user.size();
  b.slot_budget_tokens = kSlotMaxTokens;

  const size_t rem_pos = req.user.find("\n\nYou remember (not on screen now):");
  const size_t scr_pos = req.user.find("\n\nYour screen now:\n");
  const size_t know_pos = req.user.find("\n\nWorld guide:\n");
  const size_t notes_pos = req.user.find("\n\nYour boss notes:\n");

  const size_t prefix_end = rem_pos != std::string::npos   ? rem_pos
                            : scr_pos != std::string::npos ? scr_pos
                                                           : req.user.size();
  b.prefix_bytes = prefix_end;
  if (rem_pos != std::string::npos && scr_pos != std::string::npos &&
      scr_pos > rem_pos) {
    b.remembered_bytes = scr_pos - rem_pos;
  }
  if (scr_pos != std::string::npos) {
    const size_t scr_end = know_pos != std::string::npos    ? know_pos
                           : notes_pos != std::string::npos ? notes_pos
                                                            : req.user.size();
    if (scr_end > scr_pos) {
      b.screen_bytes = scr_end - scr_pos;
    }
  }
  if (know_pos != std::string::npos) {
    const size_t know_end =
        notes_pos != std::string::npos ? notes_pos : req.user.size();
    if (know_end > know_pos) {
      b.knowledge_bytes = know_end - know_pos;
    }
  }
  if (notes_pos != std::string::npos && req.user.size() > notes_pos) {
    b.notes_bytes = req.user.size() - notes_pos;
  }
  b.est_system_tokens = EstimateTokens(req.system);
  b.est_user_tokens = EstimateTokens(req.user);
  b.est_total_tokens = b.est_system_tokens + b.est_user_tokens;
  return b;
}

std::string FormatPromptInspection(const PromptInput& input,
                                   const Screen& screen) {
  const LlmRequest req = BuildPlanRequest(input, screen);
  const PromptBreakdown b = InspectPrompt(input, screen);
  return base::StringPrintf(
      "== prompt inspection (%s, %zu options)\n"
      "tokens (est): total=%zu / %zu (system=%zu, user=%zu)\n"
      "bytes: system=%zu, prefix=%zu, remembered=%zu, screen=%zu, "
      "knowledge=%zu, notes=%zu, user_total=%zu\n"
      "-- user prompt --\n%s\n",
      input.bot_name.c_str(), screen.options.size(), b.est_total_tokens,
      b.slot_budget_tokens, b.est_system_tokens, b.est_user_tokens,
      b.system_bytes, b.prefix_bytes, b.remembered_bytes, b.screen_bytes,
      b.knowledge_bytes, b.notes_bytes, b.user_bytes, req.user.c_str());
}

}  // namespace harness
