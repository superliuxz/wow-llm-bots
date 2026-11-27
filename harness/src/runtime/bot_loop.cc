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

// Player chat channels worth keeping in short-term per-player exchange memory:
// direct/group/local channels always count; noisy public broadcast channels
// (General/Trade/LFG/custom) only enter when directly addressing the bot.
bool IsConversationalPlayerChannel(pb::ChatChannel ch, bool at_me) {
  switch (ch) {
    case pb::WHISPER:
    case pb::SAY:
    case pb::YELL:
    case pb::PARTY:
    case pb::RAID:
    case pb::RAID_WARNING:
    case pb::GUILD:
    case pb::OFFICER:
    case pb::EMOTE:
    case pb::BATTLEGROUND:
      return true;
    case pb::GENERAL:
    case pb::TRADE:
    case pb::LFG:
    case pb::CUSTOM_CHANNEL:
      return at_me;
    default:
      return false;
  }
}

std::string ChatChannelLabel(pb::ChatChannel ch) {
  switch (ch) {
    case pb::WHISPER:
      return "whisper";
    case pb::SAY:
      return "say";
    case pb::YELL:
      return "yell";
    case pb::PARTY:
      return "party";
    case pb::RAID:
      return "raid";
    case pb::RAID_WARNING:
      return "raid_warning";
    case pb::GUILD:
      return "guild";
    case pb::OFFICER:
      return "officer";
    case pb::GENERAL:
      return "general";
    case pb::TRADE:
      return "trade";
    case pb::LFG:
      return "lfg";
    case pb::EMOTE:
      return "emote";
    case pb::BATTLEGROUND:
      return "bg";
    case pb::CUSTOM_CHANNEL:
      return "channel";
    default:
      return "chat";
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

// How long one act of a plan's `act:` line may run before the held `do:`
// steps go ahead anyway (a walk into range, then a cast; a standing order's
// act or `stop moving` can supersede it on the module without a word).
constexpr base::TimeDelta kPlanActTimeout = base::Seconds(12);

// Per-sender chat wake rate limit (A-M8): at most 3 chat-triggered LLM wakes
// per sender in a 10 s sliding window (messages still appear in recent_events).
constexpr base::TimeDelta kChatWakeWindow = base::Seconds(10);
constexpr size_t kMaxChatWakesPerSender = 3;

// Per-party unprompted chat budget across all bots in the group (Now #10): at
// most 2 unprompted lines per 30 s, spaced by >= 6 s, with no 30 s echoes.
constexpr base::TimeDelta kPartyBudgetWindow = base::Seconds(30);
constexpr size_t kMaxUnpromptedPartyLinesPerWindow = 2;
constexpr base::TimeDelta kMinUnpromptedPartySpacing = base::Seconds(6);
constexpr int64_t kTypingMsPerChar = 35;

bool HasGroupMembers(const pb::Snapshot& snapshot) {
  return (snapshot.ext.party && !snapshot.ext.party->members.empty()) ||
         (snapshot.ext.raid && !snapshot.ext.raid->members.empty());
}

// A party member is AFK and the bot is out of combat: it waits for them like
// a player would, so its routine look timer and unasked chatter hold until
// they're back (P0 wakes, chat and a refusal's retry still go through).
bool WaitingOnAfkMember(const pb::Snapshot& snapshot) {
  if (snapshot.base.self.in_combat || !snapshot.ext.party) {
    return false;
  }
  return std::any_of(snapshot.ext.party->members.begin(),
                     snapshot.ext.party->members.end(), [](const pb::Unit& m) {
                       return m.group && m.group->online && m.group->afk;
                     });
}

bool IsGroupSharedChatChannel(pb::ChatChannel ch,
                              const pb::Snapshot& snapshot) {
  if (ch == pb::PARTY || ch == pb::RAID) {
    return true;
  }
  return (ch == pb::SAY || ch == pb::YELL) && HasGroupMembers(snapshot);
}

std::string PartyBudgetKey(const std::string& bot,
                           const pb::Snapshot& snapshot) {
  if (!HasGroupMembers(snapshot)) {
    return "";
  }
  std::vector<std::string> names{bot};
  if (snapshot.ext.party) {
    for (const pb::Unit& m : snapshot.ext.party->members) {
      if (!m.name.empty()) {
        names.push_back(m.name);
      }
    }
  }
  if (snapshot.ext.raid) {
    for (const pb::Unit& m : snapshot.ext.raid->members) {
      if (!m.name.empty()) {
        names.push_back(m.name);
      }
    }
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names.size() > 1 ? base::JoinString(names, ",") : "";
}

// How long a person would take to type `chars` characters, less what the
// reader already waited (`already_waited`: the brain's inference time since the
// line that woke it), never negative.
base::TimeDelta ComputeTypingDelay(size_t chars,
                                   base::TimeDelta min_delay,
                                   base::TimeDelta max_delay,
                                   base::TimeDelta already_waited) {
  if (!max_delay.is_positive()) {
    return base::TimeDelta();
  }
  const base::TimeDelta raw =
      min_delay +
      base::Milliseconds(static_cast<int64_t>(chars) * kTypingMsPerChar);
  return std::max(base::TimeDelta(),
                  std::clamp(raw, min_delay, max_delay) - already_waited);
}

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

// How players address party members in chat (addressing, not judgment): a
// class by its name, a plural, or a common nickname/typo, or a role. A word
// here only says who a line is for; the addressed bots' brains decide what to
// do about it.
struct AddressWord {
  std::string_view word;
  std::string_view class_name;  // pb::PlayerPart::class_, or "" for a role.
  pb::Role role = pb::Role::ROLE_UNSPECIFIED;
};
constexpr AddressWord kAddressWords[] = {
    {"warrior", "Warrior"},
    {"warriors", "Warrior"},
    {"warr", "Warrior"},
    {"paladin", "Paladin"},
    {"paladins", "Paladin"},
    {"pally", "Paladin"},
    {"pallies", "Paladin"},
    {"pala", "Paladin"},
    {"hunter", "Hunter"},
    {"hunters", "Hunter"},
    {"hunt", "Hunter"},
    {"rogue", "Rogue"},
    {"rogues", "Rogue"},
    {"rouge", "Rogue"},
    {"rouges", "Rogue"},
    {"priest", "Priest"},
    {"priests", "Priest"},
    {"preist", "Priest"},
    {"death knight", "Death Knight"},
    {"deathknight", "Death Knight"},
    {"dk", "Death Knight"},
    {"dks", "Death Knight"},
    {"shaman", "Shaman"},
    {"shamans", "Shaman"},
    {"shammy", "Shaman"},
    {"sham", "Shaman"},
    {"mage", "Mage"},
    {"mages", "Mage"},
    {"warlock", "Warlock"},
    {"warlocks", "Warlock"},
    {"lock", "Warlock"},
    {"locks", "Warlock"},
    {"druid", "Druid"},
    {"druids", "Druid"},
    {"dudu", "Druid"},
    {"tank", "", pb::Role::TANK},
    {"tanks", "", pb::Role::TANK},
    {"healer", "", pb::Role::HEALER},
    {"healers", "", pb::Role::HEALER},
    {"heals", "", pb::Role::HEALER},
    {"healz", "", pb::Role::HEALER},
    {"dps", "", pb::Role::DPS},
};

// Words of a tactical call to the whole group ("los", "spread", "stack",
// "stop dps", "buff pls", "anyone got decurse?"): unaddressed, such a line
// from a human wakes every online brain bot in the party or raid so nobody
// misses a tactical call (AnswersPartyLine still keeps replies to a single
// responder); chit-chat and invitations ("anyone up for RFC?") wake only one.
constexpr std::string_view kGroupCallWords[] = {
    "los",     "buff",   "buffs",  "rebuff", "decurse",   "dispel",
    "cleanse", "cure",   "curse",  "poison", "disease",   "magic",
    "cc",      "sheep",  "poly",   "sap",    "trap",      "fear",
    "shackle", "hex",    "root",   "stun",   "kick",      "interrupt",
    "pull",    "wait",   "stop",   "hold",   "run",       "stack",
    "clump",   "spread", "focus",  "kill",   "assist",    "help",
    "heal",    "rez",    "res",    "ress",   "mana",      "oom",
    "drink",   "water",  "food",   "summon", "follow",    "come",
    "move",    "kite",   "taunt",  "aoe",    "adds",      "add",
    "careful", "fire",   "behind", "lust",   "bloodlust", "hero",
    "heroism", "bl",     "resume", "burn",   "nuke",      "cease",
    "skull",   "cross",  "star",   "moon",   "square",    "triangle",
    "diamond", "circle",
};

// The unprompted self-repeat filter: a bot doesn't say a line again, unasked,
// within 5 minutes of saying it.
constexpr base::TimeDelta kSelfRepeatWindow = base::Minutes(5);
constexpr size_t kMaxSaidLines = 32;

std::string NormalizeSaid(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (IsAsciiWordChar(c)) {
      out.push_back(base::ToLowerASCII(c));
    } else if (!out.empty() && out.back() != ' ') {
      out.push_back(' ');
    }
  }
  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

bool AddressesClassOrRole(std::string_view text,
                          std::string_view class_name,
                          pb::Role role) {
  for (const AddressWord& w : kAddressWords) {
    const bool matches =
        w.class_name.empty()
            ? (role != pb::Role::ROLE_UNSPECIFIED && w.role == role)
            : base::EqualsCaseInsensitiveASCII(w.class_name, class_name);
    if (matches && ContainsWholeWordIgnoringCase(text, w.word)) {
      return true;
    }
  }
  return false;
}

bool IsGroupCall(std::string_view text) {
  return std::any_of(std::begin(kGroupCallWords), std::end(kGroupCallWords),
                     [&](std::string_view w) {
                       return ContainsWholeWordIgnoringCase(text, w);
                     });
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

BotLoop::PartyLineAddressees BotLoop::AddresseesOf(
    const std::string& bot,
    std::string_view text,
    const pb::Snapshot& snapshot) const {
  // The online brain bots in this party (this harness runs them, and their
  // snapshots are fresh), this one included (T15: an offline or stalled bot
  // must not be the one that answers).
  PartyLineAddressees out;
  out.online.push_back(bot);
  const auto add_members = [&](const std::vector<pb::Unit>& members) {
    for (const pb::Unit& m : members) {
      if (m.name.empty() || m.name == bot) {
        continue;
      }
      const auto it = bots_.find(m.name);
      if (it != bots_.end() && it->second.latest_snapshot &&
          !it->second.latest_received_at.is_null() &&
          base::TimeTicks::Now() - it->second.latest_received_at <=
              config_.stale_snapshot_after) {
        out.online.push_back(m.name);
      }
    }
  };
  if (snapshot.ext.party) {
    add_members(snapshot.ext.party->members);
  }
  if (snapshot.ext.raid) {
    add_members(snapshot.ext.raid->members);
  }
  std::sort(out.online.begin(), out.online.end());
  out.online.erase(std::unique(out.online.begin(), out.online.end()),
                   out.online.end());
  // A bot's class and role as it sees itself, else as this bot's group frame
  // shows it.
  const auto class_and_role = [&](const std::string& name) {
    std::string class_name;
    pb::Role role = pb::Role::ROLE_UNSPECIFIED;
    const auto take = [&](const pb::Unit& u) {
      if (class_name.empty() && u.player) {
        class_name = u.player->class_;
      }
      if (role == pb::Role::ROLE_UNSPECIFIED && u.group) {
        role = u.group->role;
      }
    };
    if (name == bot) {
      take(snapshot.base.self.unit);
    } else if (const auto it = bots_.find(name);
               it != bots_.end() && it->second.latest_snapshot) {
      take(it->second.latest_snapshot->base.self.unit);
    }
    for (const auto* group :
         {snapshot.ext.party ? &snapshot.ext.party->members : nullptr,
          snapshot.ext.raid ? &snapshot.ext.raid->members : nullptr}) {
      if (!group) {
        continue;
      }
      for (const pb::Unit& m : *group) {
        if (m.name == name) {
          take(m);
        }
      }
    }
    return std::pair(class_name, role);
  };
  for (const std::string& name : out.online) {
    if (ContainsWholeWordIgnoringCase(text, name)) {
      out.addressed.push_back(name);
    }
  }
  for (const std::string& name : out.online) {
    if (std::find(out.addressed.begin(), out.addressed.end(), name) !=
        out.addressed.end()) {
      continue;
    }
    const auto [class_name, role] = class_and_role(name);
    if (AddressesClassOrRole(text, class_name, role)) {
      out.addressed.push_back(name);
    }
  }
  return out;
}

bool BotLoop::WakesForPartyLine(const std::string& bot,
                                std::string_view text,
                                const std::string& from,
                                const pb::Snapshot& snapshot) const {
  const PartyLineAddressees a = AddresseesOf(bot, text, snapshot);
  if (!a.addressed.empty()) {
    return std::find(a.addressed.begin(), a.addressed.end(), bot) !=
           a.addressed.end();
  }
  // A brain bot's own party/raid line is never a call for the whole group.
  const bool from_human = !from.empty() && !bots_.contains(from);
  if (from_human && IsGroupCall(text)) {
    return true;
  }
  return a.online.front() == bot;
}

bool BotLoop::PlaysWithHuman(const pb::Snapshot& snapshot) const {
  const auto is_human = [&](const pb::Unit& m) {
    return m.group && m.group->online && !m.name.empty() &&
           m.name != snapshot.bot && !bots_.contains(m.name);
  };
  return (snapshot.ext.party &&
          std::any_of(snapshot.ext.party->members.begin(),
                      snapshot.ext.party->members.end(), is_human)) ||
         (snapshot.ext.raid &&
          std::any_of(snapshot.ext.raid->members.begin(),
                      snapshot.ext.raid->members.end(), is_human));
}

bool BotLoop::AnswersPartyLine(const std::string& bot,
                               std::string_view text,
                               const pb::Snapshot& snapshot) const {
  // One responder per line (Now #10): even if several bots are addressed (and
  // woken), only the first of them answers.
  const PartyLineAddressees a = AddresseesOf(bot, text, snapshot);
  return (a.addressed.empty() ? a.online.front() : a.addressed.front()) == bot;
}

bool BotLoop::AnswersPartyChat(const std::string& bot,
                               const std::vector<BotState::ObservedChat>& chats,
                               const pb::Snapshot& snapshot) const {
  bool any_group_line = false;
  for (const BotState::ObservedChat& c : chats) {
    if (c.triggered_wake && IsGroupSharedChatChannel(c.channel, snapshot)) {
      any_group_line = true;
      if (AnswersPartyLine(bot, c.text, snapshot)) {
        return true;
      }
    }
  }
  return !any_group_line;  // Not woken by party chat: nothing to share out.
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
      ResetBotSession(&state, /*cancel_in_flight=*/false);
      scheduler_.SetHumanParty(name, false);
    }
    party_chat_budgets_.clear();
    // Answers to screens of the old connection are useless: free the slots.
    scheduler_.CancelAllInFlight();
    // Server resets its own health flag on disconnect; start clean on the next
    // connection.
    consecutive_llm_failures_ = 0;
    llm_healthy_ = true;
  }
}

void BotLoop::SetLlmHealthy(bool healthy) {
  if (llm_healthy_ == healthy) {
    return;
  }
  llm_healthy_ = healthy;
  Log(healthy
          ? "harness: LLM recovered; restoring brain popup ownership"
          : "harness: LLM unhealthy; handing popup ownership to playerbots");
  pb::Command cmd;
  cmd.health = pb::Command::Health{.llm_healthy = healthy};
  send_command_.Run(cmd);
  ++commands_sent_;
}

void BotLoop::RecordLlmFailure(bool immediate_unhealthy) {
  if (config_.llm_unhealthy_after_failures <= 0) {
    return;
  }
  if (immediate_unhealthy) {
    consecutive_llm_failures_ = std::max(consecutive_llm_failures_ + 1,
                                         config_.llm_unhealthy_after_failures);
  } else {
    ++consecutive_llm_failures_;
  }
  if (consecutive_llm_failures_ >= config_.llm_unhealthy_after_failures) {
    SetLlmHealthy(false);
  }
}

void BotLoop::RecordLlmSuccess() {
  consecutive_llm_failures_ = 0;
  SetLlmHealthy(true);
}

// static
void BotLoop::ResetBotSession(BotState* state, bool cancel_in_flight) {
  state->runner.Stop();
  state->triggers = {};
  state->orders.Clear();
  state->plan_acts = {};
  state->latest_snapshot.reset();
  state->look_deadline = base::TimeTicks();
  state->has_ever_woken = false;
  state->was_in_instance = false;
  state->last_map.clear();
  state->notes.clear();
  state->last_selected_notes.clear();
  state->recent_chat_command_ids.clear();
  state->pending_says.clear();
  state->said_lines.clear();
  state->call_started_at = base::TimeTicks();
  state->call_state = BotState::CallState::kIdle;
  state->queued_urgency = Urgency::kP2Routine;
  if (state->in_flight_cancel) {
    auto cancel = std::move(state->in_flight_cancel);
    if (cancel_in_flight) {
      cancel->Cancel();
    }
  }
  state->call_deadline = base::TimeTicks();
  state->call_timed_out_epoch = 0;
  state->early_steps_dispatched = false;
  state->early_step_failed = false;
  state->step_refusals = 0;
  state->invalid_plan_retries = 0;
  state->last_outcome.clear();
  state->order_wakes = 0;
  state->order_quiet_since = base::TimeTicks();
  state->pending_reasons.clear();
  state->pending_chats.clear();
  state->chat_wake_times_by_sender.clear();
  state->deferred_chat_at = base::TimeTicks();
  state->recent_events.clear();
  state->whisperers.clear();
  state->player_chat_memory.clear();
  state->player_chat_memory_seq = 0;
  state->condense_state = BotState::CondenseState::kIdle;
  ++state->memory_condense_epoch;
  state->condense_retry_after = base::TimeTicks();
  ResetLooking(state);
  ++state->call_epoch;
}

void BotLoop::NoteEvent(BotState* state,
                        const pb::Event& event,
                        bool triggered_wake,
                        base::TimeTicks at) {
  NoteText(state, DescribeEvent(event));
  if (event.chat) {
    const size_t cap = std::max<size_t>(config_.max_recent_events, 1) * 2;
    if (state->pending_chats.size() >= cap) {
      state->pending_chats.erase(state->pending_chats.begin());
    }
    state->pending_chats.push_back({event.chat->channel, event.chat->from,
                                    event.chat->text, event.chat->at_me,
                                    triggered_wake, at});
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

void BotLoop::NotePlayerExchange(const std::string& bot,
                                 BotState* state,
                                 const pb::Event& event) const {
  if (!event.chat ||
      !IsConversationalPlayerChannel(event.chat->channel, event.chat->at_me) ||
      event.chat->from.empty() || event.chat->text.empty() ||
      base::EqualsCaseInsensitiveASCII(event.chat->from, bot)) {
    return;
  }
  RecordPlayerExchange(state, event.chat->from, /*from_bot=*/false,
                       event.chat->channel, event.chat->text);
}

void BotLoop::RecordPlayerExchange(BotState* state,
                                   std::string_view player,
                                   bool from_bot,
                                   pb::ChatChannel channel,
                                   std::string_view text) const {
  if (config_.max_player_memory_players == 0 ||
      config_.max_player_memory_exchanges == 0) {
    return;
  }
  const std::string_view trimmed_player =
      base::TrimWhitespaceASCII(player, base::TRIM_ALL);
  const std::string_view trimmed_text =
      base::TrimWhitespaceASCII(text, base::TRIM_ALL);
  if (trimmed_player.empty() || trimmed_text.empty()) {
    return;
  }
  BotState::PlayerChatMemory* entry = nullptr;
  for (BotState::PlayerChatMemory& existing : state->player_chat_memory) {
    if (base::EqualsCaseInsensitiveASCII(existing.player, trimmed_player)) {
      entry = &existing;
      break;
    }
  }
  if (!entry) {
    if (state->player_chat_memory.size() >= config_.max_player_memory_players) {
      auto lru = std::min_element(state->player_chat_memory.begin(),
                                  state->player_chat_memory.end(),
                                  [](const BotState::PlayerChatMemory& a,
                                     const BotState::PlayerChatMemory& b) {
                                    return a.last_touch_seq < b.last_touch_seq;
                                  });
      state->player_chat_memory.erase(lru);
    }
    entry = &state->player_chat_memory.emplace_back(
        BotState::PlayerChatMemory{std::string(trimmed_player), {}, 0});
  }
  entry->last_touch_seq = ++state->player_chat_memory_seq;
  if (!entry->exchanges.empty()) {
    const BotState::PlayerExchange& last = entry->exchanges.back();
    if (last.from_bot == from_bot && last.channel == channel &&
        last.text == trimmed_text) {
      return;
    }
  }
  entry->exchanges.push_back(BotState::PlayerExchange{
      .from_bot = from_bot,
      .channel = channel,
      .text = std::string(trimmed_text),
  });
  while (entry->exchanges.size() > config_.max_player_memory_exchanges) {
    entry->exchanges.pop_front();
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
    NoteEvent(&state, ev, wakes, received_at);
    NoteWhisperer(&state, ev, received_at);
    NotePlayerExchange(bot, &state, ev);
  }
  Screen screen = RenderScreen(snapshot);
  const std::optional<WakeTrigger> trig =
      state.triggers.EvaluateSnapshot(snapshot);
  scheduler_.SetHumanParty(bot, PlaysWithHuman(snapshot));
  state.latest_snapshot = std::move(snapshot);
  state.latest_screen = std::move(screen);
  state.latest_received_at = received_at;

  const bool look_woke = OnLookSnapshot(bot, &state, received_at);

  if (state.plan_acts.active()) {
    if (state.latest_snapshot->done_act_command_id ==
        state.plan_acts.pending_command_id) {
      SendNextPlanAct(bot, &state, received_at);
    } else if (received_at - state.plan_acts.sent_at >= kPlanActTimeout) {
      Log(base::StrCat({"harness: ", bot, " plan act ",
                        std::to_string(state.plan_acts.pending_command_id),
                        " timed out; going on with its do: steps"}));
      FinishPlanActs(bot, &state, received_at);
    }
  }

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
    // Only genuine interrupts (P0 combat/death/control/hp, returning alive, or
    // watched enemy player nearby) abort the running plan immediately. Non-P0
    // alerts (party state, spell misses, out-of-combat mana) wake the brain
    // without stopping the active step while it decides.
    const bool stops_plan =
        trig->urgency == Urgency::kP0Combat ||
        (trig->kinds & (kWakeAlive | kWakeEnemyPlayer)) != 0;
    if (stops_plan) {
      state.runner.Stop();
      state.look_deadline = base::TimeTicks();
    }
    WakeBot(bot, trig->urgency, trig->reason);
    return;
  }
  if (!ord.wake_reason.empty()) {
    return;
  }
  // While a plan's acts run, its steps wait (a new Choose would cancel them).
  if (look_woke || state.call_in_flight() || state.has_queued_wake() ||
      ord.covered || state.plan_acts.active()) {
    return;
  }
  const RunnerAction action = state.runner.OnSnapshot(
      *state.latest_snapshot, state.latest_screen, received_at);
  if (action.step_done) {
    state.step_refusals = 0;
    state.last_outcome.clear();
  }
  DispatchRunnerAction(bot, &state, action);
  if (!state.has_queued_wake() && !state.call_in_flight() &&
      !state.look_deadline.is_null() && received_at >= state.look_deadline &&
      !(state.pending_reasons.empty() &&
        WaitingOnAfkMember(*state.latest_snapshot))) {
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

base::TimeDelta InvalidPlanBackoff(int failures) {
  constexpr int kBaseSeconds = 10;
  constexpr int kMaxSeconds = 120;
  const int n = std::max(failures, 1);
  const int shift = std::min(n - 1, 4);
  return base::Seconds(std::min(kBaseSeconds << shift, kMaxSeconds));
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
  std::optional<WakeTrigger> chat_trig =
      is_player_chat ? EvaluateEventTrigger(imm.event, state.triggers.watches())
                     : std::nullopt;
  // T9 & Now #10: a group or nearby party-shared line wakes only the bots it
  // is for (addressed, or for a human's group call every bot of a small group,
  // else the group's single responder); the others just hear it.
  if (chat_trig && state.latest_snapshot &&
      IsGroupSharedChatChannel(imm.event.chat->channel,
                               *state.latest_snapshot) &&
      !WakesForPartyLine(bot, imm.event.chat->text, imm.event.chat->from,
                         *state.latest_snapshot)) {
    chat_trig.reset();
  }
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
      // Not dropped: answered once the window frees a wake (the 4th quick
      // "STOP" must still get through), without stopping the plan.
      const base::TimeTicks retry_at = times.front() + kChatWakeWindow;
      if (state.deferred_chat_at.is_null() ||
          retry_at < state.deferred_chat_at) {
        state.deferred_chat_at = retry_at;
      }
      if (std::find(state.pending_reasons.begin(), state.pending_reasons.end(),
                    chat_trig->reason) == state.pending_reasons.end()) {
        state.pending_reasons.push_back(chat_trig->reason);
      }
    } else {
      times.push_back(received_at);
    }
  }
  NoteEvent(&state, imm.event,
            /*triggered_wake=*/chat_trig.has_value() && !chat_rate_limited,
            received_at);
  NoteWhisperer(&state, imm.event, received_at);
  NotePlayerExchange(bot, &state, imm.event);
  if (chat_rate_limited) {
    return;
  }
  if (imm.event.error && imm.event.error->command_id != 0) {
    if (state.plan_acts.active() &&
        imm.event.error->command_id == state.plan_acts.pending_command_id) {
      // A refused plan act: the rest of the line is dropped (its later acts
      // assumed this one) and the held steps go ahead; the brain reads the
      // error (NoteEvent above) on its next look.
      Log(base::StrCat(
          {"harness: ", bot, " plan act refused: ", imm.event.error->text}));
      state.plan_acts.left.clear();
      FinishPlanActs(bot, &state, received_at);
      return;
    }
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
      if (state.call_in_flight()) {
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
      if (state.call_in_flight()) {
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
  // Only P0 combat/emergency events abort a running plan immediately (step
  // refusals already stopped the runner above). Player chat and non-P0 alerts
  // (prompts, loot rolls, raid warnings) let the active step keep running
  // while the brain wakes and decides whether to change course.
  if (!is_player_chat && trig->urgency == Urgency::kP0Combat) {
    state.runner.Stop();
    state.look_deadline = base::TimeTicks();
  } else if (refusal) {
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
      ResetBotSession(&state, /*cancel_in_flight=*/true);
      scheduler_.SetHumanParty(bot, false);
      continue;
    }
    while (!state.pending_says.empty() &&
           now >= state.pending_says.front().due_at) {
      BotState::PendingSay next = std::move(state.pending_says.front());
      state.pending_says.pop_front();
      SendSay(bot, next.channel, next.to, next.text);
    }
    // Enforce overall wall-clock deadline on stalled in-flight LLM calls
    // (A-M13) so a wedged llama-server never blocks a bot for 60 s.
    if (state.call_in_flight() && !state.call_deadline.is_null() &&
        now >= state.call_deadline) {
      state.call_deadline = base::TimeTicks();
      state.call_timed_out_epoch = state.call_epoch;
      RecordLlmFailure(/*immediate_unhealthy=*/true);
      if (state.in_flight_cancel) {
        auto cancel = std::move(state.in_flight_cancel);
        cancel->Cancel();
      }
      continue;
    }
    if (!state.latest_snapshot ||
        state.call_state != BotState::CallState::kIdle) {
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
    if (!state.deferred_chat_at.is_null() && now >= state.deferred_chat_at) {
      state.deferred_chat_at = base::TimeTicks();
      if (!state.pending_reasons.empty()) {
        // A rate-limited chat line, kept in pending_reasons: chat never stops
        // a running plan (A-M8).
        WakeBot(bot, Urgency::kP3Chat, "", /*preserve_backoffs=*/true);
        continue;
      }
    }
    if (!state.look_deadline.is_null() && now >= state.look_deadline &&
        !(state.pending_reasons.empty() &&
          WaitingOnAfkMember(*state.latest_snapshot))) {
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
    state.invalid_plan_retries = 0;
  }
  if (!reason.empty() &&
      std::find(state.pending_reasons.begin(), state.pending_reasons.end(),
                reason) == state.pending_reasons.end()) {
    state.pending_reasons.push_back(reason);
  }
  state.queued_urgency = state.has_queued_wake()
                             ? std::min(state.queued_urgency, urgency)
                             : urgency;
  state.MarkQueued();
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
      it->second.ClearQueued();
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
  state.queued_urgency = Urgency::kP2Routine;
  state.MarkStarted();
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
  }
  for (const BotState::ObservedChat& c : asked_chats) {
    if (IsPlayerChatChannel(c.channel)) {
      ctx.extra_present_players.push_back(c.from);
    }
  }
  if (!state.player_chat_memory.empty()) {
    std::vector<const BotState::PlayerChatMemory*> ordered;
    ordered.reserve(state.player_chat_memory.size());
    for (const BotState::PlayerChatMemory& mem : state.player_chat_memory) {
      ordered.push_back(&mem);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const BotState::PlayerChatMemory* a,
                 const BotState::PlayerChatMemory* b) {
                return a->last_touch_seq > b->last_touch_seq;
              });
    ctx.player_chat_memory.reserve(ordered.size());
    for (const BotState::PlayerChatMemory* mem : ordered) {
      PlayerChatHistory hist;
      hist.player = mem->player;
      hist.turns.reserve(mem->exchanges.size());
      for (const BotState::PlayerExchange& ex : mem->exchanges) {
        hist.turns.push_back(PlayerChatTurn{
            .from_bot = ex.from_bot,
            .channel = ChatChannelLabel(ex.channel),
            .text = ex.text,
        });
      }
      ctx.player_chat_memory.push_back(std::move(hist));
    }
  }
  ctx.current_goal = state.current_goal;
  ctx.wake_reason = wake_reason;
  const base::TimeTicks effective_ask_time =
      state.latest_received_at.is_null()
          ? asked_at_for_whisperers
          : std::max(asked_at_for_whisperers, state.latest_received_at);
  const bool prompted_by_chat =
      std::any_of(asked_chats.begin(), asked_chats.end(),
                  [](const BotState::ObservedChat& c) {
                    return c.triggered_wake && IsPlayerChatChannel(c.channel);
                  });
  if (!AnswersPartyChat(bot, asked_chats, *state.latest_snapshot) ||
      (!prompted_by_chat && IsPartyChatBudgetMuted(bot, *state.latest_snapshot,
                                                   effective_ask_time))) {
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
  state.call_started_at = std::max(asked_at, state.latest_received_at);
  if (call_limit.is_positive()) {
    request.timeout_ms = static_cast<int>(call_limit.InMilliseconds());
    state.call_deadline = state.call_started_at + call_limit;
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
  if (!state.call_in_flight() || state.call_epoch != epoch ||
      (state.has_queued_wake() && state.queued_urgency == Urgency::kP0Combat) ||
      !state.latest_snapshot) {
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
      it->second.MarkFinished();
      it->second.in_flight_cancel.reset();
      it->second.call_deadline = base::TimeTicks();
      was_timeout = it->second.call_timed_out_epoch == epoch;
      Requeue(&it->second, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_chats), std::move(asked_outcome));
      if (was_timeout && !it->second.has_queued_wake()) {
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
    state.MarkFinished();
    state.in_flight_cancel.reset();
    state.call_deadline = base::TimeTicks();
    if (!decision.result.reply) {
      RecordLlmFailure(/*immediate_unhealthy=*/false);
      Log(base::StringPrintf("harness: %s brain error (%.0f ms): %s",
                             bot.c_str(), decision.result.round_trip_ms,
                             decision.result.error.c_str()));
      state.look_deadline = now + base::Seconds(10);
      Requeue(&state, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_chats), std::move(asked_outcome));
    } else if (!plan) {
      const base::TimeDelta backoff =
          InvalidPlanBackoff(++state.invalid_plan_retries);
      Log(base::StringPrintf(
          "harness: %s invalid plan (%.0f ms); retry in %.0f s", bot.c_str(),
          decision.result.round_trip_ms, backoff.InSecondsF()));
      // The streamed first line already started steps: an invalid plan keeps
      // none of them (harness #12).
      if (state.early_steps_dispatched) {
        state.early_steps_dispatched = false;
        state.runner.Stop();
        SendStopMoving(bot);
      }
      state.look_deadline = now + backoff;
      Requeue(&state, std::move(asked_reasons), std::move(asked_events),
              std::move(asked_chats), std::move(asked_outcome));
    } else {
      RecordLlmSuccess();
      state.invalid_plan_retries = 0;
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
      // Only a queued P0 combat/emergency wake supersedes this in-flight reply
      // and drops its steps/watches. Non-P0 wakes (chat, party status, prompts,
      // routine alerts) must not cause wake storms that drop every reply: this
      // plan still installs, and the queued wake runs next to adjust course.
      // When an early-dispatched step of this same call failed before the reply
      // finished, still deliver say/tell and watches, and keep the refusal wake
      // or backoff timer instead of requeueing the already-answered reasons.
      const bool superseded =
          state.has_queued_wake() && state.queued_urgency == Urgency::kP0Combat;
      if ((!superseded || state.early_step_failed) && state.latest_snapshot) {
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
        const base::TimeTicks effective_now =
            state.latest_received_at.is_null()
                ? now
                : std::max(now, state.latest_received_at);
        const bool prompted_by_chat = std::any_of(
            asked_chats.begin(), asked_chats.end(),
            [](const BotState::ObservedChat& c) {
              return c.triggered_wake && IsPlayerChatChannel(c.channel);
            });
        const bool party_reply_muted = state.party_reply_muted_epoch == epoch;
        // The brain's thinking time counts toward the human typing delay: it
        // runs from the line that woke the bot (or the call's start), so a
        // short ack to a command lands about as fast as a person's.
        base::TimeTicks wake_started_at;
        for (const BotState::ObservedChat& c : asked_chats) {
          if (c.triggered_wake && !c.at.is_null() &&
              (wake_started_at.is_null() || c.at < wake_started_at)) {
            wake_started_at = c.at;
          }
        }
        if (wake_started_at.is_null()) {
          wake_started_at = state.call_started_at;
        }
        const base::TimeDelta waited =
            wake_started_at.is_null()
                ? base::TimeDelta()
                : std::max(base::TimeDelta(), effective_now - wake_started_at);
        // Every line goes through here: unasked, a bot doesn't repeat itself
        // within kSelfRepeatWindow.
        const auto say_line = [&](pb::ChatChannel channel,
                                  const std::string& to,
                                  const std::string& text, bool group_budget) {
          if (!prompted_by_chat &&
              (IsSelfRepeat(state, text, effective_now) ||
               (group_budget && WaitingOnAfkMember(*state.latest_snapshot)))) {
            return;
          }
          if (group_budget &&
              !AllowAndRecordPartyChat(bot, *state.latest_snapshot, text,
                                       prompted_by_chat, effective_now)) {
            return;
          }
          QueueOrSendSay(bot, &state, channel, to, text, effective_now, waited);
          RecordSaid(&state, text, effective_now);
          if (channel == pb::WHISPER && !to.empty()) {
            RecordPlayerExchange(&state, to, /*from_bot=*/true, channel, text);
          } else if (channel == pb::PARTY || channel == pb::RAID ||
                     channel == pb::SAY) {
            std::vector<std::string> targets;
            for (auto it = asked_chats.rbegin(); it != asked_chats.rend();
                 ++it) {
              if (!IsConversationalPlayerChannel(it->channel, it->at_me) ||
                  it->from.empty() ||
                  base::EqualsCaseInsensitiveASCII(it->from, bot)) {
                continue;
              }
              const bool seen = std::any_of(
                  targets.begin(), targets.end(), [&](const std::string& t) {
                    return base::EqualsCaseInsensitiveASCII(t, it->from);
                  });
              if (!seen) {
                targets.push_back(it->from);
                if (targets.size() >= 2) {
                  break;
                }
              }
            }
            for (const std::string& target : targets) {
              RecordPlayerExchange(&state, target, /*from_bot=*/true, channel,
                                   text);
            }
          }
        };
        if (!plan->say.empty() && !party_reply_muted) {
          // Answer a party/raid line in that channel (RH-L1, A-M14: decided by
          // the structured chat event's channel, never by splitting strings).
          pb::ChatChannel ch = pb::SAY;
          if (HasGroupMembers(*state.latest_snapshot)) {
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
            say_line(ch, "", plan->say, /*group_budget=*/true);
          }
        }
        const bool in_party =
            state.latest_snapshot->ext.party &&
            !state.latest_snapshot->ext.party->members.empty();
        // The module sets `ext.raid` for raid groups (members stay in
        // `ext.party`, T13).
        const bool in_raid = state.latest_snapshot->ext.raid.has_value();
        if (!plan->party.empty() && (in_party || in_raid) &&
            !party_reply_muted) {
          say_line(in_raid ? pb::RAID : pb::PARTY, "", plan->party,
                   /*group_budget=*/true);
        }
        if (!plan->tell.empty()) {
          say_line(pb::WHISPER, plan->tell_to, plan->tell,
                   /*group_budget=*/false);
        }
        // Public channels (F1): a refusal (not queued, wrong place) comes back
        // as a chat error event, which wakes the brain to see it.
        if (!plan->general.empty()) {
          say_line(pb::GENERAL, "", plan->general, /*group_budget=*/false);
        }
        if (!plan->lfg.empty()) {
          say_line(pb::LFG, "", plan->lfg, /*group_budget=*/false);
        }
        if (!plan->remember.empty() && !config_.bots_dir.empty()) {
          // Trust by sender, not keywords: what the party (and its leader)
          // teaches is kept as is ("always heal me first"); only lines from
          // players outside the group go through the injection filter.
          std::flat_set<std::string> party;
          if (state.latest_snapshot && state.latest_snapshot->ext.party) {
            for (const pb::Unit& m :
                 state.latest_snapshot->ext.party->members) {
              party.insert(m.name);
            }
          }
          if (state.latest_snapshot && state.latest_snapshot->ext.raid) {
            for (const pb::Unit& m : state.latest_snapshot->ext.raid->members) {
              party.insert(m.name);
            }
          }
          std::vector<std::string> player_chat_texts;
          for (const BotState::ObservedChat& c : asked_chats) {
            if (IsPlayerChatChannel(c.channel) && !party.contains(c.from)) {
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
        // The `act:` line goes first: a Choose other than Keep or Attack
        // would cancel it on the module, so such `do:` steps wait for it.
        // Steps already started from the streamed first line keep running
        // (the act, sent after them, isn't cancelled by them).
        StartPlanActs(bot, &state, plan->acts, now);
        if (!state.early_steps_dispatched) {
          const std::optional<OptionKind> first =
              PlanRunner::FirstStepKind(plan->steps, ask_screen);
          if (state.plan_acts.active() && first &&
              *first != OptionKind::kKeep && *first != OptionKind::kAttack) {
            state.runner.Stop();
            state.plan_acts.held = BotState::PlanActs::HeldSteps{
                .steps = plan->steps, .ask_screen = ask_screen};
          } else {
            const RunnerAction action =
                state.runner.Start(plan->steps, ask_seq, ask_screen, now);
            DispatchRunnerAction(bot, &state, action);
          }
        }
        if (state.order_failure_during_call) {
          state.order_failure_during_call = false;
          WakeForOrder(bot, &state, "", now, /*is_failure=*/true);
        }
        if (!state.has_queued_wake() && state.latest_snapshot &&
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
  if (!in_combat) {
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
  }

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
  if (state->call_state != BotState::CallState::kIdle) {
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
  state->runner.Stop();
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
    // WakeForOrder stops the plan only when it really wakes: a wake held back
    // by backoff leaves the plan running (`every 2s -> wake` must not idle
    // the bot between looks).
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

void BotLoop::StartPlanActs(const std::string& bot,
                            BotState* state,
                            const std::vector<Act>& acts,
                            base::TimeTicks now) {
  // A new plan replaces an older plan's acts and the steps they held.
  state->plan_acts = {};
  if (acts.empty()) {
    return;
  }
  state->plan_acts.left.assign(acts.begin(), acts.end());
  SendNextPlanAct(bot, state, now);
}

void BotLoop::SendNextPlanAct(const std::string& bot,
                              BotState* state,
                              base::TimeTicks now) {
  BotState::PlanActs& pa = state->plan_acts;
  pa.pending_command_id = 0;
  while (!pa.left.empty()) {
    const Act act = std::move(pa.left.front());
    pa.left.pop_front();
    // A plan has no `when`: no `it`. The act's thing resolves on the newest
    // snapshot by the standing orders' rules.
    const std::string target_guid =
        state->latest_snapshot
            ? ResolvePlanActTargetGuid(*state->latest_snapshot, act, now)
            : std::string();
    std::optional<pb::Command::Act> wire =
        OrderActToCommand(act, "", "", target_guid);
    if (!wire || wire->verb == pb::Command::Act::WAKE) {
      // `wake` (or anything the wire can't carry) does nothing here: the
      // brain is the one that just answered.
      continue;
    }
    if (wire->verb == pb::Command::Act::TRADE_GOLD) {
      const bool in_trade = state->latest_snapshot &&
                            state->latest_snapshot->ext.trade &&
                            !state->latest_snapshot->ext.trade->with.empty();
      const uint32_t money =
          state->latest_snapshot ? state->latest_snapshot->base.self.money : 0;
      if (!in_trade) {
        state->last_outcome = "no trade window is open";
        pa.left.clear();
        pa.held.reset();
        break;
      }
      if (wire->copper == 0 || wire->copper > money) {
        state->last_outcome = wire->copper == 0 ? "invalid trade gold amount"
                                                : "not enough money";
        pa.left.clear();
        pa.held.reset();
        break;
      }
    }
    pa.pending_command_id = SendAct(bot, std::move(*wire));
    pa.sent_at = now;
    return;
  }
  FinishPlanActs(bot, state, now);
}

void BotLoop::FinishPlanActs(const std::string& bot,
                             BotState* state,
                             base::TimeTicks now) {
  std::optional<BotState::PlanActs::HeldSteps> held =
      std::move(state->plan_acts.held);
  state->plan_acts = {};
  // Only a queued P0 emergency wake replaces the held steps (non-P0 wakes do
  // not supersede an active plan).
  if (!held || state->call_in_flight() ||
      (state->has_queued_wake() &&
       state->queued_urgency == Urgency::kP0Combat) ||
      !state->latest_snapshot) {
    return;
  }
  const RunnerAction action = state->runner.StartOn(
      held->steps, held->ask_screen, state->latest_snapshot->seq,
      state->latest_screen, now);
  DispatchRunnerAction(bot, state, action);
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

void BotLoop::QueueOrSendSay(const std::string& bot,
                             BotState* state,
                             pb::ChatChannel channel,
                             const std::string& to,
                             const std::string& text,
                             base::TimeTicks now,
                             base::TimeDelta already_waited) {
  // Only the first line of a reply had the brain's thinking time to type in;
  // a line queued behind another is typed after it, in full.
  const base::TimeDelta delay = ComputeTypingDelay(
      text.size(), config_.typing_delay_min, config_.typing_delay_max,
      state->pending_says.empty() ? already_waited : base::TimeDelta());
  if (!delay.is_positive()) {
    SendSay(bot, channel, to, text);
    return;
  }
  static constexpr size_t kMaxPendingSays = 4;
  if (state->pending_says.size() >= kMaxPendingSays) {
    return;
  }
  const base::TimeTicks start =
      state->pending_says.empty()
          ? now
          : std::max(now, state->pending_says.back().due_at);
  state->pending_says.push_back({start + delay, channel, to, text});
}

// static
bool BotLoop::IsSelfRepeat(const BotState& state,
                           std::string_view text,
                           base::TimeTicks now) {
  const std::string norm = NormalizeSaid(text);
  return !norm.empty() &&
         std::any_of(state.said_lines.begin(), state.said_lines.end(),
                     [&](const BotState::SaidLine& l) {
                       return l.text == norm && now - l.at <= kSelfRepeatWindow;
                     });
}

// static
void BotLoop::RecordSaid(BotState* state,
                         std::string_view text,
                         base::TimeTicks now) {
  std::string norm = NormalizeSaid(text);
  if (norm.empty()) {
    return;
  }
  auto& lines = state->said_lines;
  while (!lines.empty() && (now - lines.front().at > kSelfRepeatWindow ||
                            lines.size() >= kMaxSaidLines)) {
    lines.pop_front();
  }
  lines.push_back({std::move(norm), now});
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

bool BotLoop::IsPartyChatBudgetMuted(const std::string& bot,
                                     const pb::Snapshot& snapshot,
                                     base::TimeTicks now) const {
  const std::string key = PartyBudgetKey(bot, snapshot);
  if (key.empty()) {
    return false;
  }
  const auto it = party_chat_budgets_.find(key);
  if (it == party_chat_budgets_.end() || it->second.unprompted_times.empty()) {
    return false;
  }
  const auto& times = it->second.unprompted_times;
  if (now - times.back() < kMinUnpromptedPartySpacing) {
    return true;
  }
  const size_t recent = static_cast<size_t>(std::count_if(
      times.begin(), times.end(),
      [&](base::TimeTicks t) { return now - t <= kPartyBudgetWindow; }));
  return recent >= kMaxUnpromptedPartyLinesPerWindow;
}

bool BotLoop::AllowAndRecordPartyChat(const std::string& bot,
                                      const pb::Snapshot& snapshot,
                                      std::string_view text,
                                      bool prompted_by_player_chat,
                                      base::TimeTicks now) {
  const std::string norm =
      base::ToLowerASCII(base::TrimWhitespaceASCII(text, base::TRIM_ALL));
  if (norm.empty()) {
    return false;
  }
  const std::string key = PartyBudgetKey(bot, snapshot);
  if (key.empty()) {
    return true;
  }
  if (party_chat_budgets_.size() >= 64 && !party_chat_budgets_.contains(key)) {
    std::erase_if(party_chat_budgets_, [&](const auto& kv) {
      return kv.second.unprompted_times.empty() ||
             now - kv.second.unprompted_times.back() > kPartyBudgetWindow;
    });
  }
  PartyChatBudget& budget = party_chat_budgets_[key];
  while (!budget.unprompted_times.empty() &&
         now - budget.unprompted_times.front() > kPartyBudgetWindow) {
    budget.unprompted_times.pop_front();
  }
  if (!prompted_by_player_chat) {
    if (!budget.last_text_lower.empty() && norm == budget.last_text_lower &&
        now - budget.last_text_at <= kPartyBudgetWindow) {
      return false;  // Suppress duplicate party echo ("grats", "oom", ...).
    }
    if (budget.unprompted_times.size() >= kMaxUnpromptedPartyLinesPerWindow ||
        (!budget.unprompted_times.empty() &&
         now - budget.unprompted_times.back() < kMinUnpromptedPartySpacing)) {
      return false;
    }
    budget.unprompted_times.push_back(now);
  }
  budget.last_text_lower = norm;
  budget.last_text_at = now;
  return true;
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
      state->call_state != BotState::CallState::kIdle ||
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
      "lines, each starting with '- ', plain facts only, no preamble. Keep a "
      "trailing '[heard in chat]' tag on any line made from notes that carry "
      "it.";
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
