// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/spell_text.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace llm_brain {
namespace {

bool IsValueCode(char c) {
  switch (c) {
    case 's':
    case 'o':
    case 't':
    case 'm':
    case 'a':
    case 'd':
    case 'h':
    case 'n':
    case 'u':
    case 'x':
    case 'e':
    case 'q':
    case 'z':
      return true;
    default:
      return false;
  }
}

// The number at the end of `text` ("Restores 61 health" -> 61), or nullopt.
std::optional<double> LastNumber(std::string_view text) {
  size_t end = text.size();
  while (end > 0 && !base::IsAsciiDigit(text[end - 1])) {
    --end;
  }
  size_t start = end;
  while (start > 0 &&
         (base::IsAsciiDigit(text[start - 1]) || text[start - 1] == '.')) {
    --start;
  }
  double value = 0;
  if (start == end ||
      !base::StringToDouble(text.substr(start, end - start), &value)) {
    return std::nullopt;
  }
  return value;
}

uint32_t ReadU32(std::string_view bytes, size_t at) {
  return static_cast<uint32_t>(static_cast<unsigned char>(bytes[at])) |
         static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8 |
         static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 2]))
             << 16 |
         static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24;
}

}  // namespace

std::unordered_map<uint32_t, std::string> ParseSpellDescriptions(
    std::string_view dbc,
    const std::vector<uint32_t>& wanted,
    uint32_t locale) {
  std::unordered_map<uint32_t, std::string> out;
  constexpr size_t kHeader = 20;
  if (dbc.size() < kHeader || dbc.substr(0, 4) != "WDBC") {
    return out;
  }
  const uint32_t records = ReadU32(dbc, 4);
  const uint32_t fields = ReadU32(dbc, 8);
  const uint32_t record_size = ReadU32(dbc, 12);
  const uint32_t strings_size = ReadU32(dbc, 16);
  const uint32_t field = kSpellDbcDescriptionField + locale;
  const size_t strings_at =
      kHeader + static_cast<size_t>(records) * record_size;
  if (field >= fields || record_size < fields * 4u ||
      strings_at + strings_size > dbc.size()) {
    return out;
  }
  const std::string_view strings = dbc.substr(strings_at, strings_size);
  std::vector<uint32_t> sorted = wanted;
  std::sort(sorted.begin(), sorted.end());
  for (uint32_t i = 0; i < records; ++i) {
    const size_t at = kHeader + static_cast<size_t>(i) * record_size;
    const uint32_t id = ReadU32(dbc, at);
    if (!std::binary_search(sorted.begin(), sorted.end(), id)) {
      continue;
    }
    const uint32_t offset = ReadU32(dbc, at + field * 4u);
    if (offset == 0 || offset >= strings.size()) {
      continue;
    }
    const size_t end = strings.find('\0', offset);
    std::string text(strings.substr(
        offset, end == strings.npos ? strings.npos : end - offset));
    if (!text.empty()) {
      out.emplace(id, std::move(text));
    }
  }
  return out;
}

std::string FormatSpellText(std::string_view text,
                            const SpellValueSource& values) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '$' || i + 1 >= text.size()) {
      out.push_back(text[i++]);
      continue;
    }
    size_t j = i + 1;
    // "$l" / "$g": a choice between forms, "one:many;".
    const char choice = base::ToLowerASCII(text[j]);
    if (choice == 'l' || choice == 'g') {
      const size_t colon = text.find(':', j + 1);
      const size_t semi = text.find(';', j + 1);
      if (colon != text.npos && semi != text.npos && colon < semi) {
        const std::string_view first = text.substr(j + 1, colon - j - 1);
        const std::string_view second =
            text.substr(colon + 1, semi - colon - 1);
        if (choice == 'g') {
          out.append(first);
        } else {
          const std::optional<double> n = LastNumber(out);
          out.append(n && *n == 1 ? first : second);
        }
        i = semi + 1;
        continue;
      }
    }
    // "$12345s1": another spell's value.
    uint32_t spell_id = 0;
    while (j < text.size() && base::IsAsciiDigit(text[j])) {
      spell_id = spell_id * 10 + static_cast<uint32_t>(text[j] - '0');
      ++j;
    }
    if (j >= text.size() || !IsValueCode(base::ToLowerASCII(text[j]))) {
      out.push_back(text[i++]);  // Not a token we know: keep the '$'.
      continue;
    }
    const char code = base::ToLowerASCII(text[j++]);
    int index = 0;
    if (j < text.size() && text[j] >= '1' && text[j] <= '3') {
      index = text[j++] - '0';
    }
    if (const std::optional<std::string> value =
            values.Value(spell_id, code, index)) {
      out.append(*value);
    } else {
      out.append(text.substr(i, j - i));
    }
    i = j;
  }
  return out;
}

}  // namespace llm_brain
