// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_text.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace llm_brain {

std::string CleanChatText(std::string_view text, size_t max_bytes) {
  text = text.substr(0, text.find_first_of("\r\n"));
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f || c == '|') {
      continue;
    }
    if (c == ' ' && (out.empty() || out.back() == ' ')) {
      continue;
    }
    out.push_back(c);
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  if (out.size() > max_bytes) {
    size_t cut = max_bytes;
    // Back up over UTF-8 continuation bytes (10xxxxxx) to a character start.
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') {
      out.pop_back();
    }
  }
  return out;
}

bool IsTrivialChat(std::string_view text) {
  // Lowercase letters only; punctuation, spaces and smileys are dropped.
  std::string word;
  bool other = false;
  for (const char c : text) {
    if (c >= 'A' && c <= 'Z') {
      word.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c >= 'a' && c <= 'z') {
      word.push_back(c);
    } else if (c == '?') {
      other = true;  // A question always asks something.
    }
  }
  if (other) {
    return false;
  }
  if (word.empty()) {
    return true;  // ":)", "!!!", "..."
  }
  // Acknowledgements only: "yes" / "sure" may answer a bot's question, so
  // they still wake it.
  static constexpr std::string_view kTrivial[] = {
      "ok",   "okay", "k",    "kk",  "kkk",    "lol", "lmao",
      "haha", "hah",  "ty",   "thx", "thanks", "np",  "yw",
      "gg",   "nice", "cool", "xd",  "hehe",   "rofl"};
  for (std::string_view t : kTrivial) {
    if (word == t) {
      return true;
    }
  }
  return false;
}

bool MentionsName(std::string_view text, std::string_view name) {
  if (name.empty() || text.size() < name.size()) {
    return false;
  }
  const auto is_word = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  for (size_t i = 0; i + name.size() <= text.size(); ++i) {
    const bool left_ok = (i == 0) || !is_word(text[i - 1]);
    const bool right_ok =
        (i + name.size() == text.size()) || !is_word(text[i + name.size()]);
    if (!left_ok || !right_ok) {
      continue;
    }
    bool match = true;
    for (size_t j = 0; j < name.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(text[i + j])) !=
          std::tolower(static_cast<unsigned char>(name[j]))) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

std::optional<size_t> RaidIconIndex(std::string_view word) {
  std::string w(word);
  for (char& c : w) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (size_t i = 0; i < kRaidIconNames.size(); ++i) {
    if (w == kRaidIconNames[i]) {
      return i;
    }
  }
  // The client's other tags (ICON_TAG_RAID_TARGET_*2 and `{rt1}`..`{rt8}`).
  if (w == "coin") {
    return 1;
  }
  if (w == "x") {
    return 6;
  }
  if (w.size() == 3 && w.starts_with("rt") && w[2] >= '1' && w[2] <= '8') {
    return static_cast<size_t>(w[2] - '1');
  }
  return std::nullopt;
}

std::string NormalizeRaidIcons(std::string_view text) {
  const auto is_word = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
  };
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const size_t close =
        text[i] == '{' ? text.find('}', i + 1) : std::string_view::npos;
    const std::optional<size_t> icon =
        close == std::string_view::npos
            ? std::nullopt
            : RaidIconIndex(text.substr(i + 1, close - i - 1));
    if (!icon) {
      out.push_back(text[i]);
      ++i;
      continue;
    }
    if (!out.empty() && is_word(out.back())) {
      out.push_back(' ');
    }
    out += kRaidIconNames[*icon];
    i = close + 1;
    if (i < text.size() && is_word(text[i])) {
      out.push_back(' ');
    }
  }
  return out;
}

}  // namespace llm_brain
