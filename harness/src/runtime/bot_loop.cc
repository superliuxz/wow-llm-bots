// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "runtime/bot_loop.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <flat_map>
#include <flat_set>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "llm/llm_client.h"
#include "llm/scheduler.h"
#include "plan/order_evaluator.h"
#include "plan/orders.h"
#include "plan/plan.h"
#include "plan/plan_runner.h"
#include "plan/triggers.h"
#include "runtime/decision_log.h"
#include "runtime/file_util.h"
#include "screen/knowledge.h"
#include "screen/prompt.h"
#include "screen/screen_renderer.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

std::optional<double> AgeMsSince(int64_t snapshot_monotonic_us,
                                 base::TimeTicks at) {
  if (snapshot_monotonic_us <= 0) {
    return std::nullopt;
  }
  const base::TimeTicks built =
      base::TimeTicks() + base::Microseconds(snapshot_monotonic_us);
  return (at - built).InMicroseconds() / 1000.0;
}

std::flat_set<std::string> CollectExpectedNames(
    const std::vector<PlanStep>& steps,
    const std::vector<int>& expect,
    const Screen& screen) {
  std::flat_set<std::string> names;
  for (int opt : expect) {
    if (opt >= 1 && static_cast<size_t>(opt) <= screen.details.size()) {
      const std::string& name =
          screen.details[static_cast<size_t>(opt - 1)].name;
      if (!name.empty()) {
        names.insert(name);
      }
    }
  }
  for (const PlanStep& step : steps) {
    if (step.option >= 1 &&
        static_cast<size_t>(step.option) <= screen.details.size()) {
      const ScreenOption& d =
          screen.details[static_cast<size_t>(step.option - 1)];
      if (d.kind == OptionKind::kAttack && !d.name.empty()) {
        names.insert(d.name);
      }
    }
  }
  return names;
}

// True for player-originated chat channels (excludes monster/boss/system lines
// so NPC chatter never enters relationships or player-chat rate limits).
bool IsPlayerChatChannel(pb::ChatChannel ch) {
  switch (ch) {
    case pb::WHISPER:
    case pb::SAY:
    case pb::YELL:
    case pb::PARTY:
    case pb::RAID:
    case pb::RAID_WARNING:
    case pb::GUILD:
    case pb::OFFICER:
    case pb::GENERAL:
    case pb::TRADE:
    case pb::LFG:
    case pb::EMOTE:
    case pb::BATTLEGROUND:
    case pb::CUSTOM_CHANNEL:
      return true;
    default:
      return false;
  }
}

// Sanitizes a `remember:` note before writing to `memory.md` /
// `relationships.md` (A-MH6): strips newlines, control bytes, and leading `#`
// heading syntax; when player chat was present in the wake, rejects prompt-
// injection directives and tags provenance so untrusted chat cannot masquerade
// as bot instructions.
std::string SanitizeRememberNote(
    std::string_view raw,
    const std::vector<std::string>& player_chat_texts) {
  std::string cleaned;
  cleaned.reserve(raw.size());
  for (unsigned char uc : raw) {
    if (uc == '\r' || uc == '\n' || uc == '\t') {
      cleaned.push_back(' ');
    } else if (uc >= 0x20 && uc != 0x7f) {
      cleaned.push_back(static_cast<char>(uc));
    }
  }
  std::string_view view = base::TrimWhitespaceASCII(cleaned, base::TRIM_ALL);
  while (!view.empty() && (view.front() == '#' || view.front() == ' ')) {
    view.remove_prefix(1);
  }
  view = base::TrimWhitespaceASCII(view, base::TRIM_ALL);
  if (view.empty()) {
    return "";
  }
  std::string out(view);
  if (player_chat_texts.empty()) {
    return out;
  }
  const std::string lower = base::ToLowerASCII(out);
  static constexpr std::string_view kInjectionMarkers[] = {
      "remember:",   "ignore previous", "ignore all",   "ignore rules",
      "ignore your", "system prompt",   "system:",      "from now on",
      "override",    "you must",        "always leave", "never heal",
      "always pass", "always decline",  "do:",          "order:",
      "orders:",     "watch:",          "new rule",
  };
  for (std::string_view marker : kInjectionMarkers) {
    if (lower.find(marker) != std::string::npos) {
      return "";
    }
  }
  static constexpr std::string_view kImperativeWords[] = {
      "always ", "never ", "must ", "leave ", "ignore ", "remember",
  };
  for (const std::string& chat : player_chat_texts) {
    const std::string chat_lower = base::ToLowerASCII(chat);
    bool has_imperative = false;
    for (std::string_view w : kImperativeWords) {
      if (chat_lower.find(w) != std::string::npos) {
        has_imperative = true;
        break;
      }
    }
    if (!has_imperative) {
      continue;
    }
    // Reject if the memo parrots a >= 20-byte slice of an imperative player
    // chat line.
    constexpr size_t kSlice = 20;
    for (size_t i = 0; i + kSlice <= lower.size(); i += 4) {
      if (chat_lower.find(std::string_view(lower).substr(i, kSlice)) !=
          std::string::npos) {
        return "";
      }
    }
  }
  return base::StrCat({out, " [heard in chat]"});
}

// Per-sender chat wake rate limit (A-M8): at most 3 chat-triggered LLM wakes
// per sender in a 10 s sliding window (messages still appear in recent_events).
constexpr base::TimeDelta kChatWakeWindow = base::Seconds(10);
constexpr size_t kMaxChatWakesPerSender = 3;

// Structured `memory.md` (F5): optional `## Earlier summary` section followed
// by `## Recent` (or flat lines for legacy/hand-edited files).
constexpr std::string_view kEarlierSummaryHeading = "## Earlier summary";
constexpr std::string_view kRecentMemoryHeading = "## Recent";
constexpr size_t kMaxSummaryBullets = 6;
constexpr size_t kMaxSummaryLineBytes = 140;
constexpr size_t kMaxRelationshipNotesPerPlayer = 8;

struct ParsedMemory {
  std::vector<std::string> summary;
  std::vector<std::string> recent;
};

ParsedMemory ParseMemoryFile(std::string_view text) {
  ParsedMemory out;
  bool in_summary = false;
  for (std::string_view raw : base::SplitStringPiece(
           text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (raw == kEarlierSummaryHeading) {
      in_summary = true;
      continue;
    }
    if (raw.starts_with("## ")) {
      in_summary = false;
      continue;
    }
    if (in_summary) {
      out.summary.emplace_back(raw);
    } else {
      out.recent.emplace_back(raw);
    }
  }
  return out;
}

std::string FormatMemoryFile(const std::vector<std::string>& summary,
                             const std::vector<std::string>& recent) {
  std::string out;
  if (!summary.empty()) {
    base::StrAppend(
        &out, {kEarlierSummaryHeading, "\n", base::JoinString(summary, "\n"),
               "\n\n", kRecentMemoryHeading, "\n"});
    if (!recent.empty()) {
      base::StrAppend(&out, {base::JoinString(recent, "\n"), "\n"});
    }
    return out;
  }
  if (!recent.empty()) {
    base::StrAppend(&out, {kRecentMemoryHeading, "\n",
                           base::JoinString(recent, "\n"), "\n"});
  }
  return out;
}

std::string_view TruncateUtf8(std::string_view text, size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return text;
  }
  size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return text.substr(0, end);
}

