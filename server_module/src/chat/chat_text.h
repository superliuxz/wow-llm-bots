// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CHAT_CHAT_TEXT_H_
#define MOD_LLM_BRAIN_CHAT_CHAT_TEXT_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace llm_brain {

// The client's own limit for one chat line, in bytes.
inline constexpr size_t kMaxChatBytes = 255;

// What the server accepts as a typed chat line (WorldSession's chat handler):
// cut at the first line break; no control bytes (the server drops a line
// containing one) and no '|' (the client's escape for item links and colors,
// which the server validates and a typed line never has); runs of spaces
// collapsed; trimmed; at most `max_bytes` bytes, never cutting a UTF-8
// character in half. Empty when nothing is left.
std::string CleanChatText(std::string_view text,
                          size_t max_bytes = kMaxChatBytes);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CHAT_CHAT_TEXT_H_
