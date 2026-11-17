// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/strings/string_number_conversions.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "base/strings/string_util.h"

namespace base {
namespace {

template <typename Int>
std::string IntToString(Int value) {
  char buf[32];
  const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  return ec == std::errc() ? std::string(buf, end) : std::string();
}

template <typename Int>
bool ParseInteger(std::string_view input, Int* output) {
  input = TrimWhitespaceASCII(input);
  if (input.empty()) {
    return false;
  }
  if (input.front() == '+') {
    input.remove_prefix(1);
    if (input.empty() || input.front() == '-') {
      return false;
    }
  }
  if constexpr (std::is_unsigned_v<Int>) {
    if (input.front() == '-') {
      return false;
    }
  }
  Int parsed = 0;
  const auto [end, ec] =
      std::from_chars(input.data(), input.data() + input.size(), parsed);
  if (ec != std::errc() || end != input.data() + input.size()) {
    return false;
  }
  *output = parsed;
  return true;
}

}  // namespace

std::string NumberToString(int value) {
  return IntToString(value);
}

std::string NumberToString(unsigned int value) {
  return IntToString(value);
}

std::string NumberToString(long value) {
  return IntToString(value);
}

std::string NumberToString(unsigned long value) {
  return IntToString(value);
}

std::string NumberToString(long long value) {
  return IntToString(value);
}

std::string NumberToString(unsigned long long value) {
  return IntToString(value);
}

std::string NumberToString(double value) {
  char buf[64];
  const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  return ec == std::errc() ? std::string(buf, end) : std::string();
}

bool StringToInt(std::string_view input, int* output) {
  return ParseInteger(input, output);
}

bool StringToUint(std::string_view input, unsigned int* output) {
  return ParseInteger(input, output);
}

bool StringToInt64(std::string_view input, int64_t* output) {
  return ParseInteger(input, output);
}

bool StringToUint64(std::string_view input, uint64_t* output) {
  return ParseInteger(input, output);
}

bool StringToSizeT(std::string_view input, size_t* output) {
  return ParseInteger(input, output);
}

bool StringToDouble(std::string_view input, double* output) {
  input = TrimWhitespaceASCII(input);
  if (input.empty()) {
    return false;
  }
  if (input.front() == '+') {
    input.remove_prefix(1);
    if (input.empty() || input.front() == '-') {
      return false;
    }
  }
  double parsed = 0;
  const auto [end, ec] =
      std::from_chars(input.data(), input.data() + input.size(), parsed);
  if (ec != std::errc() || end != input.data() + input.size()) {
    return false;
  }
  *output = parsed;
  return true;
}

}  // namespace base