std::vector<std::string> ParseCondensedBullets(std::string_view text) {
  std::vector<std::string> bullets;
  for (std::string_view raw : base::SplitStringPiece(
           text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (raw.starts_with("#") || raw.starts_with("```")) {
      continue;
    }
    if (raw.starts_with("- ") || raw.starts_with("* ")) {
      raw.remove_prefix(2);
    } else if (raw.starts_with("• ")) {
      raw.remove_prefix(4);
    }
    raw = base::TrimWhitespaceASCII(raw, base::TRIM_ALL);
    if (raw.empty()) {
      continue;
    }
    std::string line(TruncateUtf8(raw, kMaxSummaryLineBytes));
    for (char& c : line) {
      if (c == '\r' || c == '\n') {
        c = ' ';
      }
    }
    bullets.push_back(base::StrCat({"- ", line}));
    if (bullets.size() >= kMaxSummaryBullets) {
      break;
    }
  }
  return bullets;
}

bool IsAsciiWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

bool IsAsciiLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool ContainsWholeWordIgnoringCase(std::string_view haystack,
                                   std::string_view word) {
  if (word.size() < 2 || haystack.size() < word.size()) {
    return false;
  }
  for (size_t i = 0; i + word.size() <= haystack.size(); ++i) {
    const bool left_ok = i == 0 || !IsAsciiWordChar(haystack[i - 1]);
    const bool right_ok = i + word.size() == haystack.size() ||
                          !IsAsciiWordChar(haystack[i + word.size()]);
    if (left_ok && right_ok &&
        base::EqualsCaseInsensitiveASCII(haystack.substr(i, word.size()),
                                         word)) {
      return true;
    }
  }
  return false;
}

struct RelationshipSection {
  std::string player;
  std::vector<std::string> notes;
};

struct ParsedRelationships {
  std::vector<std::string> preamble;
  std::vector<RelationshipSection> sections;
};

ParsedRelationships ParseRelationshipsFile(std::string_view text) {
  ParsedRelationships out;
  RelationshipSection* current = nullptr;
  for (std::string_view raw : base::SplitStringPiece(
           text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (raw.starts_with("# ")) {
      continue;  // Ignore top-level document title headings.
    }
    if (raw.starts_with("## ")) {
      const std::string_view name =
          base::TrimWhitespaceASCII(raw.substr(3), base::TRIM_ALL);
      if (name.empty()) {
        current = nullptr;
        continue;
      }
      current = nullptr;
      for (RelationshipSection& sec : out.sections) {
        if (base::EqualsCaseInsensitiveASCII(sec.player, name)) {
          current = &sec;
          break;
        }
      }
      if (!current) {
        current = &out.sections.emplace_back(
            RelationshipSection{std::string(name), {}});
      }
      continue;
    }
    if (current) {
      current->notes.emplace_back(raw);
    } else {
      out.preamble.emplace_back(raw);
    }
  }
  return out;
}

std::string FormatRelationshipsFile(const ParsedRelationships& rels) {
  std::string out;
  if (!rels.preamble.empty()) {
    base::StrAppend(&out, {base::JoinString(rels.preamble, "\n"), "\n"});
  }
  for (const RelationshipSection& sec : rels.sections) {
    if (sec.player.empty() || sec.notes.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back('\n');
    }
    base::StrAppend(&out, {"## ", sec.player, "\n",
                           base::JoinString(sec.notes, "\n"), "\n"});
  }
  return out;
}

void AddUniquePlayerName(std::vector<std::string>* out,
                         std::string_view bot,
                         std::string_view name) {
  name = base::TrimWhitespaceASCII(name, base::TRIM_ALL);
  if (name.size() < 2 || name.size() > 12 ||
      base::EqualsCaseInsensitiveASCII(name, bot)) {
    return;
  }
  for (char c : name) {
    if (!IsAsciiLetter(c)) {
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

std::string NormalizeInstanceKey(std::string_view raw) {
  std::string s = base::ToLowerASCII(base::TrimWhitespaceASCII(raw));
  return s;
}

std::string StripLeadingThe(std::string_view key) {
  if (key.starts_with("the ")) {
    return std::string(key.substr(4));
  }
  return std::string(key);
}

// Indexes `<notes_dir>/*/notes.md` by the `# <Instance Name> (<Abbr>)` heading
// on line 1 (plus directory name and `"the "`-stripped alias) so bots entering
// any dungeon/raid in real play load boss notes automatically.
std::flat_map<std::string, std::string> LoadInstanceNotesIndex(
    const std::string& notes_dir) {
  std::flat_map<std::string, std::string> index;
  if (notes_dir.empty()) {
    return index;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(notes_dir, ec)) {
    return index;
  }
  for (const auto& entry : std::filesystem::directory_iterator(notes_dir, ec)) {
    if (ec) {
      break;
    }
    std::error_code entry_ec;
    if (!entry.is_directory(entry_ec) || entry_ec) {
      continue;
    }
    const std::filesystem::path notes_path = entry.path() / "notes.md";
    if (!std::filesystem::is_regular_file(notes_path, entry_ec) || entry_ec) {
      continue;
    }
    const std::optional<std::string> text = ReadFile(notes_path.string());
    if (!text || text->empty()) {
      continue;
    }
    const std::string path_str = notes_path.string();
    const std::string dir_key =
        NormalizeInstanceKey(entry.path().filename().string());
    if (!dir_key.empty()) {
      index.try_emplace(dir_key, path_str);
    }
    const size_t nl = text->find('\n');
    std::string_view first_line = nl == std::string::npos
                                      ? std::string_view(*text)
                                      : std::string_view(*text).substr(0, nl);
    first_line = base::TrimWhitespaceASCII(first_line);
    if (first_line.starts_with("# ")) {
      first_line.remove_prefix(2);
      // Strip trailing ` (<Abbr>)` if present.
      const size_t open_paren = first_line.rfind(" (");
      if (open_paren != std::string_view::npos && first_line.ends_with(")")) {
        first_line = first_line.substr(0, open_paren);
      }
      const std::string title_key = NormalizeInstanceKey(first_line);
      if (!title_key.empty()) {
        index.try_emplace(title_key, path_str);
        const std::string no_the = StripLeadingThe(title_key);
        if (no_the != title_key && !no_the.empty()) {
          index.try_emplace(no_the, path_str);
        }
      }
    }
  }
  return index;
}

}  // namespace

BotLoop::BotLoop(BotLoopConfig config,
                 AsyncAskFn ask,
                 SendCommandFn send_command,
                 LogMessageFn log_message)
    : config_(std::move(config)),
      ask_(std::move(ask)),
      send_command_(std::move(send_command)),
      log_message_(std::move(log_message)),
      decision_log_(config_.bots_dir.empty()
                        ? nullptr
                        : std::make_unique<DecisionLog>(config_.bots_dir)),
      scheduler_(config_.max_in_flight) {
  if (!config_.notes_dir.empty()) {
    instance_notes_index_ = LoadInstanceNotesIndex(config_.notes_dir);
    if (!instance_notes_index_.empty()) {
      Log(base::StringPrintf(
          "harness: indexed %zu instance boss-note keys "
          "from %s",
          instance_notes_index_.size(), config_.notes_dir.c_str()));
    }
  }
  if (!config_.knowledge_path.empty()) {
    std::string err;
    if (std::optional<WorldKnowledge> wk =
            WorldKnowledge::LoadFile(config_.knowledge_path, &err)) {
      knowledge_ = std::move(*wk);
      Log(base::StringPrintf(
          "harness: loaded world knowledge (%zu quests, %zu zones, %zu "
          "dungeons, %zu bosses) from %s",
          knowledge_.quest_count(), knowledge_.zone_count(),
          knowledge_.dungeon_count(), knowledge_.boss_count(),
          config_.knowledge_path.c_str()));
    } else {
      Log(base::StrCat({"harness: world knowledge unavailable (", err, ")"}));
    }
  }
}

BotLoop::~BotLoop() = default;

std::string BotLoop::ReadFileIfChanged(const std::string& path) {
  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(path, ec);
  if (ec) {
    file_cache_.erase(path);
    return "";  // No such file.
  }
  if (auto it = file_cache_.find(path);
      it != file_cache_.end() && it->second.mtime == mtime) {
    return it->second.text;
  }
  std::string text = ReadFile(path).value_or("");
  file_cache_[path] = {mtime, text};
  return text;
}

bool BotLoop::AnswersPartyChat(const std::string& bot,
                               const std::vector<BotState::ObservedChat>& chats,
                               const pb::Snapshot& snapshot) const {
  std::vector<const BotState::ObservedChat*> group_lines;
  for (const BotState::ObservedChat& c : chats) {
    if (c.triggered_wake && (c.channel == pb::PARTY || c.channel == pb::RAID)) {
      group_lines.push_back(&c);
    }
  }
  if (group_lines.empty()) {
    return true;  // Not woken by party chat: nothing to share out.
  }
  // The brain bots in this party (this harness runs them), this one included.
  std::vector<std::string> brain_bots{bot};
  const auto add_members = [&](const std::vector<pb::Unit>& members) {
    for (const pb::Unit& m : members) {
      if (!m.name.empty() && m.name != bot && bots_.contains(m.name)) {
        brain_bots.push_back(m.name);
      }
    }
  };
  if (snapshot.ext.party) {
    add_members(snapshot.ext.party->members);
  }
  if (snapshot.ext.raid) {
    add_members(snapshot.ext.raid->members);
  }
  std::sort(brain_bots.begin(), brain_bots.end());
  brain_bots.erase(std::unique(brain_bots.begin(), brain_bots.end()),
                   brain_bots.end());
  for (const BotState::ObservedChat* line : group_lines) {
    bool named_any = false;
    for (const std::string& name : brain_bots) {
      if (ContainsWholeWordIgnoringCase(line->text, name)) {
        named_any = true;
        if (name == bot) {
          return true;  // Asked by name.
        }
      }
    }
    if (!named_any && brain_bots.front() == bot) {
      return true;  // Nobody named: one bot answers for the group.
    }
  }
  return false;
}

std::string BotLoop::CurrentGoal(const std::string& bot) const {
  const auto it = bots_.find(bot);
  return it == bots_.end() ? "" : it->second.current_goal;
}

void BotLoop::OnConnectionChanged(bool connected) {
  Log(connected ? "harness: connected to mod-llm-brain"
                : "harness: disconnected from mod-llm-brain");
  if (!connected) {
    for (auto&& [name, state] : bots_) {
      state.runner.Stop();
      state.orders.Clear();
      state.latest_snapshot.reset();
      state.look_deadline = base::TimeTicks();
      state.has_ever_woken = false;
      state.was_in_instance = false;
      state.last_map.clear();
      state.notes.clear();
      state.last_selected_notes.clear();
      state.recent_chat_command_ids.clear();
      state.has_queued_wake = false;
      state.queued_urgency = Urgency::kP2Routine;
      state.call_in_flight = false;
      state.in_flight_cancel.reset();
      state.call_deadline = base::TimeTicks();
      state.call_timed_out_epoch = 0;
      state.early_steps_dispatched = false;
      state.early_step_failed = false;
      state.step_refusals = 0;
      state.last_outcome.clear();
      state.order_wakes = 0;
      state.pending_reasons.clear();
      state.pending_chats.clear();
      state.chat_wake_times_by_sender.clear();
      state.recent_events.clear();
      state.whisperers.clear();
      state.condense_state = BotState::CondenseState::kIdle;
      ++state.memory_condense_epoch;
      state.condense_retry_after = base::TimeTicks();
      ResetLooking(&state);
      ++state.call_epoch;
    }
    // Answers to screens of the old connection are useless: free the slots.
    scheduler_.CancelAllInFlight();
  }
}

void BotLoop::NoteEvent(BotState* state,
                        const pb::Event& event,
                        bool triggered_wake) {
  NoteText(state, DescribeEvent(event));
  if (event.chat) {
    const size_t cap = std::max<size_t>(config_.max_recent_events, 1) * 2;
    if (state->pending_chats.size() >= cap) {
      state->pending_chats.erase(state->pending_chats.begin());
    }
    state->pending_chats.push_back({event.chat->channel, event.chat->from,
                                    event.chat->text, event.chat->at_me,
                                    triggered_wake});
  }
}

// static
void BotLoop::ResetLooking(BotState* state) {
  state->pending_look.reset();
  state->glancing.clear();
  state->was_in_combat = false;
  state->last_bag_free_slots = 1;
  state->last_quest_state.reset();
}

void BotLoop::NoteText(BotState* state, std::string text) {
  if (text.empty()) {
    return;
  }
  if (!state->recent_events.empty() && state->recent_events.back() == text) {
    return;
  }
  state->recent_events.push_back(std::move(text));
  while (state->recent_events.size() >
         std::max<size_t>(config_.max_recent_events, 1)) {
    state->recent_events.pop_front();
  }
}

// How long and how many whisperers a bot can answer with `tell`: a player
// answers the last few people who talked to them, not everyone from an hour
// ago.
constexpr base::TimeDelta kWhispererMemory = base::Seconds(600);
constexpr size_t kMaxWhisperers = 4;

// static
void BotLoop::NoteWhisperer(BotState* state,
                            const pb::Event& event,
                            base::TimeTicks now) {
  // Only actual whisperers enter the bounded whisperer list; Say/General/LFG
  // and invite senders must not evict whisperers or let bots whisper General
  // strangers. Current party members are added separately in StartBotCall().
  if (!event.chat || event.chat->channel != pb::WHISPER ||
      event.chat->from.empty()) {
    return;
  }
  std::string from = event.chat->from;
  std::erase_if(state->whisperers,
                [&](const BotState::Whisperer& w) { return w.name == from; });
  state->whisperers.push_front({std::move(from), now});
  if (state->whisperers.size() > kMaxWhisperers) {
    state->whisperers.pop_back();
  }
}

void BotLoop::Requeue(BotState* state,
                      std::vector<std::string> reasons,
                      std::vector<std::string> events,
                      std::vector<BotState::ObservedChat> chats,
                      std::string outcome) {
  for (std::string& r : state->pending_reasons) {
    if (std::find(reasons.begin(), reasons.end(), r) == reasons.end()) {
      reasons.push_back(std::move(r));
    }
  }
  state->pending_reasons = std::move(reasons);
  const size_t chat_cap = std::max<size_t>(config_.max_recent_events, 1) * 2;
  chats.insert(chats.end(),
               std::make_move_iterator(state->pending_chats.begin()),
               std::make_move_iterator(state->pending_chats.end()));
  if (chats.size() > chat_cap) {
    chats.erase(chats.begin(), chats.end() - static_cast<ptrdiff_t>(chat_cap));
  }
  state->pending_chats = std::move(chats);
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    state->recent_events.push_front(std::move(*it));
  }
  while (state->recent_events.size() >
         std::max<size_t>(config_.max_recent_events, 1)) {
    state->recent_events.pop_front();
  }
  if (state->last_outcome.empty() && !outcome.empty()) {
    state->last_outcome = std::move(outcome);
  }
}

void BotLoop::OnSnapshot(pb::Snapshot snapshot, base::TimeTicks received_at) {
  const std::string bot = snapshot.bot;
  if (bot.empty()) {
    return;
  }
  BotState& state = bots_[bot];
  for (const pb::Event& ev : snapshot.events) {
    const bool wakes =
        EvaluateEventTrigger(ev, state.triggers.watches()).has_value();
    NoteEvent(&state, ev, wakes);
    NoteWhisperer(&state, ev, received_at);
  }
  Screen screen = RenderScreen(snapshot);
  const std::optional<WakeTrigger> trig =
      state.triggers.EvaluateSnapshot(snapshot);
  state.latest_snapshot = std::move(snapshot);
  state.latest_screen = std::move(screen);
  state.latest_received_at = received_at;

  const bool look_woke = OnLookSnapshot(bot, &state, received_at);

  // Pre-pull standing-order compilation on instance entry (A4): when a bot
  // enters an instance with boss notes, install cached compiled orders at once
  // or wake urgently out-of-combat to compile them before the pull.
  const bool in_instance = state.latest_snapshot->base.where.in_instance;
  const std::string cur_map = state.latest_snapshot->base.where.map;
  const bool left_or_changed_instance =
      state.was_in_instance && (!in_instance || state.last_map != cur_map);
  if (left_or_changed_instance) {
    if (!in_instance) {
      state.notes.clear();
      state.last_selected_notes.clear();
    }
    if (!state.orders.orders().empty()) {
      if (state.orders.SetOrders({}, received_at)) {
        SendStopMoving(bot);
      }
      Log(base::StrCat(
          {bot, ": cleared standing orders after leaving ", state.last_map}));
    }
  }
  const bool entered_instance =
      in_instance && (!state.was_in_instance || state.last_map != cur_map);
  state.was_in_instance = in_instance;
  state.last_map = cur_map;
  bool pre_pull_woke = false;
  if (entered_instance) {
    state.notes.clear();
    if (!config_.bots_dir.empty()) {
      state.notes =
          ReadFile(base::StrCat({config_.bots_dir, "/", bot, "/notes.md"}))
              .value_or("");
    }
    if (state.notes.empty() && !instance_notes_index_.empty()) {
      const auto lookup = [&](std::string_view raw) -> std::string {
        const std::string key = NormalizeInstanceKey(raw);
        if (key.empty()) {
          return "";
        }
        if (auto it = instance_notes_index_.find(key);
            it != instance_notes_index_.end()) {
          return ReadFile(it->second).value_or("");
        }
        const std::string no_the = StripLeadingThe(key);
        if (no_the != key) {
          if (auto it = instance_notes_index_.find(no_the);
              it != instance_notes_index_.end()) {
            return ReadFile(it->second).value_or("");
          }
        }
        return "";
      };
      state.notes = lookup(cur_map);
      if (state.notes.empty()) {
        state.notes = lookup(state.latest_snapshot->base.where.zone);
      }
    }
    const std::string selected_notes =
        SelectRelevantNotes(state.notes, *state.latest_snapshot);
    if (!selected_notes.empty()) {
      const CompiledOrdersKey key{bot, cur_map, selected_notes};
      if (auto cache_it = compiled_orders_cache_.find(key);
          cache_it != compiled_orders_cache_.end() &&
          !cache_it->second.empty()) {
        state.orders.SetOrders(cache_it->second, received_at);
        Log(base::StrCat({bot, ": pre-pull orders cache hit for ", cur_map,
                          " (", std::to_string(cache_it->second.size()),
                          " orders)"}));
      } else if (!state.latest_snapshot->base.self.in_combat) {
        state.has_ever_woken = true;
        WakeBot(bot, Urgency::kUrgent,
                base::StrCat({"entered instance ", cur_map,
                              ": compile standing orders from boss notes"}));
        pre_pull_woke = true;
      }
    }
  }

  if (!state.has_ever_woken) {
    state.has_ever_woken = true;
    WakeBot(bot, trig ? trig->urgency : Urgency::kNormal,
            trig ? trig->reason : "first look");
    return;
  }
  if (pre_pull_woke) {
    return;
  }
  const uint32_t prev_order_cmd = state.orders.pending_command_id();
  const OrderEvalResult ord =
      state.orders.OnSnapshot(*state.latest_snapshot, received_at);
  if (prev_order_cmd != 0 &&
      state.latest_snapshot->done_act_command_id == prev_order_cmd) {
    state.order_wakes = 0;
    state.order_quiet_since = base::TimeTicks();
  }
  if (!ord.covered && ord.wake_reason.empty() && !ord.act) {
    if (state.order_wakes > 0) {
      if (state.order_quiet_since.is_null()) {
        state.order_quiet_since = received_at;
      } else if (received_at - state.order_quiet_since >= base::Seconds(10)) {
        state.order_wakes = 0;
        state.order_quiet_since = base::TimeTicks();
      }
    }
  } else {
    state.order_quiet_since = base::TimeTicks();
  }
  const bool trig_covered =
      trig && ord.covered && ord.order_index >= 0 &&
      static_cast<size_t>(ord.order_index) < state.orders.orders().size() &&
      OrderCoversWake(
          state.orders.orders()[static_cast<size_t>(ord.order_index)],
          trig->kinds, trig->attackers);
  const std::string_view extra_trig_reason =
      (trig && !trig_covered) ? std::string_view(trig->reason)
                              : std::string_view{};
  const bool order_woke =
      DispatchOrderEval(bot, &state, ord, received_at, extra_trig_reason);
  if (order_woke) {
    return;
  }
  if (trig && !trig_covered) {
    state.runner.Stop();
    state.look_deadline = base::TimeTicks();
    WakeBot(bot, trig->urgency, trig->reason);
    return;
  }
  if (!ord.wake_reason.empty()) {
    return;
  }
  if (look_woke || state.call_in_flight || state.has_queued_wake ||
      ord.covered) {
    return;
  }
  const RunnerAction action = state.runner.OnSnapshot(
      *state.latest_snapshot, state.latest_screen, received_at);
  if (action.step_done) {
    state.step_refusals = 0;
    state.last_outcome.clear();
  }
  DispatchRunnerAction(bot, &state, action);
  if (!state.has_queued_wake && !state.call_in_flight &&
      !state.look_deadline.is_null() && received_at >= state.look_deadline) {
    state.look_deadline = base::TimeTicks();
    state.runner.Stop();
    const bool backoff_wake = !state.pending_reasons.empty();
    WakeBot(bot, Urgency::kNormal, backoff_wake ? "" : "look timer",
            /*refusal=*/backoff_wake);
  }
}

void SetEarlierDeadline(base::TimeTicks* deadline, base::TimeTicks candidate) {
  if (deadline->is_null() || candidate < *deadline) {
    *deadline = candidate;
  }
}

base::TimeDelta StepRefusalBackoff(int refusals) {
  if (refusals <= 1) {
    return base::TimeDelta();
  }
  constexpr int kMaxSeconds = 30;
  const int shift = std::min(refusals - 1, 5);
  return base::Seconds(std::min(1 << shift, kMaxSeconds));
}

void BotLoop::OnEvent(pb::ImmediateEvent imm, base::TimeTicks received_at) {
  const std::string& bot = imm.bot;
  if (bot.empty()) {
    return;
  }
  BotState& state = bots_[bot];
  const bool is_player_chat = imm.event.chat.has_value() &&
                              IsPlayerChatChannel(imm.event.chat->channel) &&
                              imm.event.chat->channel != pb::RAID_WARNING;
  const std::optional<WakeTrigger> chat_trig =
      is_player_chat ? EvaluateEventTrigger(imm.event, state.triggers.watches())
                     : std::nullopt;
  bool chat_rate_limited = false;
  if (is_player_chat && chat_trig && !imm.event.chat->from.empty()) {
    // Per-sender sliding-window rate limit (A-M8): cap spam at 3 LLM wakes
    // per sender per 10 s while still recording the message in recent_events.
    if (state.chat_wake_times_by_sender.size() >= 64) {
      std::erase_if(state.chat_wake_times_by_sender, [&](const auto& kv) {
        return kv.second.empty() ||
               received_at - kv.second.back() > kChatWakeWindow;
      });
    }
    auto& times = state.chat_wake_times_by_sender[imm.event.chat->from];
    while (!times.empty() && received_at - times.front() > kChatWakeWindow) {
      times.pop_front();
    }
    if (times.size() >= kMaxChatWakesPerSender) {
      chat_rate_limited = true;
    } else {
      times.push_back(received_at);
    }
  }
  NoteEvent(&state, imm.event,
            /*triggered_wake=*/chat_trig.has_value() && !chat_rate_limited);
  NoteWhisperer(&state, imm.event, received_at);
  if (chat_rate_limited) {
    return;
  }
  if (imm.event.error && imm.event.error->command_id != 0) {
    if (state.orders.active() &&
        imm.event.error->command_id == state.orders.pending_command_id()) {
      OrderEvalResult failed = state.orders.OnStepFailed(imm.event.error->text);
      state.order_quiet_since = base::TimeTicks();
      // An "act cancelled by new choice" event is our own plan's choice
      // replacing an old movement act (whether it arrives mid-call or after
      // the reply): never report it as a standing-order failure or stop the
      // new plan.
      if (imm.event.error->text == "act cancelled by new choice") {
        return;
      }
      if (state.call_in_flight) {
        if (!failed.wake_reason.empty()) {
          if (std::find(state.pending_reasons.begin(),
                        state.pending_reasons.end(),
                        failed.wake_reason) == state.pending_reasons.end()) {
            state.pending_reasons.push_back(std::move(failed.wake_reason));
          }
          state.order_failure_during_call = true;
        }
        return;
      }
      state.runner.Stop();
      if (!failed.wake_reason.empty()) {
        WakeForOrder(bot, &state, failed.wake_reason, received_at,
                     /*is_failure=*/true);
      }
      return;
    }
    auto chat_it = std::find(state.recent_chat_command_ids.begin(),
                             state.recent_chat_command_ids.end(),
                             imm.event.error->command_id);
    if (chat_it != state.recent_chat_command_ids.end()) {
      state.recent_chat_command_ids.erase(chat_it);
      // Chat error with real command_id: wake the brain so it sees the refusal
      // without stopping the active runner step or freezing step_refusals.
      if (const std::optional<WakeTrigger> trig =
              EvaluateEventTrigger(imm.event, state.triggers.watches())) {
        if (state.latest_snapshot) {
          WakeBot(bot, trig->urgency, trig->reason, /*refusal=*/false);
        } else if (std::find(state.pending_reasons.begin(),
                             state.pending_reasons.end(),
                             trig->reason) == state.pending_reasons.end()) {
          state.pending_reasons.push_back(trig->reason);
        }
      }
      return;
    } else if (!state.runner.active() ||
               imm.event.error->command_id != state.runner.last_command_id()) {
      return;
    } else {
      state.runner.Stop();
      if (!imm.event.error->text.empty()) {
        state.last_outcome = imm.event.error->text;
      }
      if (state.call_in_flight) {
        state.early_step_failed = true;
      }
      const base::TimeDelta backoff = StepRefusalBackoff(++state.step_refusals);
      if (backoff.is_positive()) {
        if (const std::optional<WakeTrigger> trig =
                EvaluateEventTrigger(imm.event, state.triggers.watches());
            trig && std::find(state.pending_reasons.begin(),
                              state.pending_reasons.end(),
                              trig->reason) == state.pending_reasons.end()) {
          state.pending_reasons.push_back(trig->reason);
        }
        SetEarlierDeadline(&state.look_deadline, received_at + backoff);
        Log(base::StringPrintf(
            "harness: %s step refused %d times in a row; "
            "next look in %.0f s",
            bot.c_str(), state.step_refusals, backoff.InSecondsF()));
        return;
      }
    }
  }
  const bool refusal = imm.event.error && imm.event.error->command_id != 0;
  const std::optional<WakeTrigger> trig =
      is_player_chat
          ? chat_trig
          : EvaluateEventTrigger(imm.event, state.triggers.watches());
  if (!trig) {
    return;
  }
  // Player whispers and party/say chat must not abort a running plan mid-pull
  // or clear refusal backoffs (A-M8); the brain wakes and can change course in
  // its next plan if needed.
  if (!is_player_chat) {
    state.runner.Stop();
    state.look_deadline = base::TimeTicks();
  }
  if (state.latest_snapshot) {
    WakeBot(bot, trig->urgency, trig->reason,
            /*preserve_backoffs=*/refusal || is_player_chat);
  } else if (std::find(state.pending_reasons.begin(),
                       state.pending_reasons.end(),
                       trig->reason) == state.pending_reasons.end()) {
    state.pending_reasons.push_back(trig->reason);
  }
}

void BotLoop::OnTick(base::TimeTicks now) {
  for (auto&& [bot, state] : bots_) {
    if (state.latest_snapshot &&
        now - state.latest_received_at > config_.stale_snapshot_after) {
      Log(base::StringPrintf(
          "harness: %s: no snapshot for %.0f s; waiting "
          "for it to come back",
          bot.c_str(), (now - state.latest_received_at).InSecondsF()));
      state.runner.Stop();
      state.orders.Clear();
      state.latest_snapshot.reset();
      state.look_deadline = base::TimeTicks();
      state.has_ever_woken = false;
      state.was_in_instance = false;
      state.last_map.clear();
      state.notes.clear();
      state.last_selected_notes.clear();
      state.recent_chat_command_ids.clear();
      state.has_queued_wake = false;
      state.queued_urgency = Urgency::kP2Routine;
      state.call_in_flight = false;
      if (state.in_flight_cancel) {
        auto cancel = std::move(state.in_flight_cancel);
        cancel->Cancel();
      }
      state.call_deadline = base::TimeTicks();
      state.call_timed_out_epoch = 0;
      state.early_steps_dispatched = false;
      state.early_step_failed = false;
      state.step_refusals = 0;
      state.last_outcome.clear();
      state.order_wakes = 0;
      state.pending_reasons.clear();
      state.pending_chats.clear();
      state.chat_wake_times_by_sender.clear();
      state.recent_events.clear();
      ResetLooking(&state);
      ++state.call_epoch;
      continue;
    }
    // Enforce overall wall-clock deadline on stalled in-flight LLM calls
    // (A-M13) so a wedged llama-server never blocks a bot for 60 s.
    if (state.call_in_flight && !state.call_deadline.is_null() &&
        now >= state.call_deadline) {
      state.call_deadline = base::TimeTicks();
      state.call_timed_out_epoch = state.call_epoch;
      if (state.in_flight_cancel) {
        auto cancel = std::move(state.in_flight_cancel);
        cancel->Cancel();
      }
      continue;
    }
    if (!state.latest_snapshot || state.call_in_flight ||
        state.has_queued_wake) {
      continue;
    }
    if (const OrderEvalResult timeout = state.orders.CheckTimeout(now);
        !timeout.wake_reason.empty()) {
      DispatchOrderEval(bot, &state, timeout, now);
      continue;
    }
    if (const std::optional<std::string> timeout =
            state.runner.CheckTimeout(now)) {
      state.look_deadline = base::TimeTicks();
      WakeBot(bot, Urgency::kNormal, *timeout);
      continue;
    }
    if (!state.look_deadline.is_null() && now >= state.look_deadline) {
      state.look_deadline = base::TimeTicks();
      state.runner.Stop();
      const bool backoff_wake = !state.pending_reasons.empty();
      WakeBot(bot, Urgency::kNormal, backoff_wake ? "" : "look timer",
              /*refusal=*/backoff_wake);
    }
  }
}

void BotLoop::WakeBot(const std::string& bot,
                      Urgency urgency,
                      const std::string& reason,
                      bool preserve_backoffs) {
  BotState& state = bots_[bot];
  if (!preserve_backoffs) {
    state.step_refusals = 0;
    state.order_wakes = 0;
  }
  if (!reason.empty() &&
      std::find(state.pending_reasons.begin(), state.pending_reasons.end(),
                reason) == state.pending_reasons.end()) {
    state.pending_reasons.push_back(reason);
  }
  state.queued_urgency =
      state.has_queued_wake ? std::min(state.queued_urgency, urgency) : urgency;
  state.has_queued_wake = true;
  // Replacing a queued P4 condense job with a real wake returns condensing to
  // idle so it can be re-scheduled after the gameplay reply finishes.
  if (state.condense_state == BotState::CondenseState::kQueued) {
    state.condense_state = BotState::CondenseState::kIdle;
  }
  base::WeakPtr<BotLoop> self = weak_factory_.GetWeakPtr();
  scheduler_.SubmitCancelable(
      bot, urgency, [self, bot](int slot, base::OnceClosure done) {
        if (!self) {
          std::move(done).Run();
          return base::OnceClosure{};
        }
        return self->StartBotCall(bot, slot, std::move(done));
      });
}

base::OnceClosure BotLoop::StartBotCall(const std::string& bot,
                                        int slot,
                                        base::OnceClosure done) {
  auto it = bots_.find(bot);
  if (it == bots_.end() || !it->second.latest_snapshot) {
    if (it != bots_.end()) {
      it->second.has_queued_wake = false;
      it->second.pending_reasons.clear();
    }
    std::move(done).Run();
    return base::OnceClosure{};
  }
  BotState& state = it->second;
  if (!state.identity_loaded) {
    state.identity_loaded = true;
    if (!config_.bots_dir.empty()) {
      if (const std::optional<std::string> id = ReadFile(
              base::StrCat({config_.bots_dir, "/", bot, "/identity.md"}))) {
        state.identity = *id;
      }
    }
    // The shipped identity when the bot has none of its own yet (RH-M4).
    if (state.identity.empty() && !config_.identities_dir.empty()) {
      state.identity =
          ReadFile(base::StrCat({config_.identities_dir, "/", bot, ".md"}))
              .value_or("");
    }
  }
  if (!config_.bots_dir.empty()) {
    const std::string per_bot = ReadFileIfChanged(
        base::StrCat({config_.bots_dir, "/", bot, "/notes.md"}));
    if (!per_bot.empty()) {
      state.notes = per_bot;
    } else if (!state.latest_snapshot->base.where.in_instance) {
      state.notes.clear();
    }
  }

  const Urgency call_urgency = state.queued_urgency;
  state.has_queued_wake = false;
  state.queued_urgency = Urgency::kP2Routine;
  state.call_in_flight = true;
  state.early_steps_dispatched = false;
  state.early_step_failed = false;
  state.order_failure_during_call = false;
  const uint64_t epoch = ++state.call_epoch;

  const std::string wake_reason =
      state.pending_reasons.empty()
          ? "plan done"
          : base::JoinString(state.pending_reasons, "; ");
  std::vector<std::string> asked_reasons = std::move(state.pending_reasons);
  state.pending_reasons.clear();
  std::vector<BotState::ObservedChat> asked_chats =
      std::move(state.pending_chats);
  state.pending_chats.clear();

  const base::TimeTicks asked_at_for_whisperers = base::TimeTicks::Now();
  PromptAssemblyContext ctx;
  ctx.rules = config_.rules;
  ctx.bot_name = bot;
  ctx.identity = state.identity;
  if (!config_.bots_dir.empty()) {
    ctx.raw_memory = ReadFileIfChanged(
        base::StrCat({config_.bots_dir, "/", bot, "/memory.md"}));
    ctx.raw_relationships = ReadFileIfChanged(
        base::StrCat({config_.bots_dir, "/", bot, "/relationships.md"}));
    for (const BotState::ObservedChat& c : asked_chats) {
      if (IsPlayerChatChannel(c.channel)) {
        ctx.extra_present_players.push_back(c.from);
      }
    }
  }
  ctx.current_goal = state.current_goal;
  ctx.wake_reason = wake_reason;
  if (!AnswersPartyChat(bot, asked_chats, *state.latest_snapshot)) {
    state.party_reply_muted_epoch = epoch;
    ctx.party_reply_muted = true;
  }
  ctx.recent_events.assign(state.recent_events.begin(),
                           state.recent_events.end());
  state.recent_events.clear();
  std::vector<std::string> asked_events = ctx.recent_events;
  for (const BotState::Whisperer& w : state.whisperers) {
    if (asked_at_for_whisperers - w.at <= kWhispererMemory) {
      ctx.whisperers.push_back(w.name);
    }
  }
  // Ages on the snapshots' clock (seen_at is a snapshot's received time).
  ctx.remembered = RememberedPanels(state, state.latest_received_at,
                                    config_.remember_panels_for);
  // Consume `last_outcome` once delivered so it never goes stale across
  // unrelated future wakes (requeued on cancel/error via `asked_outcome`).
  std::string asked_outcome = std::move(state.last_outcome);
  state.last_outcome.clear();
  ctx.last_outcome = asked_outcome;
  ctx.raw_notes = state.notes;
  for (const Order& o : state.orders.orders()) {
    ctx.active_orders.push_back(FormatOrder(o));
  }
  ctx.slot = slot;
  ctx.temperature = config_.temperature;

  const PromptInput input =
      AssemblePromptInput(ctx, *state.latest_snapshot, &knowledge_);
  state.last_selected_notes = input.notes;
  std::vector<std::string> order_names = input.order_names;

  const uint32_t ask_seq = state.latest_snapshot->seq;
  state.ask_watch_inputs = TriggerEvaluator::InputsOf(*state.latest_snapshot);
  const int64_t snap_mono_us = state.latest_snapshot->monotonic_us;
  const Screen ask_screen = state.latest_screen;
  const base::TimeTicks asked_at = base::TimeTicks::Now();
  const std::optional<double> age_at_ask_ms =
      AgeMsSince(snap_mono_us, asked_at);

  LlmRequest request = BuildPlanRequest(input, ask_screen);
  const base::TimeDelta call_limit = (call_urgency == Urgency::kP0Combat)
                                         ? config_.combat_call_timeout
                                         : config_.call_timeout;
  if (call_limit.is_positive()) {
    request.timeout_ms = static_cast<int>(call_limit.InMilliseconds());
    const base::TimeTicks call_start =
        std::max(asked_at, state.latest_received_at);
    state.call_deadline = call_start + call_limit;
  } else {
    state.call_deadline = base::TimeTicks();
  }
  LlmRequest request_copy = request;

  // Shared strictly on this single sequence between OnFirstLine and OnReply.
  auto first_line_age_holder = std::make_unique<double>(0.0);
  double* first_line_age_ptr = first_line_age_holder.get();

  base::WeakPtr<BotLoop> self = weak_factory_.GetWeakPtr();
  FirstLineCallback on_first_line =
      [self, bot, epoch, ask_seq, ask_screen, snap_mono_us,
       first_line_age_ptr](std::string_view line) {
        if (self) {
          self->OnFirstLine(bot, epoch, ask_seq, ask_screen, snap_mono_us,
                            first_line_age_ptr, line);
        }
      };

  base::scoped_refptr<LlmCancelHandle> handle = ask_.Run(
      std::move(request),
      [self, bot, epoch, ask_seq, ask_screen,
       order_names = std::move(order_names), wake_reason,
       asked_reasons = std::move(asked_reasons),
       asked_events = std::move(asked_events),
       asked_chats = std::move(asked_chats),
       asked_outcome = std::move(asked_outcome),
       request_copy = std::move(request_copy), snap_mono_us, age_at_ask_ms,
       holder = std::move(first_line_age_holder),
       done = std::move(done)](LlmResult result) mutable {
        if (!self) {
          std::move(done).Run();
          return;
        }
        self->OnReply(bot, epoch, ask_seq, std::move(ask_screen),
                      std::move(order_names), std::move(wake_reason),
                      std::move(asked_reasons), std::move(asked_events),
                      std::move(asked_chats), std::move(asked_outcome),
                      std::move(request_copy), snap_mono_us, age_at_ask_ms,
                      *holder, std::move(done), std::move(result));
      },
      std::move(on_first_line));
  state.in_flight_cancel = handle;
  if (!handle) {
    return base::OnceClosure{};
  }
  // Cancel() is thread-safe and never runs the reply inline: the reply (with
  // `cancelled` set) arrives later on this sequence and releases the slot.
  return [handle = std::move(handle)]() { handle->Cancel(); };
}

void BotLoop::OnFirstLine(const std::string& bot,
                          uint64_t epoch,
                          uint32_t ask_seq,
                          const Screen& ask_screen,
                          int64_t snapshot_monotonic_us,
                          double* out_age_at_first_line_ms,
                          std::string_view line) {
  if (const auto age =
          AgeMsSince(snapshot_monotonic_us, base::TimeTicks::Now())) {
    *out_age_at_first_line_ms = *age;
  }
  auto it = bots_.find(bot);
  if (it == bots_.end()) {
    return;
  }
  BotState& state = it->second;
  if (!state.call_in_flight || state.call_epoch != epoch ||
      state.has_queued_wake || !state.latest_snapshot) {
    return;
  }
  const std::optional<std::vector<PlanStep>> steps =
      ParsePlanStepsLine(line, static_cast<int>(ask_screen.options.size()));
  if (!steps) {
    return;
  }
  state.early_steps_dispatched = true;
  // Provisionally mark attacked targets as expected while the rest of the plan
  // streams, so entering combat before `expect:` arrives does not re-wake.
  state.triggers.SetExpectedNames(CollectExpectedNames(*steps, {}, ask_screen));
  const RunnerAction action =
      state.runner.Start(*steps, ask_seq, ask_screen, base::TimeTicks::Now());
  DispatchRunnerAction(bot, &state, action);
}

void BotLoop::OnReply(const std::string& bot,
                      uint64_t epoch,
                      uint32_t ask_seq,
                      Screen ask_screen,
                      std::vector<std::string> order_names,
                      std::string wake_reason,
                      std::vector<std::string> asked_reasons,
                      std::vector<std::string> asked_events,
                      std::vector<BotState::ObservedChat> asked_chats,
                      std::string asked_outcome,
                      LlmRequest request,
                      int64_t snapshot_monotonic_us,
                      std::optional<double> age_at_ask_ms,
                      double age_at_first_line_ms,
                      base::OnceClosure done,
                      LlmResult result) {
  const base::TimeTicks now = base::TimeTicks::Now();
  if (result.cancelled) {
    // Preempted by a more urgent wake, timed out on tick (A-M13), or the module
    // disconnected.
    bool was_timeout = false;
    if (auto it = bots_.find(bot);
        it != bots_.end() && it->second.call_epoch == epoch) {
      it->second.call_in_flight = false;
      it->second.in_flight_cancel.reset();
      it->second.call_deadline = base::TimeTicks();
      was_timeout = it->second.call_timed_out_epoch == epoch;
      Requeue(&it->second, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_chats), std::move(asked_outcome));
      if (was_timeout && !it->second.has_queued_wake) {
        it->second.look_deadline = now + base::Seconds(2);
      }
    }
    ++calls_cancelled_;
    Log(base::StringPrintf("harness: %s call %s after %.0f ms", bot.c_str(),
                           was_timeout ? "timed out" : "cancelled",
                           result.round_trip_ms));
    std::move(done).Run();
    return;
  }
  const std::optional<double> age_at_plan_ms =
      AgeMsSince(snapshot_monotonic_us, now);

  std::optional<Plan> plan;
  if (result.reply) {
    if (result.reply->finish_reason == "length") {
      Log(base::StringPrintf(
          "harness: %s reply hit max_tokens (%d); truncated last line dropped",
          bot.c_str(), request.max_tokens));
    }
    plan = ParsePlan(
        StripTruncatedLastLine(result.reply->text, result.reply->finish_reason),
        static_cast<int>(ask_screen.options.size()), order_names);
  }

  Decision decision;
  decision.bot = bot;
  decision.reason = std::move(wake_reason);
  decision.request = std::move(request);
  decision.result = std::move(result);
  decision.plan = plan;
  decision.time = base::Time::Now();
  decision.age_at_ask_ms = age_at_ask_ms;
  if (age_at_first_line_ms > 0) {
    decision.age_at_first_line_ms = age_at_first_line_ms;
  } else if (age_at_plan_ms) {
    decision.age_at_first_line_ms = age_at_plan_ms;
  }
  decision.age_at_plan_ms = age_at_plan_ms;
  if (decision_log_) {
    decision_log_->Record(decision);
  }
  ++decisions_completed_;

  auto it = bots_.find(bot);
  if (it != bots_.end() && it->second.call_epoch == epoch) {
    BotState& state = it->second;
    state.call_in_flight = false;
    state.in_flight_cancel.reset();
    state.call_deadline = base::TimeTicks();
    if (!decision.result.reply) {
      Log(base::StringPrintf("harness: %s brain error (%.0f ms): %s",
                             bot.c_str(), decision.result.round_trip_ms,
                             decision.result.error.c_str()));
      state.look_deadline = now + base::Seconds(10);
      Requeue(&state, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_chats), std::move(asked_outcome));
    } else if (!plan) {
      Log(base::StringPrintf("harness: %s invalid plan (%.0f ms)", bot.c_str(),
                             decision.result.round_trip_ms));
      state.look_deadline = now + base::Seconds(10);
      Requeue(&state, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_chats), std::move(asked_outcome));
    } else {
      if (!plan->goal.empty()) {
        state.current_goal = plan->goal;
      }
      const std::string formatted = FormatPlan(*plan);
      const std::string do_line = formatted.substr(0, formatted.find('\n'));
      Log(base::StringPrintf(
          "harness: %s woke (%s) -> %s (do %.0f ms, total %.0f ms, look %d s, "
          "prompt %d tok)",
          bot.c_str(), decision.reason.c_str(), do_line.c_str(),
          decision.result.first_line_ms, decision.result.round_trip_ms,
          plan->look_s, decision.result.reply->prompt_tokens));
      // Only install watches/steps if no newer surprise already superseded this
      // call while it was in flight. When an early-dispatched step of this
      // same call failed before the reply finished, still deliver say/tell and
      // watches, and keep the refusal wake or backoff timer instead of
      // requeueing the already-answered wake reasons.
      if ((!state.has_queued_wake || state.early_step_failed) &&
          state.latest_snapshot) {
        state.triggers.SetPlanContext(
            plan->watches,
            state.early_step_failed
                ? std::flat_set<std::string>{}
                : CollectExpectedNames(plan->steps, plan->expect, ask_screen),
            &state.ask_watch_inputs);
        if (plan->clear_orders && plan->orders.empty()) {
          if (state.orders.SetOrders({}, now)) {
            SendStopMoving(bot);
          }
          state.order_wakes = 0;
          if (state.latest_snapshot->base.where.in_instance &&
              !state.latest_snapshot->base.self.in_combat &&
              !state.last_selected_notes.empty()) {
            compiled_orders_cache_.erase(
                CompiledOrdersKey{bot, state.latest_snapshot->base.where.map,
                                  state.last_selected_notes});
          }
        } else if (!plan->orders.empty()) {
          if (state.orders.SetOrders(plan->orders, now)) {
            SendStopMoving(bot);
          }
        }
        if (!plan->orders.empty() &&
            state.latest_snapshot->base.where.in_instance &&
            !state.latest_snapshot->base.self.in_combat &&
            !state.last_selected_notes.empty()) {
          const std::string& map_name = state.latest_snapshot->base.where.map;
          const CompiledOrdersKey key{bot, map_name, state.last_selected_notes};
          // Bounded (RH-L5): a long session visits many instance/notes pairs;
          // dropping all of them only costs one recompile per instance entry.
          // Always overwrite with the newest compiled orders (A-L8).
          if (compiled_orders_cache_.size() >= kMaxCompiledOrdersCache &&
              !compiled_orders_cache_.contains(key)) {
            compiled_orders_cache_.clear();
          }
          const bool inserted =
              compiled_orders_cache_.insert_or_assign(key, plan->orders).second;
          Log(base::StrCat({bot, inserted ? ": cached " : ": updated cached ",
                            std::to_string(plan->orders.size()),
                            " pre-pull standing orders for ", map_name}));
        }
        if (!state.early_step_failed) {
          state.look_deadline = now + base::Seconds(plan->look_s);
        }
        const bool party_reply_muted = state.party_reply_muted_epoch == epoch;
        if (!plan->say.empty() && !party_reply_muted) {
          // Answer a party/raid line in that channel (RH-L1, A-M14: decided by
          // the structured chat event's channel, never by splitting strings).
          pb::ChatChannel ch = pb::SAY;
          if ((state.latest_snapshot->ext.party &&
               !state.latest_snapshot->ext.party->members.empty()) ||
              (state.latest_snapshot->ext.raid &&
               !state.latest_snapshot->ext.raid->members.empty())) {
            for (const BotState::ObservedChat& c : asked_chats) {
              if (c.triggered_wake &&
                  (c.channel == pb::PARTY || c.channel == pb::RAID)) {
                ch = c.channel;
                break;
              }
            }
          }
          // A `party:` line already says it there (RH-L6: no double post).
          if (ch == pb::SAY || plan->party.empty()) {
            SendSay(bot, ch, "", plan->say);
          }
        }
        const bool in_party =
            state.latest_snapshot->ext.party &&
            !state.latest_snapshot->ext.party->members.empty();
        const bool in_raid = state.latest_snapshot->ext.raid &&
                             !state.latest_snapshot->ext.raid->members.empty();
        if (!plan->party.empty() && (in_party || in_raid) &&
            !party_reply_muted) {
          SendSay(bot, in_raid ? pb::RAID : pb::PARTY, "", plan->party);
        }
        if (!plan->tell.empty()) {
          SendSay(bot, pb::WHISPER, plan->tell_to, plan->tell);
        }
        // Public channels (F1): a refusal (not queued, wrong place) comes back
        // as a chat error event, which wakes the brain to see it.
        if (!plan->general.empty()) {
          SendSay(bot, pb::GENERAL, "", plan->general);
        }
        if (!plan->lfg.empty()) {
          SendSay(bot, pb::LFG, "", plan->lfg);
        }
        if (!plan->remember.empty() && !config_.bots_dir.empty()) {
          std::vector<std::string> player_chat_texts;
          for (const BotState::ObservedChat& c : asked_chats) {
            if (IsPlayerChatChannel(c.channel)) {
              player_chat_texts.push_back(c.text);
            }
          }
          const std::string safe_memo =
              SanitizeRememberNote(plan->remember, player_chat_texts);
          if (safe_memo.empty()) {
            Log(base::StrCat(
                {bot, ": ignored unsafe remember note from chat wake"}));
          } else {
            const std::string dir = base::StrCat({config_.bots_dir, "/", bot});
            const std::string mem_path = dir + "/memory.md";
            const std::string date = DateUtc(base::Time::Now());
            if (!MakeDirectories(dir) ||
                !AppendToFile(mem_path, base::StrCat({"- ", date, " ",
                                                      safe_memo, "\n"}))) {
              Log(base::StrCat({bot, ": could not write memory.md"}));
            } else {
              file_cache_.erase(mem_path);
              RecordRelationshipNotes(bot, state, asked_chats, date, safe_memo);
            }
          }
        }
        for (const std::string& name : plan->close) {
          SendPanel(bot, PanelFromName(name), /*open=*/false);
        }
        // A look at what the brain already had on its screen shows nothing
        // new: no wake for it (it woke the bot on every snapshot: review of
        // f40b0b6, finding 5).
        BotState::PendingLook look;
        for (const std::string& name : plan->open) {
          const pb::Panel panel = PanelFromName(name);
          SendPanel(bot, panel, /*open=*/true);
          if (std::find(ask_screen.open_panels.begin(),
                        ask_screen.open_panels.end(),
                        panel) == ask_screen.open_panels.end()) {
            look.panels.push_back(panel);
          }
          // The brain keeps it open.
          std::erase_if(state.glancing, [&](const BotState::Glance& g) {
            return g.panel == panel;
          });
        }
        if (!plan->hover.empty()) {
          for (const Hoverable& h : ask_screen.hoverables) {
            if (h.name == plan->hover) {
              SendHover(bot, h.ref);
              if (std::find(ask_screen.shown_tooltips.begin(),
                            ask_screen.shown_tooltips.end(),
                            h.name) == ask_screen.shown_tooltips.end()) {
                look.hover = h.name;
              }
              break;
            }
          }
        }
        if (!look.panels.empty() || !look.hover.empty()) {
          look.until = now + config_.look_wait;
          state.pending_look = std::move(look);
        }
        if (!state.early_steps_dispatched) {
          const RunnerAction action =
              state.runner.Start(plan->steps, ask_seq, ask_screen, now);
          DispatchRunnerAction(bot, &state, action);
        }
        if (state.order_failure_during_call) {
          state.order_failure_during_call = false;
          WakeForOrder(bot, &state, "", now, /*is_failure=*/true);
        }
        if (!state.has_queued_wake && state.latest_snapshot &&
            !state.latest_snapshot->base.self.in_combat) {
          MaybeScheduleMemoryCondense(bot, &state, now);
        }
      } else {
        // Superseded by a newer wake while in flight: that call answers. Still
        // honor an explicit `orders: none` clear so a runaway standing-order
        // wake can never trap the bot in a livelock (A-H2).
        if (plan->clear_orders && plan->orders.empty() &&
            state.latest_snapshot) {
          if (state.orders.SetOrders({}, now)) {
            SendStopMoving(bot);
          }
          state.order_wakes = 0;
          if (state.latest_snapshot->base.where.in_instance &&
              !state.latest_snapshot->base.self.in_combat &&
              !state.last_selected_notes.empty()) {
            compiled_orders_cache_.erase(
                CompiledOrdersKey{bot, state.latest_snapshot->base.where.map,
                                  state.last_selected_notes});
          }
        }
        Requeue(&state, std::move(asked_reasons), std::move(asked_events),
                std::move(asked_chats), std::move(asked_outcome));
      }
    }
  }
  std::move(done).Run();
}

