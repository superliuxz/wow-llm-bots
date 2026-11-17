// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_STRINGS_STRING_SPLIT_H_
#define COMMON_BASE_STRINGS_STRING_SPLIT_H_

#include <string>
#include <string_view>
#include <vector>

namespace base {

enum WhitespaceHandling {
  KEEP_WHITESPACE,
  TRIM_WHITESPACE,
};

enum SplitResult {
  SPLIT_WANT_ALL,
  SPLIT_WANT_NONEMPTY,
};

[[nodiscard]] std::vector<std::string> SplitString(
    std::string_view input,
    std::string_view separators,
    WhitespaceHandling whitespace,
    SplitResult result_type);

[[nodiscard]] std::vector<std::string_view> SplitStringPiece(
    std::string_view input,
    std::string_view separators,
    WhitespaceHandling whitespace,
    SplitResult result_type);

}  // namespace base

#endif  // COMMON_BASE_STRINGS_STRING_SPLIT_H_
