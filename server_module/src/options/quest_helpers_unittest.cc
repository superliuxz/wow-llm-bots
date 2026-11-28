// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/quest_helpers.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(QuestHelpersTest, FailedEscortQuestOfferedEvenWhenLogNotFull) {
  const std::vector<AbandonQuestCandidate> log = {
      {.quest_id = 10, .title = "Kobold Camp Cleanup", .level = 3},
      {.quest_id = 20,
       .title = "The Defias Traitor",
       .level = 14,
       .failed = true},
      {.quest_id = 30,
       .title = "A Threat Within",
       .level = 1,
       .complete = true},
  };
  const std::vector<AbandonQuestOption> opts = SelectAbandonQuestOptions(
      /*alive=*/true, /*in_combat=*/false, /*total_log_quests=*/3,
      /*quest_log_open=*/false, log);
  ASSERT_EQ(opts.size(), 1u);
  EXPECT_EQ(opts[0].quest_id, 20u);
  EXPECT_EQ(opts[0].guid, "qabandon:20");
  EXPECT_EQ(opts[0].label, "Abandon failed quest \"The Defias Traitor\"");
}

TEST(QuestHelpersTest, RoutineActiveQuestsNotClutteredWhenLogNotFullAndClosed) {
  const std::vector<AbandonQuestCandidate> log = {
      {.quest_id = 10, .title = "Kobold Camp Cleanup", .level = 3},
      {.quest_id = 11, .title = "Wolves Across the Border", .level = 2},
  };
  EXPECT_TRUE(SelectAbandonQuestOptions(
                  /*alive=*/true, /*in_combat=*/false, /*total_log_quests=*/2,
                  /*quest_log_open=*/false, log)
                  .empty());
}

TEST(QuestHelpersTest, FullTwentyFiveQuestLogExposesPrioritizedAbandonOptions) {
  std::vector<AbandonQuestCandidate> log;
  for (uint32_t i = 1; i <= 25; ++i) {
    AbandonQuestCandidate c;
    c.quest_id = 100 + i;
    c.title = "Quest " + std::to_string(i);
    c.level = 10 + (i % 15);
    c.complete = (i <= 10);
    log.push_back(c);
  }
  // Mark one high-level quest failed and one timed incomplete.
  log[24].failed = true;
  log[24].title = "Failed Escort";
  log[23].timed = true;
  log[23].title = "Timed Delivery";

  const std::vector<AbandonQuestOption> opts = SelectAbandonQuestOptions(
      /*alive=*/true, /*in_combat=*/false, /*total_log_quests=*/25,
      /*quest_log_open=*/false, log);
  ASSERT_EQ(opts.size(), kMaxAbandonOptions);
  EXPECT_EQ(opts[0].quest_id, 125u);
  EXPECT_EQ(opts[0].label, "Abandon failed quest \"Failed Escort\"");
  EXPECT_EQ(opts[1].quest_id, 124u);
  EXPECT_EQ(opts[1].label, "Abandon timed quest \"Timed Delivery\"");
  // Remaining options are incomplete quests before completed ones.
  for (size_t i = 2; i < opts.size(); ++i) {
    EXPECT_GT(opts[i].quest_id, 110u);
  }
}

TEST(QuestHelpersTest, OpenQuestLogPanelAllowsAbandoningActiveQuests) {
  const std::vector<AbandonQuestCandidate> log = {
      {.quest_id = 50, .title = "Old Green Quest", .level = 5},
      {.quest_id = 60, .title = "Current Quest", .level = 20},
  };
  const std::vector<AbandonQuestOption> opts = SelectAbandonQuestOptions(
      /*alive=*/true, /*in_combat=*/false, /*total_log_quests=*/2,
      /*quest_log_open=*/true, log);
  ASSERT_EQ(opts.size(), 2u);
  EXPECT_EQ(opts[0].quest_id, 50u);
  EXPECT_EQ(opts[0].label, "Abandon quest \"Old Green Quest\"");
  EXPECT_EQ(opts[1].quest_id, 60u);
}

// Adversarial counter-test: P0 combat priority & dead state never offer abandon
// options, even for failed quests or a full 25-quest log.
TEST(QuestHelpersTest, CombatAndDeathSuppressAbandonOptions) {
  const std::vector<AbandonQuestCandidate> log = {
      {.quest_id = 20, .title = "Failed Escort", .level = 14, .failed = true},
  };
  EXPECT_TRUE(SelectAbandonQuestOptions(
                  /*alive=*/true, /*in_combat=*/true, /*total_log_quests=*/25,
                  /*quest_log_open=*/true, log)
                  .empty());
  EXPECT_TRUE(SelectAbandonQuestOptions(
                  /*alive=*/false, /*in_combat=*/false, /*total_log_quests=*/25,
                  /*quest_log_open=*/true, log)
                  .empty());
}