void BotLoop::DispatchRunnerAction(const std::string& bot,
                                   BotState* state,
                                   const RunnerAction& action) {
  if (action.choose) {
    const uint32_t id = SendChoose(bot, *action.choose);
    state->runner.set_last_command_id(id);
  }
  if (!action.finished_reason.empty()) {
    state->look_deadline = base::TimeTicks();
    WakeBot(bot, Urgency::kNormal, action.finished_reason);
  }
}

uint32_t BotLoop::SendChoose(const std::string& bot,
                             const pb::Command::Choose& choose) {
  pb::Command cmd;
  cmd.bot = bot;
  const uint32_t id = ++next_command_id_;
  cmd.command_id = id;
  cmd.choose = choose;
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
  return id;
}

bool BotLoop::OnLookSnapshot(const std::string& bot,
                             BotState* state,
                             base::TimeTicks received_at) {
  const pb::Snapshot& snap = *state->latest_snapshot;
  const pb::Ui& ui = snap.base.ui;
  const auto is_open = [&ui](pb::Panel panel) {
    return std::find(ui.open_panels.begin(), ui.open_panels.end(), panel) !=
           ui.open_panels.end();
  };
  for (const pb::Panel panel : ui.open_panels) {
    if (std::vector<std::string> lines = PanelLines(snap, panel);
        !lines.empty()) {
      state->seen_panels[panel] = {std::move(lines), received_at};
    }
  }

  // Habits: no brain call. They act on panels a habit opened only, never on
  // one the brain opened, and each act is a line in the next prompt, so the
  // brain knows what its body did (review of f40b0b6, finding 8).
  const BotLoopConfig::Habits& habits = config_.habits;
  const bool in_combat = snap.base.self.in_combat;
  const auto asked = [&](pb::Panel panel) {
    return state->pending_look &&
           std::find(state->pending_look->panels.begin(),
                     state->pending_look->panels.end(),
                     panel) != state->pending_look->panels.end();
  };
  if (habits.close_panels_in_combat && in_combat && !state->was_in_combat) {
    for (const BotState::Glance& g : state->glancing) {
      if (is_open(g.panel)) {
        SendPanel(bot, g.panel, /*open=*/false);
        NoteText(state, base::StrCat({"you closed your ", PanelName(g.panel),
                                      " (combat started)"}));
      }
    }
    state->glancing.clear();
  }
  state->was_in_combat = in_combat;
  std::erase_if(state->glancing, [&](const BotState::Glance& g) {
    if (asked(g.panel)) {
      return true;  // The brain asked for it meanwhile: it stays open.
    }
    if (!is_open(g.panel)) {
      return received_at >= g.until;  // Never shown: given up.
    }
    SendPanel(bot, g.panel, /*open=*/false);  // Seen (and remembered).
    return true;
  });
  const auto glance = [&](pb::Panel panel, std::string_view why) {
    if (in_combat || is_open(panel) || asked(panel) ||
        std::any_of(
            state->glancing.begin(), state->glancing.end(),
            [&](const BotState::Glance& g) { return g.panel == panel; })) {
      return;
    }
    SendPanel(bot, panel, /*open=*/true);
    state->glancing.push_back({panel, received_at + config_.look_wait});
    NoteText(state, base::StrCat({"you glanced at your ", PanelName(panel),
                                  " (", why, ")"}));
  };
  if (const std::optional<uint32_t> bag_free =
          snap.base.self.indicators.bag_free_slots) {
    const uint32_t free = *bag_free;
    if (habits.open_bags_when_full && free == 0 &&
        state->last_bag_free_slots != 0) {
      glance(pb::BAGS, "they were full");
    }
    state->last_bag_free_slots = free;
  }
  std::string quest_state;
  for (const pb::Objective& o : snap.ext.quest_tracker) {
    if (o.quest) {
      base::StrAppend(&quest_state, {base::NumberToString(o.quest->quest_id),
                                     o.quest->complete ? "c" : "", ";"});
    }
  }
  if (habits.glance_at_quest_log && state->last_quest_state &&
      quest_state != *state->last_quest_state) {
    glance(pb::QUEST_LOG, "your quests changed");
  }
  state->last_quest_state = std::move(quest_state);

  // The look the brain asked for.
  if (!state->pending_look) {
    return false;
  }
  const BotState::PendingLook& look = *state->pending_look;
  bool shown = std::all_of(look.panels.begin(), look.panels.end(), is_open);
  if (!look.hover.empty()) {
    shown = shown &&
            std::any_of(snap.base.tooltips.begin(), snap.base.tooltips.end(),
                        [&](const pb::Tooltipped& t) {
                          return t.ref && t.ref->name == look.hover;
                        });
  }
  if (!shown) {
    if (received_at >= look.until) {
      state->pending_look.reset();
    }
    return false;
  }
  std::vector<std::string> what;
  for (const pb::Panel panel : look.panels) {
    what.emplace_back(PanelName(panel));
  }
  if (!look.hover.empty()) {
    what.push_back(look.hover);
  }
  state->pending_look.reset();
  if (state->call_in_flight || state->has_queued_wake) {
    return false;  // A newer call sees it anyway.
  }
  state->look_deadline = base::TimeTicks();
  WakeBot(bot, Urgency::kNormal,
          base::StrCat({"looked at ", base::JoinString(what, ", ")}));
  return true;
}

