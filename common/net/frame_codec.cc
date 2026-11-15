// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/frame_codec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace net {

bool AppendFrame(std::string_view payload, std::string* out) {
  if (payload.size() > kMaxFramePayloadBytes) {
    return false;
  }
  const auto size = static_cast<uint32_t>(payload.size());
  const char header[kFrameHeaderBytes] = {
      static_cast<char>((size >> 24) & 0xff),
      static_cast<char>((size >> 16) & 0xff),
      static_cast<char>((size >> 8) & 0xff),
      static_cast<char>(size & 0xff),
  };
  out->append(header, kFrameHeaderBytes);
  out->append(payload);
  return true;
}

uint32_t ReadFrameLength(std::string_view header) {
  uint32_t size = 0;
  for (size_t i = 0; i < kFrameHeaderBytes; ++i) {
    size = (size << 8) | static_cast<uint8_t>(header[i]);
  }
  return size;
}

FrameDecoder::FrameDecoder(size_t max_payload_bytes)
    : max_payload_bytes_(max_payload_bytes) {}

std::vector<std::string> FrameDecoder::Append(std::string_view data) {
  std::vector<std::string> frames;
  if (failed_) {
    return frames;
  }
  buffer_.append(data);
  size_t offset = 0;
  while (buffer_.size() - offset >= kFrameHeaderBytes) {
    const std::string_view rest = std::string_view(buffer_).substr(offset);
    const size_t size = ReadFrameLength(rest);
    if (size > max_payload_bytes_) {
      failed_ = true;
      buffer_.clear();
      return frames;
    }
    if (rest.size() < kFrameHeaderBytes + size) {
      break;  // Wait for the rest of this frame.
    }
    frames.emplace_back(rest.substr(kFrameHeaderBytes, size));
    offset += kFrameHeaderBytes + size;
  }
  buffer_.erase(0, offset);
  return frames;
}

}  // namespace net
