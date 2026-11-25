// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CHAT_CHAT_TEXT_H_
#define MOD_LLM_BRAIN_CHAT_CHAT_TEXT_H_

#include <array>
#include <cstddef>
#include <optional>
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

// A line that asks nothing of anyone ("ok", "lol", "ty", "kk", ":)"): heard,
// but nobody needs to wake for it (RH-H4).
bool IsTrivialChat(std::string_view text);

// True when `name` appears in `text` as a whole ASCII word (case-insensitive),
// so a bot named "Ed" does not wake on "need" (A-L4).
bool MentionsName(std::string_view text, std::string_view name);

// The raid target icons by index (Group::GetTargetIcon slot; `{rt<i+1>}`).
inline constexpr std::array<std::string_view, 8> kRaidIconNames = {
    "star", "circle", "diamond", "triangle",
    "moon", "square", "cross",   "skull"};

// The icon index of a word ("skull", "Cross", "rt8", "x", "coin": the
// client's chat icon tags, case-insensitive), or nullopt.
std::optional<size_t> RaidIconIndex(std::string_view word);

// Incoming chat with the client's raid target icon tags written as their
// names: "kill {skull} then {rt7}" -> "kill skull then cross" (the client
// draws them as icons; the brain and the leader commands read words). Spaces
// keep a tag from gluing onto a neighbouring word; any other `{...}` is left
// as typed.
std::string NormalizeRaidIcons(std::string_view text);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CHAT_CHAT_TEXT_H_