// static
std::vector<std::string> BotLoop::RememberedPanels(
    const BotState& state,
    base::TimeTicks now,
    base::TimeDelta remember_for) {
  std::vector<std::string> out;
  const pb::Ui& ui = state.latest_snapshot->base.ui;
  for (const auto& [panel, seen] : state.seen_panels) {
    if (std::find(ui.open_panels.begin(), ui.open_panels.end(), panel) !=
            ui.open_panels.end() ||
        seen.lines.empty() || now - seen.seen_at > remember_for) {
      continue;  // On screen now, or forgotten.
    }
    std::vector<std::string> lines = seen.lines;
    lines.front() = base::StringPrintf(
        "%s, seen %.0f s ago: %s", std::string(PanelName(panel)).c_str(),
        (now - seen.seen_at).InSecondsF(),
        std::string(base::TrimWhitespaceASCII(lines.front())).c_str());
    out.insert(out.end(), lines.begin(), lines.end());
  }
  return out;
}

void BotLoop::SendPanel(const std::string& bot, pb::Panel panel, bool open) {
  if (panel == pb::PANEL_UNSPECIFIED) {
    return;
  }
  pb::Command cmd;
  cmd.bot = bot;
  cmd.command_id = ++next_command_id_;
  if (open) {
    cmd.open.emplace().panel = panel;
  } else {
    cmd.close.emplace().panel = panel;
  }
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
}

