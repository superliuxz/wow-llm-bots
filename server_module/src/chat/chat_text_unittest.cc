// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_text.h"

#include <string>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(ChatTextTest, KeepsAnOrdinaryLine) {
  EXPECT_EQ(CleanChatText("sure, send me an invite"),
            "sure, send me an invite");
}

TEST(ChatTextTest, CutsAtTheFirstLineBreak) {
  EXPECT_EQ(CleanChatText("first line\nsecond"), "first line");
  EXPECT_EQ(CleanChatText("first\r\nsecond"), "first");
  EXPECT_EQ(CleanChatText("\nonly after"), "");
}

TEST(ChatTextTest, DropsControlBytesAndLinkEscapes) {
  EXPECT_EQ(CleanChatText("a\tb\x01"
                          "c\x7f"),
            "abc");
  EXPECT_EQ(CleanChatText("|cffff0000red|r |Hitem:1|h[x]|h"),
            "cffff0000redr Hitem:1h[x]h");
}

TEST(ChatTextTest, CollapsesAndTrimsSpaces) {
  EXPECT_EQ(CleanChatText("   hi    there   "), "hi there");
  EXPECT_EQ(CleanChatText("   "), "");
}

TEST(ChatTextTest, NeverCutsAUtf8CharacterInHalf) {
  // "é" is two bytes: a limit landing between them drops the whole character.
  EXPECT_EQ(CleanChatText("caf\xC3\xA9", 4), "caf");
  EXPECT_EQ(CleanChatText("caf\xC3\xA9", 5), "caf\xC3\xA9");
  // A three-byte character ("€").
  EXPECT_EQ(CleanChatText("ab\xE2\x82\xAC", 4), "ab");
  EXPECT_EQ(CleanChatText(std::string(300, 'x')).size(), kMaxChatBytes);
}

TEST(ChatTextTest, TrivialLinesAskNothing) {
  for (const char* t :
       {"ok", "OK!", "kk", "lol", "ty", "thx!", ":)", "...", "gg", "Nice."}) {
    EXPECT_TRUE(IsTrivialChat(t)) << t;
  }
  for (const char* t : {"ok?", "pull", "wait here", "telda heal me", "lol wait",
                        "need mana?", "inv pls"}) {
    EXPECT_FALSE(IsTrivialChat(t)) << t;
  }
}

}  // namespace
}  // namespace llm_brain
