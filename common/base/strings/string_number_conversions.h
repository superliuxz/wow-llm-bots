// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
#define COMMON_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace base {

[[nodiscard]] std::string NumberToString(int value);
[[nodiscard]] std::string NumberToString(unsigned int value);
[[nodiscard]] std::string NumberToString(long value);
[[nodiscard]] std::string NumberToString(unsigned long value);
[[nodiscard]] std::string NumberToString(long long value);
[[nodiscard]] std::string NumberToString(unsigned long long value);
[[nodiscard]] std::string NumberToString(double value);

bool StringToInt(std::string_view input, int* output);
bool StringToUint(std::string_view input, unsigned int* output);
bool StringToInt64(std::string_view input, int64_t* output);
bool StringToUint64(std::string_view input, uint64_t* output);
bool StringToSizeT(std::string_view input, size_t* output);
bool StringToDouble(std::string_view input, double* output);

}  // namespace base

#endif  // COMMON_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
