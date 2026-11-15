// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/line_framer.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace net {

LineFramer::LineFramer(size_t max_line_bytes)
    : max_line_bytes_(max_line_bytes) {}

std::vector<std::string> LineFramer::Append(std::string_view data) {
  std::vector<std::string> lines;
  while (!data.empty()) {
    const size_t newline = data.find('\n');
    const std::string_view chunk =
        newline == std::string_view::npos ? data : data.substr(0, newline);
    if (!discarding_) {
      if (partial_.size() + chunk.size() > max_line_bytes_) {
        partial_.clear();
        discarding_ = true;
        ++dropped_lines_;
      } else {
        partial_.append(chunk);
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    if (!discarding_) {
      if (!partial_.empty() && partial_.back() == '\r') {
        partial_.pop_back();
      }
      if (!partial_.empty()) {
        lines.push_back(std::move(partial_));
      }
    }
    partial_.clear();
    discarding_ = false;
    data.remove_prefix(newline + 1);
  }
  return lines;
}

void LineFramer::Reset() {
  partial_.clear();
  discarding_ = false;
}

}  // namespace net
