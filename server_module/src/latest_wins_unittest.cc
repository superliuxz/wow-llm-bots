// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "latest_wins.h"

#include "base/time/time.h"
#include "gtest/gtest.h"

namespace llm_brain {
namespace {

const base::TimeTicks kT0 = base::TimeTicks() + base::Seconds(10);

TEST(LatestWinsTest, ANewerMessageReplacesTheWaitingOne) {
  LatestWins queue;
  EXPECT_FALSE(queue.Put("tess", "v1", kT0));
  EXPECT_TRUE(queue.Put("tess", "v2", kT0 + base::Milliseconds(5)));
  EXPECT_TRUE(queue.Put("tess", "v3", kT0 + base::Milliseconds(9)));
  ASSERT_EQ(queue.size(), 1u);
  const auto items = queue.TakeAll();
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].message, "v3");
  EXPECT_EQ(items[0].queued, kT0 + base::Milliseconds(9));
}

TEST(LatestWinsTest, KeysAreKeptApartAndInTheOrderTheyFirstArrived) {
  LatestWins queue;
  queue.Put("b", "b1", kT0);
  queue.Put("a", "a1", kT0);
  queue.Put("b", "b2", kT0);  // Replaces, keeps b's place.
  queue.Put("c", "c1", kT0);
  const auto items = queue.TakeAll();
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0].key, "b");
  EXPECT_EQ(items[0].message, "b2");
  EXPECT_EQ(items[1].key, "a");
  EXPECT_EQ(items[2].key, "c");
}

TEST(LatestWinsTest, TakingEmptiesIt) {
  LatestWins queue;
  EXPECT_TRUE(queue.empty());
  queue.Put("a", "1", kT0);
  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.TakeAll().size(), 1u);
  EXPECT_TRUE(queue.empty());
  EXPECT_TRUE(queue.TakeAll().empty());
  // A key can be put again afterwards, as new.
  EXPECT_FALSE(queue.Put("a", "2", kT0));
}

TEST(LatestWinsTest, TakeOldestTakesOneAndLeavesTheRestReplaceable) {
  LatestWins queue;
  queue.Put("a", "a1", kT0);
  queue.Put("b", "b1", kT0);
  EXPECT_EQ(queue.TakeOldest().message, "a1");
  EXPECT_EQ(queue.size(), 1u);
  // "b" is still waiting, so a newer one still replaces it.
  EXPECT_TRUE(queue.Put("b", "b2", kT0));
  EXPECT_EQ(queue.TakeOldest().message, "b2");
  EXPECT_TRUE(queue.empty());
}

}  // namespace
}  // namespace llm_brain
