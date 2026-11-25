// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/prompt.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "llm/llm_client.h"
#include "plan/orders.h"
#include "plan/plan.h"
#include "screen/knowledge.h"
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
constexpr size_t kMaxIdentityBytes = 900;
constexpr size_t kMaxMemoryBytes = 900;
constexpr size_t kMaxRecentEventsBytes = 700;
constexpr size_t kMaxActiveOrdersBytes = 700;
constexpr size_t kMaxEventLineBytes = 180;
constexpr size_t kMaxInlineFieldBytes = 200;
constexpr int kDefaultPlanMaxTokens = 150;
constexpr int kOrdersPlanMaxTokens = 350;

size_t EstimateTokens(std::string_view text) {
  return (text.size() + kBytesPerTokenEstimate - 1) / kBytesPerTokenEstimate;
}

// Text from the game or the harness that goes into one line of the prompt: a
// line break or control byte in it could write a fake line ("4. Attack ...").
std::string OneLine(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (unsigned char uc : text) {
    if (uc == '\n' || uc == '\r' || uc == '\t') {
      out.push_back(' ');
    } else if (uc >= 0x20 && uc != 0x7f) {
      out.push_back(static_cast<char>(uc));
    }
  }
  return out;
}

std::string_view TruncateUtf8Safe(std::string_view text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return text;
  }
  size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return text.substr(0, end);
}

std::string CapInline(std::string_view text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return std::string(text);
  }
  constexpr std::string_view kEllipsis = "...";
  if (max_bytes <= kEllipsis.size()) {
    return std::string(TruncateUtf8Safe(text, max_bytes));
  }
  return base::StrCat(
      {TruncateUtf8Safe(text, max_bytes - kEllipsis.size()), kEllipsis});
}

std::string CapText(std::string_view text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return std::string(text);
  }
  constexpr std::string_view kSuffix = "\n[truncated]";
  if (max_bytes <= kSuffix.size()) {
    return std::string(TruncateUtf8Safe(text, max_bytes));
  }
  return base::StrCat(
      {TruncateUtf8Safe(text, max_bytes - kSuffix.size()), kSuffix});
}

// Keeps the newest whole lines of `text` within `max_bytes` (so recent
// `memory.md` entries survive when memory fills).
std::string CapTailLines(std::string_view text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return std::string(text);
  }
  std::vector<std::string_view> lines;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) {
      if (pos < text.size()) {
        lines.push_back(text.substr(pos));
      }
      break;
    }
    lines.push_back(text.substr(pos, nl - pos));
    pos = nl + 1;
  }
  std::vector<std::string_view> kept;
  size_t used = 0;
  for (size_t i = lines.size(); i > 0; --i) {
    const std::string_view l = lines[i - 1];
    const size_t need = l.size() + (kept.empty() ? 0 : 1);
    if (used + need > max_bytes) {
      break;
    }
    kept.push_back(l);
    used += need;
  }
  if (kept.empty()) {
    return CapText(text, max_bytes);
  }
  std::reverse(kept.begin(), kept.end());
  std::string out;
  out.reserve(used);
  for (size_t i = 0; i < kept.size(); ++i) {
    if (i > 0) {
      out.push_back('\n');
    }
    out.append(kept[i]);
  }
  return out;
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

