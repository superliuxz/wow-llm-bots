// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_NET_LINE_FRAMER_H_
#define COMMON_NET_LINE_FRAMER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace net {

// Splits a byte stream into '\n'-terminated lines ("\r\n" accepted; empty lines
// skipped). A line longer than `max_line_bytes` is discarded up to its newline.
class LineFramer {
 public:
  explicit LineFramer(size_t max_line_bytes = 1 << 20);

  // Appends bytes; returns the complete lines they finish.
  std::vector<std::string> Append(std::string_view data);
  void Reset();

  size_t buffered_bytes() const { return partial_.size(); }
  size_t dropped_lines() const { return dropped_lines_; }

 private:
  const size_t max_line_bytes_;
  std::string partial_;
  bool discarding_ = false;
  size_t dropped_lines_ = 0;
};

}  // namespace net

#endif  // COMMON_NET_LINE_FRAMER_H_
