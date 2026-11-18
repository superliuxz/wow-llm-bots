// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/strings/string_split.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/string_util.h"

namespace base {

std::vector<std::string_view> SplitStringPiece(std::string_view input,
                                               std::string_view separators,
                                               WhitespaceHandling whitespace,
                                               SplitResult result_type) {
  std::vector<std::string_view> result;
  if (input.empty()) {
    return result;
  }
  size_t start = 0;
  while (start <= input.size()) {
    size_t end = separators.size() == 1
                     ? input.find(separators[0], start)
                     : input.find_first_of(separators, start);
    std::string_view piece = end == std::string_view::npos
                                 ? input.substr(start)
                                 : input.substr(start, end - start);
    if (whitespace == TRIM_WHITESPACE) {
      piece = TrimWhitespaceASCII(piece, TRIM_ALL);
    }
    if (result_type == SPLIT_WANT_ALL || !piece.empty()) {
      result.push_back(piece);
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return result;
}

std::vector<std::string_view> SplitStringPieceUsingSubstr(
    std::string_view input,
    std::string_view delimiter,
    WhitespaceHandling whitespace,
    SplitResult result_type) {
  std::vector<std::string_view> result;
  if (delimiter.empty()) {
    result.push_back(input);
    return result;
  }
  size_t start = 0;
  while (true) {
    const size_t end = input.find(delimiter, start);
    std::string_view piece = end == std::string_view::npos
                                 ? input.substr(start)
                                 : input.substr(start, end - start);
    if (whitespace == TRIM_WHITESPACE) {
      piece = TrimWhitespaceASCII(piece, TRIM_ALL);
    }
    if (result_type == SPLIT_WANT_ALL || !piece.empty()) {
      result.push_back(piece);
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + delimiter.size();
  }
  return result;
}

std::vector<std::string> SplitString(std::string_view input,
                                     std::string_view separators,
                                     WhitespaceHandling whitespace,
                                     SplitResult result_type) {
  const std::vector<std::string_view> pieces =
      SplitStringPiece(input, separators, whitespace, result_type);
  std::vector<std::string> out;
  out.reserve(pieces.size());
  for (std::string_view piece : pieces) {
    out.emplace_back(piece);
  }
  return out;
}

}  // namespace base