void BotLoop::SendHover(const std::string& bot, const pb::HoverRef& ref) {
  pb::Command cmd;
  cmd.bot = bot;
  cmd.command_id = ++next_command_id_;
  cmd.hover.emplace().ref = ref;
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
}

void BotLoop::SendStopMoving(const std::string& bot) {
  pb::Command::Act stop_moving;
  stop_moving.verb = pb::Command::Act::STOP;
  stop_moving.stop = pb::Command::Act::MOVING;
  SendAct(bot, std::move(stop_moving));
}

bool BotLoop::WakeForOrder(const std::string& bot,
                           BotState* state,
                           std::string_view reason,
                           base::TimeTicks now,
                           bool is_failure) {
  // Apply exponential backoff to BOTH repeated order failures and rapid
  // consecutive `-> wake` orders (A-H2) so a runaway `every Xs -> wake` cannot
  // livelock the bot or starve other bots at P0. The first wake (`order_wakes
  // == 1`) always fires immediately at P0 with zero delay.
  const base::TimeDelta backoff = StepRefusalBackoff(++state->order_wakes);
  if (backoff.is_positive()) {
    if (!reason.empty()) {
      const std::string r(reason);
      if (std::find(state->pending_reasons.begin(),
                    state->pending_reasons.end(),
                    r) == state->pending_reasons.end()) {
        state->pending_reasons.push_back(r);
      }
    }
    // Never push an earlier plan/backoff look deadline further out.
    SetEarlierDeadline(&state->look_deadline, now + backoff);
    Log(base::StringPrintf(
        "harness: %s order %s %d in a row; next look in %.0f s", bot.c_str(),
        is_failure ? "failure" : "wake", state->order_wakes,
        backoff.InSecondsF()));
    return false;
  }
  state->look_deadline = base::TimeTicks();
  WakeBot(bot, Urgency::kUrgent, std::string(reason),
          /*preserve_backoffs=*/true);
  return true;
}

