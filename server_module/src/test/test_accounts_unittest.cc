// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/test_accounts.h"

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(TestAccountsTest, PoolNames) {
  EXPECT_EQ(TestAccountName("WLBTEST", 0), "WLBTEST");
  EXPECT_EQ(TestAccountName("WLBTEST", 1), "WLBTEST2");
  EXPECT_EQ(TestAccountName("WLBTEST", 9), "WLBTEST10");
}

TEST(TestAccountsTest, RecognizesOnlyThePool) {
  EXPECT_TRUE(IsTestAccountName("WLBTEST", "WLBTEST"));
  EXPECT_TRUE(IsTestAccountName("WLBTEST2", "WLBTEST"));
  EXPECT_TRUE(IsTestAccountName("wlbtest12", "WLBTEST"));
  EXPECT_FALSE(IsTestAccountName("WLBTEST1", "WLBTEST"));
  EXPECT_FALSE(IsTestAccountName("WLBTEST02", "WLBTEST"));
  EXPECT_FALSE(IsTestAccountName("WLBTESTX", "WLBTEST"));
  EXPECT_FALSE(IsTestAccountName("RNDBOT12", "WLBTEST"));
  EXPECT_FALSE(IsTestAccountName("PLAYER", "WLBTEST"));
  EXPECT_FALSE(IsTestAccountName("WLBTEST", ""));
}

}  // namespace
}  // namespace llm_brain
