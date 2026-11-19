// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_STRINGS_STRING_UTIL_H_
#define COMMON_BASE_STRINGS_STRING_UTIL_H_

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace base {

enum class CompareCase {
  SENSITIVE,
  INSENSITIVE_ASCII,
};

enum TrimPositions {
  TRIM_NONE = 0,
  TRIM_LEADING = 1 << 0,
  TRIM_TRAILING = 1 << 1,
  TRIM_ALL = TRIM_LEADING | TRIM_TRAILING,
};

constexpr bool IsAsciiWhitespace(char c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' ||
         c == '\v';
}

constexpr bool IsAsciiLower(char c) {
  return c >= 'a' && c <= 'z';
}

constexpr bool IsAsciiUpper(char c) {
  return c >= 'A' && c <= 'Z';
}

constexpr bool IsAsciiAlpha(char c) {
  return IsAsciiLower(c) || IsAsciiUpper(c);
}

constexpr bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

constexpr bool IsAsciiAlphaNumeric(char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c);
}

constexpr char ToLowerASCII(char c) {
  return IsAsciiUpper(c) ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr char ToUpperASCII(char c) {
  return IsAsciiLower(c) ? static_cast<char>(c - ('a' - 'A')) : c;
}

[[nodiscard]] std::string ToLowerASCII(std::string_view str);
[[nodiscard]] std::string ToUpperASCII(std::string_view str);

[[nodiscard]] bool EqualsCaseInsensitiveASCII(std::string_view a,
                                              std::string_view b);

[[nodiscard]] bool StartsWith(
    std::string_view str,
    std::string_view search_for,
    CompareCase case_sensitivity = CompareCase::SENSITIVE);

[[nodiscard]] bool EndsWith(
    std::string_view str,
    std::string_view search_for,
    CompareCase case_sensitivity = CompareCase::SENSITIVE);

[[nodiscard]] std::optional<std::string_view> RemovePrefix(
    std::string_view string,
    std::string_view prefix,
    CompareCase case_sensitivity = CompareCase::SENSITIVE);

[[nodiscard]] std::optional<std::string_view> RemoveSuffix(
    std::string_view string,
    std::string_view suffix,
    CompareCase case_sensitivity = CompareCase::SENSITIVE);

[[nodiscard]] std::string_view TrimString(std::string_view input,
                                          std::string_view trim_chars,
                                          TrimPositions positions);

[[nodiscard]] std::string_view TrimWhitespaceASCII(
    std::string_view input,
    TrimPositions positions = TRIM_ALL);

[[nodiscard]] std::string JoinString(const std::vector<std::string>& parts,
                                     std::string_view separator);
[[nodiscard]] std::string JoinString(const std::vector<std::string_view>& parts,
                                     std::string_view separator);
[[nodiscard]] std::string JoinString(
    std::initializer_list<std::string_view> parts,
    std::string_view separator);

}  // namespace base

#endif  // COMMON_BASE_STRINGS_STRING_UTIL_H_