// Truncates `user` to `max_bytes` while keeping ALL numbered option lines (both
// `around me:` and `also:`) visible so the grammar never allows option numbers
// the brain cannot see.
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
    // Pass 1: drop non-option description lines inside the screen body from
    // bottom up (preserving `also:` and every numbered option line).
    for (size_t i = lines.size(); i > 0 && cur_size > max_bytes; --i) {
      const std::string_view l = lines[i - 1];
      if (l == "also:" || IsNumberedOptionLine(l)) {
        continue;
      }
      keep[i - 1] = false;
      cur_size -= l.size() + 1;
    }
    std::string screen_rebuilt;
    bool first = true;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (!keep[i]) {
        continue;
      }
      if (!first) {
        screen_rebuilt.push_back('\n');
      }
      screen_rebuilt.append(lines[i]);
      first = false;
    }
    if (head.size() + screen_rebuilt.size() <= max_bytes) {
      return base::StrCat({head, screen_rebuilt});
    }
    // Pass 2: trim the prefix before `Your screen now:` (keeping line 1
    // "You are <name>.") instead of ever slicing off `around me:` options.
    const size_t first_nl = head.find('\n');
    const std::string_view first_line =
        first_nl == std::string_view::npos ? head : head.substr(0, first_nl);
    const std::string minimal_head = base::StrCat({first_line, kScreenMarker});
    if (minimal_head.size() + screen_rebuilt.size() <= max_bytes) {
      return base::StrCat({minimal_head, screen_rebuilt});
    }
    // Pass 3: even the numbered option lines alone exceed `max_bytes`: shorten
    // long option descriptions proportionally so EVERY option number stays
    // visible in the prompt.
    std::vector<std::string> opt_lines;
    size_t opt_total = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (keep[i]) {
        opt_lines.emplace_back(lines[i]);
        opt_total += lines[i].size() + (opt_lines.size() > 1 ? 1 : 0);
      }
    }
    const size_t body_budget =
        max_bytes > minimal_head.size() ? max_bytes - minimal_head.size() : 0;
    if (body_budget > 0 && !opt_lines.empty() && opt_total > body_budget) {
      const size_t sep_bytes = opt_lines.size() - 1;
      const size_t per_line =
          body_budget > sep_bytes
              ? std::max<size_t>(16,
                                 (body_budget - sep_bytes) / opt_lines.size())
              : 16;
      std::string compact = minimal_head;
      for (size_t i = 0; i < opt_lines.size(); ++i) {
        if (i > 0) {
          compact.push_back('\n');
        }
        compact.append(opt_lines[i].size() > per_line
                           ? CapInline(opt_lines[i], per_line)
                           : opt_lines[i]);
      }
      if (compact.size() <= max_bytes) {
        return compact;
      }
      return CapText(compact, max_bytes);
    }
    return CapText(base::StrCat({minimal_head, screen_rebuilt}), max_bytes);
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

constexpr std::string_view kEarlierSummaryHeading = "## Earlier summary";
constexpr size_t kMaxSummaryBullets = 6;
constexpr size_t kMaxRelationshipsBudgetBytes = 800;

bool IsAsciiLetterChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void AddUniquePlayer(std::vector<std::string>* out,
                     std::string_view bot,
                     std::string_view name) {
  name = base::TrimWhitespaceASCII(name, base::TRIM_ALL);
  if (name.size() < 2 || name.size() > 12 ||
      base::EqualsCaseInsensitiveASCII(name, bot)) {
    return;
  }
  for (char c : name) {
    if (!IsAsciiLetterChar(c)) {
      return;
    }
  }
  for (const std::string& existing : *out) {
    if (base::EqualsCaseInsensitiveASCII(existing, name)) {
      return;
    }
  }
  out->emplace_back(name);
}

}  // namespace

