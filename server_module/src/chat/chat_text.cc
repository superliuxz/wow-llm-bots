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

}  // namespace llm_brain
