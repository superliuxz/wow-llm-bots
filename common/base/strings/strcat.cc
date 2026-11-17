// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/strings/strcat.h"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

namespace base {

void StrAppend(std::string* dest,
               std::initializer_list<std::string_view> pieces) {
  size_t additional = 0;
  for (std::string_view piece : pieces) {
    additional += piece.size();
  }
  dest->reserve(dest->size() + additional);
  for (std::string_view piece : pieces) {
    dest->append(piece.data(), piece.size());
  }
}

std::string StrCat(std::initializer_list<std::string_view> pieces) {
  std::string out;
  StrAppend(&out, pieces);
  return out;
}

}  // namespace base