bool BotLoop::DispatchOrderEval(const std::string& bot,
                                BotState* state,
                                const OrderEvalResult& eval,
                                base::TimeTicks now,
                                std::string_view extra_wake_reason) {
  auto merge_extra = [&] {
    if (!extra_wake_reason.empty()) {
      const std::string r(extra_wake_reason);
      if (std::find(state->pending_reasons.begin(),
                    state->pending_reasons.end(),
                    r) == state->pending_reasons.end()) {
        state->pending_reasons.push_back(r);
      }
    }
  };
  if (eval.kite_ended) {
    SendStopMoving(bot);
    return false;
  }
  if (!eval.wake_reason.empty()) {
    if (eval.timed_out || eval.preempted) {
      // Stop any in-progress movement (e.g. a timed-out act or preempted kite)
      // and release `MOVEMENT_FORCED` on the module before waking the brain.
      SendStopMoving(bot);
    }
    state->runner.Stop();
    merge_extra();
    return WakeForOrder(bot, state, eval.wake_reason, now,
                        /*is_failure=*/eval.timed_out);
  }
  if (!eval.act) {
    return false;
  }
  if (eval.preempted) {
    SendStopMoving(bot);
  }
  std::optional<pb::Command::Act> wire = OrderActToCommand(
      *eval.act, eval.it_guid, eval.it_name, eval.target_guid);
  if (!wire) {
    OrderEvalResult failed = state->orders.OnStepFailed("unsupported act verb");
    state->runner.Stop();
    state->look_deadline = base::TimeTicks();
    if (!failed.wake_reason.empty()) {
      merge_extra();
      return WakeForOrder(bot, state, failed.wake_reason, now,
                          /*is_failure=*/true);
    }
    return false;
  }
  const uint32_t id = SendAct(bot, std::move(*wire));
  state->orders.set_pending_command_id(id);
  return false;
}

