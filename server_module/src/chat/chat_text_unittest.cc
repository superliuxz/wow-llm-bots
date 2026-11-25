// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_text.h"

#include <string>
#include <string_view>

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

TEST(ChatTextTest, MentionsNameMatchesWholeWordsOnly) {
  EXPECT_FALSE(MentionsName("need heals please", "Ed"));
  EXPECT_FALSE(MentionsName("freddy come here", "Ed"));
  EXPECT_FALSE(MentionsName("edited the pull", "Ed"));
  EXPECT_TRUE(MentionsName("Ed can you heal?", "Ed"));
  EXPECT_TRUE(MentionsName("hey ed!", "Ed"));
  EXPECT_TRUE(MentionsName("wait, ED.", "Ed"));
  EXPECT_FALSE(MentionsName("hello", ""));
}

TEST(ChatTextTest, RaidIconTagsBecomeTheirNames) {
  EXPECT_EQ(NormalizeRaidIcons("kill {skull} then {cross}"),
            "kill skull then cross");
  EXPECT_EQ(NormalizeRaidIcons("{rt1}{rt2}{rt3}{rt4}{rt5}{rt6}{rt7}{rt8}"),
            "star circle diamond triangle moon square cross skull");
  EXPECT_EQ(NormalizeRaidIcons("sheep {MOON}, zap {x} and {coin}"),
            "sheep moon, zap cross and circle");
  // A tag glued to a word stays its own word.
  EXPECT_EQ(NormalizeRaidIcons("kill{skull}now"), "kill skull now");
  EXPECT_EQ(NormalizeRaidIcons("{skull}"), "skull");
}

TEST(ChatTextTest, OtherBracesAreLeftAsTyped) {
  for (const char* t : {"{rt9}", "{rt0}", "{}", "{skull", "skull}", "{ skull }",
                        "{heart}", "plain text", "", "{{rt8}}"}) {
    const std::string out = NormalizeRaidIcons(t);
    if (std::string_view(t) == "{{rt8}}") {
      EXPECT_EQ(out, "{skull}");
    } else {
      EXPECT_EQ(out, t) << t;
    }
  }
}

TEST(ChatTextTest, RaidIconIndexMatchesGroupSlots) {
  EXPECT_EQ(RaidIconIndex("skull"), 7u);
  EXPECT_EQ(RaidIconIndex("Cross"), 6u);
  EXPECT_EQ(RaidIconIndex("rt1"), 0u);
  EXPECT_EQ(RaidIconIndex("x"), 6u);
  EXPECT_FALSE(RaidIconIndex("skulls"));
  EXPECT_FALSE(RaidIconIndex("rt"));
  EXPECT_FALSE(RaidIconIndex(""));
}

}  // namespace
}  // namespace llm_brain
