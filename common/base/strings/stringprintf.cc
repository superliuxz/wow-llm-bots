// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/strings/stringprintf.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "base/compiler_specific.h"

namespace base {

void StringAppendV(std::string* dst, const char* format, va_list ap) {
  char stack_buf[1024]{};
  va_list ap_copy;
  va_copy(ap_copy, ap);
  const int result =
      std::vsnprintf(stack_buf, sizeof(stack_buf), format, ap_copy);
  va_end(ap_copy);

  if (result < 0) {
    return;
  }
  if (static_cast<size_t>(result) < sizeof(stack_buf)) {
    MSAN_UNPOISON(stack_buf, static_cast<size_t>(result) + 1);
    dst->append(stack_buf, static_cast<size_t>(result));
    return;
  }

  const size_t length = static_cast<size_t>(result);
  std::vector<char> heap_buf(length + 1, '\0');
  va_copy(ap_copy, ap);
  const int second =
      std::vsnprintf(heap_buf.data(), heap_buf.size(), format, ap_copy);
  va_end(ap_copy);
  if (second >= 0 && static_cast<size_t>(second) <= length) {
    MSAN_UNPOISON(heap_buf.data(), static_cast<size_t>(second) + 1);
    dst->append(heap_buf.data(), static_cast<size_t>(second));
  }
}

std::string StringPrintf(const char* format, ...) {
  std::string out;
  va_list ap;
  va_start(ap, format);
  StringAppendV(&out, format, ap);
  va_end(ap);
  return out;
}

void StringAppendF(std::string* dst, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  StringAppendV(dst, format, ap);
  va_end(ap);
}

}  // namespace base