uint32_t BotLoop::SendAct(const std::string& bot, pb::Command::Act act) {
  pb::Command cmd;
  cmd.bot = bot;
  cmd.command_id = ++next_command_id_;
  cmd.act = std::move(act);
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
  return cmd.command_id;
}

void BotLoop::SendSay(const std::string& bot,
                      pb::ChatChannel channel,
                      const std::string& to,
                      const std::string& text) {
  pb::Command cmd;
  cmd.bot = bot;
  cmd.command_id = ++next_command_id_;
  pb::Command::Say& say = cmd.say.emplace();
  say.channel = channel;
  say.to = to;
  say.text = text;
  auto& ids = bots_[bot].recent_chat_command_ids;
  ids.push_back(cmd.command_id);
  while (ids.size() > 16) {
    ids.pop_front();
  }
  ++commands_sent_;
  if (send_command_) {
    send_command_.Run(cmd);
  }
}

void BotLoop::Log(const std::string& line) {
  if (log_message_) {
    log_message_.Run(line);
  }
}

void BotLoop::MaybeScheduleMemoryCondense(const std::string& bot,
                                          BotState* state,
                                          base::TimeTicks now) {
  if (config_.bots_dir.empty() || config_.memory_condense_threshold == 0 ||
      state->condense_state != BotState::CondenseState::kIdle ||
      state->call_in_flight || state->has_queued_wake ||
      !state->latest_snapshot || state->latest_snapshot->base.self.in_combat ||
      now < state->condense_retry_after) {
    return;
  }
  const std::string mem_path =
      base::StrCat({config_.bots_dir, "/", bot, "/memory.md"});
  const ParsedMemory mem = ParseMemoryFile(ReadFileIfChanged(mem_path));
  if (mem.recent.size() < config_.memory_condense_threshold) {
    return;
  }
  state->condense_state = BotState::CondenseState::kQueued;
  base::WeakPtr<BotLoop> self = weak_factory_.GetWeakPtr();
  // Scheduled under `bot` itself at P4 so it runs on `bot`'s own slot without
  // evicting other bots' KV-cache slots, yields to any newer wake, and is
  // preempted immediately by a P0 combat wake.
  scheduler_.SubmitCancelable(
      bot, Urgency::kP4Reflect, [self, bot](int slot, base::OnceClosure done) {
        if (!self) {
          std::move(done).Run();
          return base::OnceClosure{};
        }
        return self->StartMemoryCondense(bot, slot, std::move(done));
      });
}

