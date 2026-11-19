// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/strings/string_util.h"

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace base {
namespace {

template <typename Range>
std::string JoinImpl(const Range& parts, std::string_view separator) {
  auto it = parts.begin();
  if (it == parts.end()) {
    return std::string();
  }
  size_t total = 0;
  size_t count = 0;
  for (const auto& part : parts) {
    total += part.size();
    ++count;
  }
  total += (count - 1) * separator.size();
  std::string out;
  out.reserve(total);
  out.append(it->data(), it->size());
  for (++it; it != parts.end(); ++it) {
    out.append(separator.data(), separator.size());
    out.append(it->data(), it->size());
  }
  return out;
}

}  // namespace

std::string ToLowerASCII(std::string_view str) {
  std::string out;
  out.reserve(str.size());
  for (char c : str) {
    out.push_back(ToLowerASCII(c));
  }
  return out;
}

std::string ToUpperASCII(std::string_view str) {
  std::string out;
  out.reserve(str.size());
  for (char c : str) {
    out.push_back(ToUpperASCII(c));
  }
  return out;
}

bool EqualsCaseInsensitiveASCII(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (ToLowerASCII(a[i]) != ToLowerASCII(b[i])) {
      return false;
    }
  }
  return true;
}

bool StartsWith(std::string_view str,
                std::string_view search_for,
                CompareCase case_sensitivity) {
  if (search_for.size() > str.size()) {
    return false;
  }
  std::string_view head = str.substr(0, search_for.size());
  return case_sensitivity == CompareCase::SENSITIVE
             ? head == search_for
             : EqualsCaseInsensitiveASCII(head, search_for);
}

bool EndsWith(std::string_view str,
              std::string_view search_for,
              CompareCase case_sensitivity) {
  if (search_for.size() > str.size()) {
    return false;
  }
  std::string_view tail = str.substr(str.size() - search_for.size());
  return case_sensitivity == CompareCase::SENSITIVE
             ? tail == search_for
             : EqualsCaseInsensitiveASCII(tail, search_for);
}

std::optional<std::string_view> RemovePrefix(std::string_view string,
                                             std::string_view prefix,
                                             CompareCase case_sensitivity) {
  if (!StartsWith(string, prefix, case_sensitivity)) {
    return std::nullopt;
  }
  string.remove_prefix(prefix.size());
  return string;
}

std::optional<std::string_view> RemoveSuffix(std::string_view string,
                                             std::string_view suffix,
                                             CompareCase case_sensitivity) {
  if (!EndsWith(string, suffix, case_sensitivity)) {
    return std::nullopt;
  }
  string.remove_suffix(suffix.size());
  return string;
}

std::string_view TrimString(std::string_view input,
                            std::string_view trim_chars,
                            TrimPositions positions) {
  if (positions & TRIM_LEADING) {
    while (!input.empty() &&
           trim_chars.find(input.front()) != std::string_view::npos) {
      input.remove_prefix(1);
    }
  }
  if (positions & TRIM_TRAILING) {
    while (!input.empty() &&
           trim_chars.find(input.back()) != std::string_view::npos) {
      input.remove_suffix(1);
    }
  }
  return input;
}

std::string_view TrimWhitespaceASCII(std::string_view input,
                                     TrimPositions positions) {
  if (positions & TRIM_LEADING) {
    while (!input.empty() && IsAsciiWhitespace(input.front())) {
      input.remove_prefix(1);
    }
  }
  if (positions & TRIM_TRAILING) {
    while (!input.empty() && IsAsciiWhitespace(input.back())) {
      input.remove_suffix(1);
    }
  }
  return input;
}

std::string JoinString(const std::vector<std::string>& parts,
                       std::string_view separator) {
  return JoinImpl(parts, separator);
}

std::string JoinString(const std::vector<std::string_view>& parts,
                       std::string_view separator) {
  return JoinImpl(parts, separator);
}

std::string JoinString(std::initializer_list<std::string_view> parts,
                       std::string_view separator) {
  return JoinImpl(parts, separator);
}

}  // namespace base
