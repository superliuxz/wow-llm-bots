// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/leader_command.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    {"stop", LeaderCommand::kWait},
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
    {"skull", LeaderCommand::kAttackSkull},
    {"kill skull", LeaderCommand::kAttackSkull},
    {"rti skull", LeaderCommand::kAttackSkull},
    {"attack skull", LeaderCommand::kAttackSkull},
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
};

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
  ParsedLeaderCommand parsed{LeaderCommand::kWait, {}};
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
  if (rest.empty() || rest.size() > 3) {
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
  return std::nullopt;
}

const char* LeaderCommandName(LeaderCommand command) {
  switch (command) {
    case LeaderCommand::kWait:
      return "wait";
    case LeaderCommand::kFollow:
      return "follow";
    case LeaderCommand::kAttackMyTarget:
      return "attack my target";
    case LeaderCommand::kAttackSkull:
      return "kill skull";
    case LeaderCommand::kDrink:
      return "drink";
    case LeaderCommand::kMount:
      return "mount up";
    case LeaderCommand::kHealMe:
      return "heal me";
  }
  return "?";
}

}  // namespace llm_brain