base::OnceClosure BotLoop::StartMemoryCondense(const std::string& bot,
                                               int slot,
                                               base::OnceClosure done) {
  auto it = bots_.find(bot);
  if (it == bots_.end() ||
      it->second.condense_state != BotState::CondenseState::kQueued ||
      config_.bots_dir.empty() || config_.memory_condense_threshold == 0 ||
      !it->second.latest_snapshot ||
      it->second.latest_snapshot->base.self.in_combat) {
    if (it != bots_.end() &&
        it->second.condense_state == BotState::CondenseState::kQueued) {
      it->second.condense_state = BotState::CondenseState::kIdle;
    }
    std::move(done).Run();
    return base::OnceClosure{};
  }
  const std::string mem_path =
      base::StrCat({config_.bots_dir, "/", bot, "/memory.md"});
  const std::optional<std::string> disk = ReadFile(mem_path);
  if (!disk) {
    it->second.condense_state = BotState::CondenseState::kIdle;
    it->second.condense_retry_after =
        base::TimeTicks::Now() + base::Seconds(60);
    std::move(done).Run();
    return base::OnceClosure{};
  }
  const ParsedMemory mem = ParseMemoryFile(*disk);
  const size_t keep_recent = std::min(config_.memory_keep_recent,
                                      config_.memory_condense_threshold - 1);
  if (mem.recent.size() < config_.memory_condense_threshold ||
      mem.recent.size() <= keep_recent) {
    it->second.condense_state = BotState::CondenseState::kIdle;
    std::move(done).Run();
    return base::OnceClosure{};
  }
  const size_t condense_count = mem.recent.size() - keep_recent;
  it->second.condense_state = BotState::CondenseState::kInFlight;
  const uint64_t condense_epoch = ++it->second.memory_condense_epoch;
  std::string user =
      base::StrCat({"Player: ", bot, "\n\nCondense these older memories:"});
  if (!mem.summary.empty()) {
    base::StrAppend(&user, {"\n\nPrevious summary:\n",
                            base::JoinString(mem.summary, "\n")});
  }
  base::StrAppend(&user, {"\n\nOlder notes to merge in:"});
  for (size_t i = 0; i < condense_count; ++i) {
    base::StrAppend(&user, {"\n", mem.recent[i]});
  }

  LlmRequest req;
  req.system =
      "You condense a World of Warcraft player's older session notes into a "
      "short bullet list of lasting memories. Keep player names, dungeons, "
      "bosses, lessons learned, and unfinished goals. Write at most 6 short "
      "lines, each starting with '- ', plain facts only, no preamble.";
  req.user = std::move(user);
  req.slot = slot;
  req.max_tokens = 200;
  req.temperature = 0.2f;
  req.stream = false;

  base::WeakPtr<BotLoop> self = weak_factory_.GetWeakPtr();
  base::scoped_refptr<LlmCancelHandle> handle =
      ask_.Run(std::move(req),
               [self, bot, condense_epoch, condense_count,
                done = std::move(done)](LlmResult result) mutable {
                 if (!self) {
                   std::move(done).Run();
                   return;
                 }
                 self->OnMemoryCondensed(bot, condense_epoch, condense_count,
                                         std::move(done), std::move(result));
               },
               {});
  if (!handle) {
    return base::OnceClosure{};
  }
  return [handle = std::move(handle)]() { handle->Cancel(); };
}

void BotLoop::OnMemoryCondensed(const std::string& bot,
                                uint64_t condense_epoch,
                                size_t condensed_recent_count,
                                base::OnceClosure done,
                                LlmResult result) {
  auto it = bots_.find(bot);
  if (it == bots_.end() || it->second.memory_condense_epoch != condense_epoch) {
    std::move(done).Run();
    return;
  }
  it->second.condense_state = BotState::CondenseState::kIdle;
  if (result.cancelled || config_.bots_dir.empty()) {
    std::move(done).Run();
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  if (!result.reply) {
    it->second.condense_retry_after = now + base::Seconds(60);
    Log(base::StrCat({bot, ": memory condensing failed: ", result.error}));
    std::move(done).Run();
    return;
  }
  const std::vector<std::string> summary =
      ParseCondensedBullets(result.reply->text);
  if (summary.empty()) {
    it->second.condense_retry_after = now + base::Seconds(60);
    Log(base::StrCat({bot, ": memory condensing returned no summary bullets"}));
    std::move(done).Run();
    return;
  }
  const std::string mem_path =
      base::StrCat({config_.bots_dir, "/", bot, "/memory.md"});
  // Re-read from disk so any `remember:` lines appended while this P4 call was
  // in flight are preserved verbatim at the end of `## Recent`; abort without
  // overwriting if the read fails.
  const std::optional<std::string> disk = ReadFile(mem_path);
  if (!disk) {
    it->second.condense_retry_after = now + base::Seconds(60);
    Log(base::StrCat({bot, ": could not re-read memory.md after condensing"}));
    std::move(done).Run();
    return;
  }
  const ParsedMemory cur = ParseMemoryFile(*disk);
  const size_t drop = std::min(condensed_recent_count, cur.recent.size());
  std::vector<std::string> kept_recent(
      cur.recent.begin() + static_cast<ptrdiff_t>(drop), cur.recent.end());
  if (!WriteFileAtomic(mem_path, FormatMemoryFile(summary, kept_recent))) {
    it->second.condense_retry_after = now + base::Seconds(60);
    Log(base::StrCat({bot, ": could not write condensed memory.md"}));
  } else {
    file_cache_.erase(mem_path);
    Log(base::StringPrintf(
        "%s: condensed %zu memory note(s) into %zu summary bullet(s)",
        bot.c_str(), drop, summary.size()));
  }
  std::move(done).Run();
}

void BotLoop::RecordRelationshipNotes(
    const std::string& bot,
    const BotState& state,
    const std::vector<BotState::ObservedChat>& asked_chats,
    std::string_view date,
    std::string_view memo) {
  if (config_.bots_dir.empty() || memo.empty()) {
    return;
  }
  const std::string dir = base::StrCat({config_.bots_dir, "/", bot});
  const std::string rel_path = dir + "/relationships.md";
  ParsedRelationships rels =
      ParseRelationshipsFile(ReadFile(rel_path).value_or(""));

  std::vector<std::string> candidates;
  if (state.latest_snapshot) {
    if (state.latest_snapshot->ext.party) {
      for (const pb::Unit& m : state.latest_snapshot->ext.party->members) {
        AddUniquePlayerName(&candidates, bot, m.name);
      }
    }
    if (state.latest_snapshot->ext.raid) {
      for (const pb::Unit& m : state.latest_snapshot->ext.raid->members) {
        AddUniquePlayerName(&candidates, bot, m.name);
      }
    }
    for (const pb::Unit& u : state.latest_snapshot->base.nearby.units) {
      if (u.kind == pb::Unit::PLAYER) {
        AddUniquePlayerName(&candidates, bot, u.name);
      }
    }
  }
  for (const BotState::Whisperer& w : state.whisperers) {
    AddUniquePlayerName(&candidates, bot, w.name);
  }
  for (const BotState::ObservedChat& c : asked_chats) {
    if (IsPlayerChatChannel(c.channel)) {
      AddUniquePlayerName(&candidates, bot, c.from);
    }
  }
  for (const RelationshipSection& sec : rels.sections) {
    AddUniquePlayerName(&candidates, bot, sec.player);
  }

  bool changed = false;
  const std::string entry = base::StrCat({"- ", date, " ", memo});
  for (const std::string& player : candidates) {
    if (!ContainsWholeWordIgnoringCase(memo, player)) {
      continue;
    }
    RelationshipSection* sec = nullptr;
    for (RelationshipSection& existing : rels.sections) {
      if (base::EqualsCaseInsensitiveASCII(existing.player, player)) {
        sec = &existing;
        break;
      }
    }
    if (!sec) {
      sec = &rels.sections.emplace_back(RelationshipSection{player, {}});
    }
    if (!sec->notes.empty() && sec->notes.back() == entry) {
      continue;
    }
    sec->notes.push_back(entry);
    if (sec->notes.size() > kMaxRelationshipNotesPerPlayer) {
      sec->notes.erase(sec->notes.begin(),
                       sec->notes.end() - static_cast<ptrdiff_t>(
                                              kMaxRelationshipNotesPerPlayer));
    }
    changed = true;
  }
  if (!changed) {
    return;
  }
  if (!MakeDirectories(dir) ||
      !WriteFileAtomic(rel_path, FormatRelationshipsFile(rels))) {
    Log(base::StrCat({bot, ": could not write relationships.md"}));
  } else {
    file_cache_.erase(rel_path);
  }
}

}  // namespace harness
