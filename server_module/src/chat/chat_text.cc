// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_text.h"

#include <cstddef>
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

}  // namespace llm_brain
