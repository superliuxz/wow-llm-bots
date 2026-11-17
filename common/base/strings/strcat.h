// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_STRINGS_STRCAT_H_
#define COMMON_BASE_STRINGS_STRCAT_H_

#include <initializer_list>
#include <string>
#include <string_view>

namespace base {

[[nodiscard]] std::string StrCat(
    std::initializer_list<std::string_view> pieces);

void StrAppend(std::string* dest,
               std::initializer_list<std::string_view> pieces);

}  // namespace base

#endif  // COMMON_BASE_STRINGS_STRCAT_H_
