// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/bot_accounts.h"

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(TestAccountsTest, PoolNames) {
  EXPECT_EQ(BotAccountName("WLBTEST", 0), "WLBTEST");
  EXPECT_EQ(BotAccountName("WLBTEST", 1), "WLBTEST2");
  EXPECT_EQ(BotAccountName("WLBTEST", 9), "WLBTEST10");
}

TEST(TestAccountsTest, RecognizesOnlyThePool) {
  EXPECT_TRUE(IsBotAccountName("WLBTEST", "WLBTEST"));
  EXPECT_TRUE(IsBotAccountName("WLBTEST2", "WLBTEST"));
  EXPECT_TRUE(IsBotAccountName("wlbtest12", "WLBTEST"));
  EXPECT_FALSE(IsBotAccountName("WLBTEST1", "WLBTEST"));
  EXPECT_FALSE(IsBotAccountName("WLBTEST02", "WLBTEST"));
  EXPECT_FALSE(IsBotAccountName("WLBTESTX", "WLBTEST"));
  EXPECT_FALSE(IsBotAccountName("RNDBOT12", "WLBTEST"));
  EXPECT_FALSE(IsBotAccountName("PLAYER", "WLBTEST"));
  EXPECT_FALSE(IsBotAccountName("WLBTEST", ""));
}

}  // namespace
}  // namespace llm_brain
