// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "chat/chat_inbox.h"

#include <cstddef>
#include <string>
#include <vector>

#include "base/threading/thread.h"
#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

pb::ChatEvent Whisper(const std::string& from, const std::string& text) {
  pb::ChatEvent chat;
  chat.channel = pb::WHISPER;
  chat.from = from;
  chat.text = text;
  chat.at_me = true;
  return chat;
}

TEST(ChatInboxTest, KeepsOnlyLinesToTheBotsInOrder) {
  ChatInbox inbox;
  inbox.SetBots({"Testpriest", "Loada"});
  EXPECT_TRUE(inbox.Add("Testpriest", Whisper("Gatda", "hi")));
  EXPECT_FALSE(inbox.Add("Gatda", Whisper("Testpriest", "not a bot of ours")));
  EXPECT_TRUE(inbox.Add("Loada", Whisper("Gatda", "and you")));
  const std::vector<ChatInbox::Line> lines = inbox.TakeAll();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].to, "Testpriest");
  EXPECT_EQ(lines[0].chat.text, "hi");
  EXPECT_EQ(lines[1].to, "Loada");
  EXPECT_TRUE(inbox.TakeAll().empty());
}

TEST(ChatInboxTest, WritesRaidIconTagsAsWords) {
  ChatInbox inbox;
  inbox.SetBots({"Loada"});
  EXPECT_TRUE(inbox.Add("Loada", Whisper("Gatda", "sheep {rt5} kill {Skull}")));
  const std::vector<ChatInbox::Line> lines = inbox.TakeAll();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].chat.text, "sheep moon kill skull");
}

TEST(ChatInboxTest, NothingIsKeptBeforeTheBotsAreKnown) {
  ChatInbox inbox;
  EXPECT_FALSE(inbox.Add("Testpriest", Whisper("Gatda", "hi")));
  EXPECT_TRUE(inbox.TakeAll().empty());
}

TEST(ChatInboxTest, AFullInboxDropsAndCounts) {
  ChatInbox inbox;
  inbox.SetBots({"Testpriest"});
  for (size_t i = 0; i < ChatInbox::kMaxWaiting; ++i) {
    EXPECT_TRUE(inbox.Add("Testpriest", Whisper("Gatda", "spam")));
  }
  EXPECT_FALSE(inbox.Add("Testpriest", Whisper("Gatda", "one too many")));
  EXPECT_EQ(inbox.TakeDropped(), 1u);
  EXPECT_EQ(inbox.TakeDropped(), 0u);
  EXPECT_EQ(inbox.TakeAll().size(), ChatInbox::kMaxWaiting);
}

TEST(ChatInboxTest, LinesAddedOnAnotherThreadArrive) {
  ChatInbox inbox;
  inbox.SetBots({"Testpriest"});
  {
    base::Thread map_thread("map-update");
    ASSERT_TRUE(map_thread.Start());
    for (int i = 0; i < 100; ++i) {
      map_thread.task_runner()->PostTask(
          [&inbox] { inbox.Add("Testpriest", Whisper("Gatda", "hi")); });
    }
    map_thread.Stop();  // Runs every posted task first.
  }
  EXPECT_EQ(inbox.TakeAll().size(), 100u);
}

}  // namespace
}  // namespace llm_brain
