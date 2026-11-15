// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_NET_FRAME_CODEC_H_
#define COMMON_NET_FRAME_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace net {

// Length-prefixed framing for a byte stream: each frame is a 4-byte big-endian
// payload length followed by the payload (any bytes, e.g. a serialized proto).

inline constexpr size_t kFrameHeaderBytes = 4;
// Largest payload either side accepts (snapshots are capped far below this).
inline constexpr size_t kMaxFramePayloadBytes = 1 << 20;

// Appends `payload` as one frame to `out`. Returns false (nothing appended) if
// the payload is larger than kMaxFramePayloadBytes.
bool AppendFrame(std::string_view payload, std::string* out);

// Reassembles frames from stream reads. A length over `max_payload_bytes` means
// the stream is corrupt or out of sync and cannot be resynchronized: the
// decoder fails and returns nothing more (callers close the connection).
class FrameDecoder {
 public:
  explicit FrameDecoder(size_t max_payload_bytes = kMaxFramePayloadBytes);

  // Appends bytes; returns the payloads of the frames they complete (empty
  // payloads included).
  std::vector<std::string> Append(std::string_view data);

  bool failed() const { return failed_; }
  size_t buffered_bytes() const { return buffer_.size(); }

 private:
  const size_t max_payload_bytes_;
  std::string buffer_;
  bool failed_ = false;
};

// Exposed for tests.
uint32_t ReadFrameLength(std::string_view header);

}  // namespace net

#endif  // COMMON_NET_FRAME_CODEC_H_
