// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/leader_command.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chat/chat_text.h"

namespace llm_brain {
namespace {

struct Phrase {
  std::string_view words;
  LeaderCommand command;
};

constexpr Phrase kPhrases[] = {
    {"wait", LeaderCommand::kWait},
    {"wait here", LeaderCommand::kWait},
    {"stay", LeaderCommand::kWait},
    {"stay here", LeaderCommand::kWait},
    {"hold", LeaderCommand::kWait},
    {"hold up", LeaderCommand::kWait},
    {"stop", LeaderCommand::kStop},
    {"follow", LeaderCommand::kFollow},
    {"follow me", LeaderCommand::kFollow},
    {"come", LeaderCommand::kFollow},
    {"come here", LeaderCommand::kFollow},
    {"come on", LeaderCommand::kFollow},
    {"on me", LeaderCommand::kFollow},
    {"lets go", LeaderCommand::kFollow},
    {"attack", LeaderCommand::kAttackMyTarget},
    {"attack my target", LeaderCommand::kAttackMyTarget},
    {"kill my target", LeaderCommand::kAttackMyTarget},
    {"assist", LeaderCommand::kAttackMyTarget},
    {"assist me", LeaderCommand::kAttackMyTarget},
    {"dps my target", LeaderCommand::kAttackMyTarget},
    {"drink", LeaderCommand::kDrink},
    {"drink up", LeaderCommand::kDrink},
    {"oom", LeaderCommand::kDrink},
    {"rest", LeaderCommand::kDrink},
    {"mount", LeaderCommand::kMount},
    {"mount up", LeaderCommand::kMount},
    {"heal", LeaderCommand::kHealMe},
    {"heal me", LeaderCommand::kHealMe},
    {"heal pls", LeaderCommand::kHealMe},
    {"need heal", LeaderCommand::kHealMe},
    {"need heals", LeaderCommand::kHealMe},
    {"go there", LeaderCommand::kMoveToPing},
    {"move there", LeaderCommand::kMoveToPing},
    {"flag", LeaderCommand::kMoveToPing},
    {"go to flag", LeaderCommand::kMoveToPing},
    {"go to the flag", LeaderCommand::kMoveToPing},
    {"go to ping", LeaderCommand::kMoveToPing},
    {"go to the ping", LeaderCommand::kMoveToPing},
    {"los", LeaderCommand::kPullBack},
    {"pull back", LeaderCommand::kPullBack},
    {"fall back", LeaderCommand::kPullBack},
    {"pull", LeaderCommand::kPull},
    {"pull it", LeaderCommand::kPull},
    {"pull my target", LeaderCommand::kPull},
    {"lust", LeaderCommand::kLust},
    {"bloodlust", LeaderCommand::kLust},
    {"bl", LeaderCommand::kLust},
    {"hero", LeaderCommand::kLust},
    {"heroism", LeaderCommand::kLust},
    {"spread", LeaderCommand::kSpread},
    {"spread out", LeaderCommand::kSpread},
    {"stack", LeaderCommand::kStack},
    {"stack up", LeaderCommand::kStack},
    {"stack on me", LeaderCommand::kStack},
    {"stack here", LeaderCommand::kStack},
    {"group up", LeaderCommand::kStack},
    {"clump", LeaderCommand::kStack},
    {"clump up", LeaderCommand::kStack},
    {"stop dps", LeaderCommand::kStopDps},
    {"dps stop", LeaderCommand::kStopDps},
    {"hold dps", LeaderCommand::kStopDps},
    {"cease fire", LeaderCommand::kStopDps},
    {"stop attacking", LeaderCommand::kStopDps},
    {"stop attack", LeaderCommand::kStopDps},
    {"no dps", LeaderCommand::kStopDps},
    {"hands off", LeaderCommand::kStopDps},
    {"stop damage", LeaderCommand::kStopDps},
    {"hold damage", LeaderCommand::kStopDps},
    {"resume", LeaderCommand::kResumeDps},
    {"resume dps", LeaderCommand::kResumeDps},
    {"dps", LeaderCommand::kResumeDps},
    {"go dps", LeaderCommand::kResumeDps},
    {"burn", LeaderCommand::kResumeDps},
    {"nuke", LeaderCommand::kResumeDps},
    {"wipe", LeaderCommand::kWipe},
    {"wipe it", LeaderCommand::kWipe},
    {"call wipe", LeaderCommand::kWipe},
    {"release", LeaderCommand::kWipe},
    {"release spirit", LeaderCommand::kWipe},
    {"go repair", LeaderCommand::kGoRepair},
    {"repair", LeaderCommand::kGoRepair},
    {"repair gear", LeaderCommand::kGoRepair},
    {"go vendor", LeaderCommand::kGoRepair},
    {"vendor", LeaderCommand::kGoRepair},
    {"go sell", LeaderCommand::kGoRepair},
    {"sell repair", LeaderCommand::kGoRepair},
    {"go train", LeaderCommand::kGoTrain},
    {"train", LeaderCommand::kGoTrain},
    {"train spells", LeaderCommand::kGoTrain},
    {"go train spells", LeaderCommand::kGoTrain},
};

// "<verb> <icon>": who to kill (or pull) by its raid mark.
constexpr std::string_view kKillVerbs[] = {"kill", "attack", "rti", "focus",
                                           "dps"};

// A bare icon word is a kill call only for the usual kill marks; "moon" or
// "square" alone more often names a crowd-control target.
bool IsBareKillMark(size_t icon) {
  return kRaidIconNames[icon] == "skull" || kRaidIconNames[icon] == "cross";
}

constexpr std::string_view kFiller[] = {
    "pls", "plz", "please",    "guys", "all", "everyone",
    "now", "ok",  "everybody", "team", "and"};

std::vector<std::string> Words(std::string_view text) {
  std::vector<std::string> words;
  std::string cur;
  for (const char c : text) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '@') {
      cur.push_back(static_cast<char>(std::tolower(uc)));
    } else if (c == '\'') {
      continue;  // "let's" -> "lets".
    } else if (!cur.empty()) {
      words.push_back(std::move(cur));
      cur.clear();
    }
  }
  if (!cur.empty()) {
    words.push_back(std::move(cur));
  }
  return words;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace

std::optional<ParsedLeaderCommand> ParseLeaderCommand(
    std::string_view text,
    const std::vector<std::string>& names) {
  std::vector<std::string> words = Words(text);
  ParsedLeaderCommand parsed{LeaderCommand::kWait, {}, std::nullopt};
  std::vector<std::string> rest;
  for (std::string& w : words) {
    if (w == "@heal") {
      rest.push_back("heal");
      continue;
    }
    const auto named =
        std::find_if(names.begin(), names.end(),
                     [&](const std::string& n) { return Lower(n) == w; });
    if (named != names.end()) {
      parsed.addressed.push_back(*named);
      continue;
    }
    if (std::find(std::begin(kFiller), std::end(kFiller), w) !=
        std::end(kFiller)) {
      continue;
    }
    rest.push_back(std::move(w));
  }
  if (rest.empty() || rest.size() > 4) {
    return std::nullopt;
  }
  std::string joined;
  for (const std::string& w : rest) {
    if (!joined.empty()) {
      joined.push_back(' ');
    }
    joined += w;
  }
  for (const Phrase& p : kPhrases) {
    if (p.words == joined) {
      parsed.command = p.command;
      return parsed;
    }
  }
  const std::optional<size_t> icon = RaidIconIndex(rest.back());
  if (!icon || rest.back() == "x") {
    return std::nullopt;  // "kill x" is a sentence more often than a mark.
  }
  parsed.mark = icon;
  if (rest.size() == 1 && IsBareKillMark(*icon)) {
    parsed.command = LeaderCommand::kAttackMark;
    return parsed;
  }
  if (rest.size() != 2) {
    return std::nullopt;
  }
  if (rest[0] == "pull") {
    parsed.command = LeaderCommand::kPull;
    return parsed;
  }
  if (std::find(std::begin(kKillVerbs), std::end(kKillVerbs), rest[0]) !=
      std::end(kKillVerbs)) {
    parsed.command = LeaderCommand::kAttackMark;
    return parsed;
  }
  return std::nullopt;
}

const char* LeaderCommandName(LeaderCommand command) {
  switch (command) {
    case LeaderCommand::kWait:
      return "wait";
    case LeaderCommand::kStop:
      return "stop";
    case LeaderCommand::kFollow:
      return "follow";
    case LeaderCommand::kAttackMyTarget:
      return "attack my target";
    case LeaderCommand::kAttackMark:
      return "kill";
    case LeaderCommand::kDrink:
      return "drink";
    case LeaderCommand::kMount:
      return "mount up";
    case LeaderCommand::kHealMe:
      return "heal me";
    case LeaderCommand::kMoveToPing:
      return "go there";
    case LeaderCommand::kPullBack:
      return "pull back";
    case LeaderCommand::kPull:
      return "pull";
    case LeaderCommand::kLust:
      return "lust";
    case LeaderCommand::kSpread:
      return "spread";
    case LeaderCommand::kStack:
      return "stack";
    case LeaderCommand::kStopDps:
      return "stop dps";
    case LeaderCommand::kResumeDps:
      return "resume dps";
    case LeaderCommand::kWipe:
      return "wipe";
    case LeaderCommand::kGoRepair:
      return "go repair";
    case LeaderCommand::kGoTrain:
      return "go train";
  }
  return "?";
}

std::string DescribeLeaderCommand(const ParsedLeaderCommand& parsed) {
  std::string out = LeaderCommandName(parsed.command);
  if ((parsed.command == LeaderCommand::kAttackMark ||
       parsed.command == LeaderCommand::kPull) &&
      parsed.mark && *parsed.mark < kRaidIconNames.size()) {
    out.push_back(' ');
    out += kRaidIconNames[*parsed.mark];
  }
  return out;
}

bool IsLeaderCommandForBot(bool sender_is_human,
                           bool sender_is_leader,
                           bool leader_is_human,
                           bool is_whisper,
                           const ParsedLeaderCommand& parsed,
                           std::string_view bot_name) {
  if (!sender_is_human) {
    return false;
  }
  if (!parsed.addressed.empty()) {
    return std::find(parsed.addressed.begin(), parsed.addressed.end(),
                     bot_name) != parsed.addressed.end();
  }
  if (is_whisper) {
    return true;
  }
  return sender_is_leader || !leader_is_human;
}

bool ShouldApplySpreadOrStack(bool is_tank, bool explicitly_addressed) {
  return !is_tank || explicitly_addressed;
}

}  // namespace llm_brain
