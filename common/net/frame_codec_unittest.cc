// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/frame_codec.h"

#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace net {
namespace {

using Frames = std::vector<std::string>;

std::string Encode(std::string_view payload) {
  std::string out;
  EXPECT_TRUE(AppendFrame(payload, &out));
  return out;
}

TEST(FrameCodecTest, HeaderIsBigEndianLength) {
  const std::string frame = Encode(std::string(0x0102, 'x'));
  ASSERT_EQ(frame.size(), kFrameHeaderBytes + 0x0102);
  EXPECT_EQ(frame.substr(0, 4), std::string("\x00\x00\x01\x02", 4));
  EXPECT_EQ(ReadFrameLength(frame), 0x0102u);
}

TEST(FrameCodecTest, PayloadMayContainAnyBytes) {
  const std::string payload("a\nb\0c\r\n\xff", 8);
  FrameDecoder decoder;
  EXPECT_EQ(decoder.Append(Encode(payload)), Frames{payload});
}

TEST(FrameCodecTest, SplitsJoinedFrames) {
  FrameDecoder decoder;
  EXPECT_EQ(decoder.Append(Encode("a") + Encode("bb") + Encode("ccc")),
            (Frames{"a", "bb", "ccc"}));
  EXPECT_EQ(decoder.buffered_bytes(), 0u);
}

TEST(FrameCodecTest, JoinsFramesSplitAcrossReads) {
  const std::string stream = Encode("hello") + Encode("world");
  FrameDecoder decoder;
  Frames got;
  for (char c : stream) {  // One byte per read: the worst case.
    for (std::string& frame : decoder.Append(std::string_view(&c, 1))) {
      got.push_back(frame);
    }
  }
  EXPECT_EQ(got, (Frames{"hello", "world"}));
}

TEST(FrameCodecTest, KeepsPartialFrameBuffered) {
  const std::string frame = Encode("snapshot");
  FrameDecoder decoder;
  EXPECT_EQ(decoder.Append(std::string_view(frame).substr(0, 6)), Frames{});
  EXPECT_EQ(decoder.buffered_bytes(), 6u);
  EXPECT_EQ(decoder.Append(std::string_view(frame).substr(6)),
            Frames{"snapshot"});
}

TEST(FrameCodecTest, DeliversEmptyPayloads) {
  // An empty proto message serializes to zero bytes.
  FrameDecoder decoder;
  EXPECT_EQ(decoder.Append(Encode("") + Encode("x")), (Frames{"", "x"}));
}

TEST(FrameCodecTest, OversizedLengthFailsForGood) {
  FrameDecoder decoder(/*max_payload_bytes=*/4);
  EXPECT_EQ(decoder.Append(Encode("ok") + Encode("too long")), Frames{"ok"});
  EXPECT_TRUE(decoder.failed());
  EXPECT_EQ(decoder.buffered_bytes(), 0u);
  EXPECT_EQ(decoder.Append(Encode("ok")), Frames{});
}

TEST(FrameCodecTest, RefusesToEncodeOversizedPayload) {
  std::string out = "keep";
  EXPECT_FALSE(AppendFrame(std::string(kMaxFramePayloadBytes + 1, 'x'), &out));
  EXPECT_EQ(out, "keep");
  EXPECT_TRUE(AppendFrame(std::string(kMaxFramePayloadBytes, 'x'), &out));
}

}  // namespace
}  // namespace net
