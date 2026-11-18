// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "grammar/plan_hooks.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace harness::grammar::hooks {

std::string NormalizeSteps(std::string_view value) {
  std::string out;
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '[' || c == ']' || c == '(' || c == ')') {
      continue;
    }
    // The multiplication sign is two bytes in UTF-8.
    if (value.substr(i).starts_with("\xC3\x97")) {
      out.push_back('x');
      ++i;
      continue;
    }
    out.push_back(c);
  }
  // No spaces around an `x`: "3 x 4" -> "3x4".
  std::string tight;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == ' ') {
      const size_t next = out.find_first_not_of(' ', i);
      const bool before_x =
          next != out.npos && (out[next] == 'x' || out[next] == 'X');
      const bool after_x =
          !tight.empty() && (tight.back() == 'x' || tight.back() == 'X');
      if (before_x || after_x) {
        continue;
      }
    }
    tight.push_back(out[i] == 'X' ? 'x' : out[i]);
  }
  return tight;
}

std::string NormalizeWatches(std::string_view value) {
  const std::string lower = base::ToLowerASCII(value);
  std::string out;
  size_t i = 0;
  while (i < lower.size()) {
    bool done = false;
    for (const std::string_view word : {"enemy_player", "mana", "hp"}) {
      if (!std::string_view(lower).substr(i).starts_with(word)) {
        continue;
      }
      // The word, then "<" however it was written, then the number; a
      // trailing "%" or "yd" is dropped.
      out.append(word);
      out.push_back('<');
      size_t j = i + word.size();
      while (j < lower.size() && (lower[j] == ' ' || lower[j] == '<')) {
        ++j;
      }
      while (j < lower.size() && base::IsAsciiDigit(lower[j])) {
        out.push_back(lower[j++]);
      }
      while (j < lower.size() && lower[j] == ' ') {
        ++j;
      }
      if (std::string_view(lower).substr(j).starts_with("%")) {
        ++j;
      } else if (std::string_view(lower).substr(j).starts_with("yd")) {
        j += 2;
      }
      i = j;
      done = true;
      break;
    }
    if (!done) {
      out.push_back(lower[i++]);
    }
  }
  return out;
}

bool LookSeconds(std::string_view value, int* seconds) {
  size_t digits = 0;
  while (digits < value.size() && base::IsAsciiDigit(value[digits])) {
    ++digits;
  }
  const std::string unit = base::ToLowerASCII(
      base::TrimWhitespaceASCII(value.substr(digits), base::TRIM_ALL));
  int scale = 0;
  if (unit.empty() || unit == "s" || unit == "sec" || unit == "secs" ||
      unit == "second" || unit == "seconds") {
    scale = 1;
  } else if (unit == "m" || unit == "min" || unit == "mins" ||
             unit == "minute" || unit == "minutes") {
    scale = 60;
  } else {
    return false;  // Something else: not a time we understand.
  }
  int count = 0;
  if (digits == 0 || digits > 4 ||
      !base::StringToInt(value.substr(0, digits), &count)) {
    return false;
  }
  *seconds = count * scale;
  return true;
}

bool CapitalizedName(std::string_view value, std::string* name) {
  value = base::TrimWhitespaceASCII(value, base::TRIM_ALL);
  if (value.size() < 2 || value.size() > 12) {
    return false;
  }
  std::string out;
  for (char c : value) {
    if (!base::IsAsciiAlpha(c)) {
      return false;
    }
    out.push_back(out.empty() ? base::ToUpperASCII(c) : base::ToLowerASCII(c));
  }
  *name = std::move(out);
  return true;
}

}  // namespace harness::grammar::hooks