TEST(QuestHelpersTest, RapidMultiShareQueuesWithoutBlockingAndPopsById) {
  std::vector<PendingSharedQuest> queue;
  EnqueueSharedQuest(
      &queue, {.quest_id = 101, .sharer_guid = 0xAA, .received_ms = 1000},
      1000);
  EnqueueSharedQuest(
      &queue, {.quest_id = 102, .sharer_guid = 0xAA, .received_ms = 1010},
      1010);
  EnqueueSharedQuest(&queue,
                     {.quest_id = 103,
                      .sharer_guid = 0xBB,
                      .received_ms = 1020,
                      .is_confirm = true},
                     1020);
  ASSERT_EQ(queue.size(), 3u);

  // Re-sharing quest 102 refreshes in place rather than duplicating.
  EnqueueSharedQuest(
      &queue, {.quest_id = 102, .sharer_guid = 0xCC, .received_ms = 1500},
      1500);
  ASSERT_EQ(queue.size(), 3u);
  EXPECT_EQ(queue[1].sharer_guid, 0xCCu);
  EXPECT_EQ(queue[1].received_ms, 1500u);

  // Pop middle quest out of order; others remain queued.
  const std::optional<PendingSharedQuest> taken = TakeSharedQuest(&queue, 102);
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->sharer_guid, 0xCCu);
  ASSERT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue[0].quest_id, 101u);
  EXPECT_EQ(queue[1].quest_id, 103u);
  EXPECT_TRUE(queue[1].is_confirm);

  // Taking an absent quest returns nullopt.
  EXPECT_FALSE(TakeSharedQuest(&queue, 999).has_value());
}

TEST(QuestHelpersTest, ShareQueuePrunesExpiredWithMsWraparoundAndCapsSize) {
  std::vector<PendingSharedQuest> queue;
  const uint32_t near_max = std::numeric_limits<uint32_t>::max() - 1000u;
  EnqueueSharedQuest(
      &queue, {.quest_id = 1, .sharer_guid = 0x10, .received_ms = near_max},
      near_max);
  // 20 s later (wrapped past 0): still valid.
  PruneExpiredSharedQuests(&queue, near_max + 20000u);
  ASSERT_EQ(queue.size(), 1u);
  // At 60 s: pruned.
  PruneExpiredSharedQuests(&queue, near_max + kPendingSharedQuestTimeoutMs);
  EXPECT_TRUE(queue.empty());

  // Cap at kMaxPendingSharedQuests, dropping oldest.
  for (uint32_t i = 1; i <= kMaxPendingSharedQuests + 3; ++i) {
    EnqueueSharedQuest(
        &queue, {.quest_id = i, .sharer_guid = 0x10, .received_ms = 5000 + i},
        5000 + i);
  }
  ASSERT_EQ(queue.size(), kMaxPendingSharedQuests);
  EXPECT_EQ(queue.front().quest_id, 4u);
  EXPECT_EQ(queue.back().quest_id,
            static_cast<uint32_t>(kMaxPendingSharedQuests + 3));
}

TEST(QuestHelpersTest, OfflineHumanPauseAndOnlineResume) {
  // Grouped with offline human and no online human -> pause levelling.
  EXPECT_TRUE(ShouldPauseForOfflineHuman({.in_group = true,
                                          .has_online_human = false,
                                          .has_offline_human = true}));
  // Human comes back online -> resume immediately.
  EXPECT_FALSE(ShouldPauseForOfflineHuman({.in_group = true,
                                           .has_online_human = true,
                                           .has_offline_human = false}));
  // Adversarial counter-tests:
  // 1. Solo bot never pauses.
  EXPECT_FALSE(ShouldPauseForOfflineHuman({.in_group = false,
                                           .has_online_human = false,
                                           .has_offline_human = true}));
  // 2. Pure bot-only group never pauses.
  EXPECT_FALSE(ShouldPauseForOfflineHuman({.in_group = true,
                                           .has_online_human = false,
                                           .has_offline_human = false}));
  // 3. Another human is still online in the group -> keep playing with them.
  EXPECT_FALSE(ShouldPauseForOfflineHuman(
      {.in_group = true, .has_online_human = true, .has_offline_human = true}));
}

}  // namespace
}  // namespace llm_brain