std::string FormatMemoryForPrompt(std::string_view raw_memory,
                                  size_t max_recent) {
  std::vector<std::string> summary;
  std::vector<std::string> recent;
  bool in_summary = false;
  for (std::string_view raw :
       base::SplitStringPiece(raw_memory, "\n", base::TRIM_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    if (raw == kEarlierSummaryHeading) {
      in_summary = true;
      continue;
    }
    if (raw.starts_with("## ")) {
      in_summary = false;
      continue;
    }
    if (in_summary) {
      summary.emplace_back(raw);
    } else {
      recent.emplace_back(raw);
    }
  }
  std::vector<std::string> lines;
  const size_t summary_cap = std::min(summary.size(), kMaxSummaryBullets + 2);
  lines.reserve(summary_cap + std::min(recent.size(), max_recent));
  for (size_t i = 0; i < summary_cap; ++i) {
    lines.push_back(std::move(summary[i]));
  }
  const size_t start =
      recent.size() > max_recent ? recent.size() - max_recent : 0;
  for (size_t i = start; i < recent.size(); ++i) {
    lines.push_back(std::move(recent[i]));
  }
  return base::JoinString(lines, "\n");
}

std::string SelectRelationshipsForPrompt(
    std::string_view raw_relationships,
    const std::vector<std::string>& present_players) {
  if (raw_relationships.empty()) {
    return "";
  }
  struct RelSection {
    std::string player;
    std::vector<std::string> notes;
  };
  std::vector<std::string> preamble;
  std::vector<RelSection> sections;
  RelSection* current = nullptr;
  for (std::string_view raw :
       base::SplitStringPiece(raw_relationships, "\n", base::TRIM_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    if (raw.starts_with("# ")) {
      continue;
    }
    if (raw.starts_with("## ")) {
      const std::string_view name =
          base::TrimWhitespaceASCII(raw.substr(3), base::TRIM_ALL);
      if (name.empty()) {
        current = nullptr;
        continue;
      }
      current = nullptr;
      for (RelSection& sec : sections) {
        if (base::EqualsCaseInsensitiveASCII(sec.player, name)) {
          current = &sec;
          break;
        }
      }
      if (!current) {
        current = &sections.emplace_back(RelSection{std::string(name), {}});
      }
      continue;
    }
    if (current) {
      current->notes.emplace_back(raw);
    } else {
      preamble.emplace_back(raw);
    }
  }

  std::string out;
  auto append_line = [&](std::string_view line) {
    if (line.empty()) {
      return false;
    }
    const size_t sep = out.empty() ? 0 : 1;
    if (out.size() + sep + line.size() > kMaxRelationshipsBudgetBytes) {
      return false;
    }
    if (sep) {
      out.push_back('\n');
    }
    out.append(line);
    return true;
  };
  for (const std::string& line : preamble) {
    if (!append_line(line)) {
      return out;
    }
  }
  std::vector<const RelSection*> matched;
  for (const RelSection& sec : sections) {
    if (sec.notes.empty()) {
      continue;
    }
    const bool relevant =
        std::any_of(present_players.begin(), present_players.end(),
                    [&](const std::string& p) {
                      return base::EqualsCaseInsensitiveASCII(p, sec.player);
                    });
    if (relevant) {
      matched.push_back(&sec);
    }
  }
  for (size_t i = 0; i < matched.size(); ++i) {
    const RelSection& sec = *matched[i];
    const size_t sep = out.empty() ? 0 : 1;
    if (out.size() + sep >= kMaxRelationshipsBudgetBytes) {
      break;
    }
    const size_t remaining = kMaxRelationshipsBudgetBytes - out.size() - sep;
    const size_t players_left = matched.size() - i;
    const size_t share = std::max(remaining / players_left, size_t{1});
    const std::string header = base::StrCat({"## ", sec.player});
    if (header.size() + 1 >= remaining) {
      continue;
    }
    std::vector<std::string_view> picked;
    size_t used = header.size();
    for (auto it = sec.notes.rbegin(); it != sec.notes.rend(); ++it) {
      const size_t next = used + 1 + it->size();
      const size_t limit = picked.empty() ? remaining : share;
      if (next > limit) {
        break;
      }
      picked.push_back(*it);
      used = next;
    }
    if (picked.empty()) {
      continue;
    }
    std::reverse(picked.begin(), picked.end());
    if (sep) {
      out.push_back('\n');
    }
    out.append(header);
    for (std::string_view note : picked) {
      out.push_back('\n');
      out.append(note);
    }
  }
  return out;
}

PromptInput AssemblePromptInput(const PromptAssemblyContext& ctx,
                                const wlb::snapshot::Snapshot& snapshot,
                                const WorldKnowledge* knowledge) {
  PromptInput out;
  out.rules = ctx.rules;
  out.bot_name = !ctx.bot_name.empty()   ? ctx.bot_name
                 : !snapshot.bot.empty() ? snapshot.bot
                                         : "bot";
  out.identity = ctx.identity;
  if (!ctx.raw_memory.empty()) {
    out.memory = FormatMemoryForPrompt(ctx.raw_memory);
  }
  if (!ctx.raw_relationships.empty()) {
    std::vector<std::string> present;
    if (snapshot.ext.party) {
      for (const wlb::snapshot::Unit& m : snapshot.ext.party->members) {
        AddUniquePlayer(&present, out.bot_name, m.name);
      }
    }
    if (snapshot.ext.raid) {
      for (const wlb::snapshot::Unit& m : snapshot.ext.raid->members) {
        AddUniquePlayer(&present, out.bot_name, m.name);
      }
    }
    for (const wlb::snapshot::Unit& u : snapshot.base.nearby.units) {
      if (u.kind == wlb::snapshot::Unit::PLAYER) {
        AddUniquePlayer(&present, out.bot_name, u.name);
      }
    }
    for (const std::string& w : ctx.whisperers) {
      AddUniquePlayer(&present, out.bot_name, w);
    }
    for (const std::string& p : ctx.extra_present_players) {
      AddUniquePlayer(&present, out.bot_name, p);
    }
    for (const wlb::snapshot::Event& ev : snapshot.events) {
      if (ev.chat && !ev.chat->from.empty()) {
        AddUniquePlayer(&present, out.bot_name, ev.chat->from);
      }
    }
    out.relationships =
        SelectRelationshipsForPrompt(ctx.raw_relationships, present);
  }
  out.current_goal = ctx.current_goal;
  out.last_outcome = ctx.last_outcome;
  out.wake_reason = ctx.wake_reason;
  if (ctx.party_reply_muted) {
    base::StrAppend(&out.wake_reason,
                    {" (another party member answers this for the group: act "
                     "on it if it asks something of you, but don't reply)"});
  }
  out.recent_events = ctx.recent_events;
  auto add_whisperer = [&](const std::string& name) {
    if (!name.empty() && name != out.bot_name &&
        std::find(out.whisperers.begin(), out.whisperers.end(), name) ==
            out.whisperers.end()) {
      out.whisperers.push_back(name);
    }
  };
  for (const std::string& w : ctx.whisperers) {
    add_whisperer(w);
  }
  for (const wlb::snapshot::Event& ev : snapshot.events) {
    if (ev.chat && ev.chat->channel == wlb::snapshot::WHISPER) {
      add_whisperer(ev.chat->from);
    }
  }
  if (snapshot.ext.party) {
    for (const wlb::snapshot::Unit& m : snapshot.ext.party->members) {
      add_whisperer(m.name);
    }
  }
  out.remembered = ctx.remembered;
  if (!ctx.raw_notes.empty()) {
    out.notes = SelectRelevantNotes(ctx.raw_notes, snapshot);
  }
  if (knowledge) {
    out.knowledge = knowledge->SelectRelevant(snapshot, !out.notes.empty());
  }
  out.active_orders = ctx.active_orders;
  out.order_names = CollectOrderNames(snapshot, out.notes);
  for (const std::string& extra : ctx.extra_order_names) {
    if (std::find(out.order_names.begin(), out.order_names.end(), extra) ==
        out.order_names.end()) {
      out.order_names.push_back(extra);
    }
  }
  out.slot = ctx.slot;
  out.temperature = ctx.temperature;
  return out;
}

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
  // Give replies room for up to 7 standing `order:` lines on compile wakes
  // (boss notes on screen: RULES only writes boss `order:` lines then) so
  // `max_tokens` does not cut them off (A-L9). `order_names` alone lists every
  // nearby unit, so it would reserve 350 tokens on almost every wake.
  const bool compile_wake = !input.notes.empty() && !input.order_names.empty();
  request.max_tokens =
      compile_wake ? kOrdersPlanMaxTokens : kDefaultPlanMaxTokens;

  const size_t reserved_tokens =
      EstimateTokens(request.system) +
      static_cast<size_t>(std::max(0, request.max_tokens)) +
      kTemplateOverheadTokens;
  const size_t user_budget_tokens =
      reserved_tokens < kSlotMaxTokens
          ? std::max(kMinUserBudgetTokens, kSlotMaxTokens - reserved_tokens)
          : kMinUserBudgetTokens;
  const size_t max_user_bytes = user_budget_tokens * kBytesPerTokenEstimate;

  // Mandatory minimum: bot name, capped wake reason, and the live screen
  // (so numbered options are never pushed out by identity, memory, or events).
  const std::string name_line =
      base::StrCat({"You are ", OneLine(input.bot_name), "."});
  const std::string woken_line = base::StrCat(
      {"Woken: ", CapInline(OneLine(input.wake_reason), kMaxInlineFieldBytes),
       "."});
  const std::string screen_block =
      base::StrCat({"\n\nYour screen now:\n", screen.text});

  const size_t mandatory_bytes =
      name_line.size() + 2 + woken_line.size() + screen_block.size();
  size_t remaining =
      max_user_bytes > mandatory_bytes ? max_user_bytes - mandatory_bytes : 0;

  // Boss notes are filled first: on the pre-pull compile wake they are what
  // the standing orders are written from.
  std::string notes_block;
  if (!input.notes.empty()) {
    constexpr std::string_view kHeader = "\n\nYour boss notes:\n";
    if (remaining > kHeader.size() + 32) {
      const size_t budget =
          std::min(kMaxNotesBytes, remaining - kHeader.size());
      notes_block = base::StrCat({kHeader, CapText(input.notes, budget)});
      remaining -= notes_block.size();
    }
  }

  std::string goal_line;
  if (!input.current_goal.empty()) {
    std::string candidate = base::StrCat(
        {"Current goal: ",
         CapInline(OneLine(input.current_goal), kMaxInlineFieldBytes), ".\n"});
    if (candidate.size() <= remaining) {
      remaining -= candidate.size();
      goal_line = std::move(candidate);
    }
  }

  std::string outcome_line;
  if (!input.last_outcome.empty()) {
    std::string candidate = base::StrCat(
        {"Last outcome: ",
         CapInline(OneLine(input.last_outcome), kMaxInlineFieldBytes), ".\n"});
    if (candidate.size() <= remaining) {
      remaining -= candidate.size();
      outcome_line = std::move(candidate);
    }
  }

  std::string orders_block;
  if (!input.active_orders.empty()) {
    constexpr std::string_view kHeader = "\n\nActive standing orders:";
    const size_t budget = std::min(kMaxActiveOrdersBytes, remaining);
    if (budget > kHeader.size() + 16) {
      std::string buf(kHeader);
      bool added_any = false;
      for (const std::string& ord : input.active_orders) {
        const std::string line = base::StrCat({"\n- ", OneLine(ord)});
        if (buf.size() + line.size() > budget) {
          break;
        }
        buf.append(line);
        added_any = true;
      }
      if (added_any) {
        remaining -= buf.size();
        orders_block = std::move(buf);
      }
    }
  }

  std::string events_block;
  if (!input.recent_events.empty()) {
    constexpr std::string_view kHeader = "\n\nSince your last look:";
    const size_t budget = std::min(kMaxRecentEventsBytes, remaining);
    if (budget > kHeader.size() + 16) {
      std::vector<std::string> kept;
      size_t used = kHeader.size();
      // Prefer the newest events when capped.
      for (size_t i = input.recent_events.size(); i > 0; --i) {
        std::string line =
            base::StrCat({"\n- ", CapInline(OneLine(input.recent_events[i - 1]),
                                            kMaxEventLineBytes)});
        if (used + line.size() > budget) {
          break;
        }
        used += line.size();
        kept.push_back(std::move(line));
      }
      if (!kept.empty()) {
        std::reverse(kept.begin(), kept.end());
        events_block.assign(kHeader);
        for (const std::string& l : kept) {
          events_block.append(l);
        }
        remaining -= events_block.size();
      }
    }
  }

  std::string identity_block;
  if (!input.identity.empty()) {
    constexpr std::string_view kHeader = "\n\n";
    if (remaining > kHeader.size() + 16) {
      const size_t budget =
          std::min(kMaxIdentityBytes, remaining - kHeader.size());
      identity_block = base::StrCat({kHeader, CapText(input.identity, budget)});
      remaining -= identity_block.size();
    }
  }

  std::string memory_block;
  if (!input.memory.empty()) {
    constexpr std::string_view kHeader =
        "\n\nWhat you remember from earlier:\n";
    if (remaining > kHeader.size() + 32) {
      const size_t budget =
          std::min(kMaxMemoryBytes, remaining - kHeader.size());
      memory_block =
          base::StrCat({kHeader, CapTailLines(input.memory, budget)});
      remaining -= memory_block.size();
    }
  }

  std::string knowledge_block;
  if (!input.knowledge.empty()) {
    constexpr std::string_view kHeader = "\n\nWorld guide:\n";
    if (remaining > kHeader.size() + 32) {
      constexpr size_t kMaxKnowledgePromptBytes = 1200;
      const size_t budget =
          std::min(kMaxKnowledgePromptBytes, remaining - kHeader.size());
      knowledge_block =
          base::StrCat({kHeader, CapText(input.knowledge, budget)});
      remaining -= knowledge_block.size();
    }
  }

  std::string relationships_block;
  if (!input.relationships.empty()) {
    constexpr std::string_view kHeader = "\n\nPeople you know:\n";
    if (remaining > kHeader.size() + 32) {
      constexpr size_t kMaxRelationshipsPromptBytes = 900;
      const size_t budget =
          std::min(kMaxRelationshipsPromptBytes, remaining - kHeader.size());
      relationships_block =
          base::StrCat({kHeader, CapText(input.relationships, budget)});
      remaining -= relationships_block.size();
    }
  }

  std::string remembered_block;
  if (!input.remembered.empty()) {
    constexpr std::string_view kRemHeader =
        "\n\nYou remember (not on screen now):";
    if (remaining > kRemHeader.size() + 32) {
      std::string buf(kRemHeader);
      bool added_any = false;
      for (const std::string& line : input.remembered) {
        if (buf.size() + 1 + line.size() > remaining) {
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

  // Static prefix first (`name`, `identity`, `memory`, `notes`, `knowledge`)
  // then dynamic per-wake fields (`goal`, `outcome`, `woken`, `events`,
  // `active_orders`, `relationships`, `remembered`) then live screen for
  // llama.cpp KV-cache prefix reuse.
  const std::string dynamic_prefix =
      base::StrCat({"\n\n", goal_line, outcome_line, woken_line, events_block,
                    orders_block});
  request.user = base::StrCat(
      {name_line, identity_block, memory_block, notes_block, knowledge_block,
       dynamic_prefix, relationships_block, remembered_block, screen_block});
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

namespace {

PromptBreakdown BreakdownOf(const LlmRequest& req) {
  PromptBreakdown b;
  b.system_bytes = req.system.size();
  b.user_bytes = req.user.size();
  b.slot_budget_tokens = kSlotMaxTokens;

  const size_t rem_pos = req.user.find("\n\nYou remember (not on screen now):");
  const size_t rel_pos = req.user.find("\n\nPeople you know:\n");
  const size_t scr_pos = req.user.find("\n\nYour screen now:\n");
  const size_t notes_pos = req.user.find("\n\nYour boss notes:\n");
  const size_t know_pos = req.user.find("\n\nWorld guide:\n");
  const auto find_dyn_start = [&]() -> size_t {
    size_t pos = req.user.size();
    for (std::string_view marker :
         {"\n\nCurrent goal: ", "\n\nLast outcome: ", "\n\nWoken: "}) {
      if (const size_t p = req.user.find(marker); p != std::string::npos) {
        pos = std::min(pos, p);
      }
    }
    return pos;
  };
  const size_t dyn_pos = find_dyn_start();
  auto next_boundary = [&](size_t start) -> size_t {
    size_t end = req.user.size();
    for (size_t p : {notes_pos, know_pos, dyn_pos, rel_pos, rem_pos, scr_pos}) {
      if (p != std::string::npos && p > start) {
        end = std::min(end, p);
      }
    }
    return end;
  };
  if (notes_pos != std::string::npos) {
    b.notes_bytes = next_boundary(notes_pos) - notes_pos;
  }
  if (know_pos != std::string::npos) {
    b.knowledge_bytes = next_boundary(know_pos) - know_pos;
  }
  if (rel_pos != std::string::npos) {
    b.relationships_bytes = next_boundary(rel_pos) - rel_pos;
  }
  if (rem_pos != std::string::npos) {
    b.remembered_bytes = next_boundary(rem_pos) - rem_pos;
  }
  if (scr_pos != std::string::npos) {
    b.screen_bytes = next_boundary(scr_pos) - scr_pos;
  }
  const size_t accounted = b.notes_bytes + b.knowledge_bytes +
                           b.relationships_bytes + b.remembered_bytes +
                           b.screen_bytes;
  b.prefix_bytes =
      req.user.size() > accounted ? req.user.size() - accounted : 0;
  b.est_system_tokens = EstimateTokens(req.system);
  b.est_user_tokens = EstimateTokens(req.user);
  b.est_total_tokens = b.est_system_tokens + b.est_user_tokens;
  return b;
}

}  // namespace

PromptBreakdown InspectPrompt(const PromptInput& input, const Screen& screen) {
  return BreakdownOf(BuildPlanRequest(input, screen));
}

std::string FormatPromptInspection(const PromptInput& input,
                                   const Screen& screen) {
  const LlmRequest req = BuildPlanRequest(input, screen);
  const PromptBreakdown b = BreakdownOf(req);
  return base::StringPrintf(
      "== prompt inspection (%s, %zu options)\n"
      "tokens (est): total=%zu / %zu (system=%zu, user=%zu)\n"
      "bytes: system=%zu, prefix=%zu, relationships=%zu, remembered=%zu, "
      "screen=%zu, knowledge=%zu, notes=%zu, user_total=%zu\n"
      "-- user prompt --\n%s\n",
      input.bot_name.c_str(), screen.options.size(), b.est_total_tokens,
      b.slot_budget_tokens, b.est_system_tokens, b.est_user_tokens,
      b.system_bytes, b.prefix_bytes, b.relationships_bytes, b.remembered_bytes,
      b.screen_bytes, b.knowledge_bytes, b.notes_bytes, b.user_bytes,
      req.user.c_str());
}

}  // namespace harness
