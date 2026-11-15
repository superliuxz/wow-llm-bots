// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "net/line_framer.h"
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace net {
namespace {

using Lines = std::vector<std::string>;

TEST(LineFramerTest, SplitsJoinedLines) {
  LineFramer framer;
  EXPECT_EQ(framer.Append("a\nb\nc\n"), (Lines{"a", "b", "c"}));
}

TEST(LineFramerTest, JoinsPartialLines) {
  LineFramer framer;
  EXPECT_EQ(framer.Append("{\"ty"), Lines{});
  EXPECT_EQ(framer.Append("pe\":1}\n{\"x"), (Lines{"{\"type\":1}"}));
  EXPECT_EQ(framer.buffered_bytes(), 3u);
  EXPECT_EQ(framer.Append("\"}\n"), (Lines{"{\"x\"}"}));
}

TEST(LineFramerTest, StripsCarriageReturnAndSkipsEmptyLines) {
  LineFramer framer;
  EXPECT_EQ(framer.Append("a\r\n\n\r\nb\n"), (Lines{"a", "b"}));
}

TEST(LineFramerTest, DropsOversizedLineAndRecovers) {
  LineFramer framer(4);
  EXPECT_EQ(framer.Append("toolong"), Lines{});
  EXPECT_EQ(framer.Append("stillgoing\nok\n"), (Lines{"ok"}));
  EXPECT_EQ(framer.dropped_lines(), 1u);
}

}  // namespace
}  // namespace net
