// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_STRINGS_STRINGPRINTF_H_
#define COMMON_BASE_STRINGS_STRINGPRINTF_H_

#include <cstdarg>
#include <string>

#include "base/compiler_specific.h"

namespace base {

[[nodiscard]] std::string StringPrintf(const char* format, ...)
    PRINTF_FORMAT(1, 2);

void StringAppendF(std::string* dst, const char* format, ...)
    PRINTF_FORMAT(2, 3);

void StringAppendV(std::string* dst, const char* format, va_list ap);

}  // namespace base

#endif  // COMMON_BASE_STRINGS_STRINGPRINTF_H_
