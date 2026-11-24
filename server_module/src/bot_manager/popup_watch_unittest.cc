// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/popup_watch.h"

#include <vector>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

TEST(PopupWatchTest, ReportsEachNewPopupOnce) {
  PopupWatch watch;
  EXPECT_TRUE(watch.Update({}).empty());

  PopupState s;
  s.ready_check = true;
  s.role_check = true;
  s.lfg_proposal_id = 7;
  s.shared_quest_id = 42;
  s.shared_quest_title = "Vile Familiars";
  s.shared_quest_from = "Hele";
  s.resurrect_offer = true;
  std::vector<pb::Event> evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 5u);
  EXPECT_EQ(evs[0].prompt->kind, pb::PromptEvent::READY_CHECK);
  EXPECT_EQ(evs[1].prompt->kind, pb::PromptEvent::ROLE_CHECK);
  EXPECT_EQ(evs[2].prompt->kind, pb::PromptEvent::LFG_PROPOSAL);
  EXPECT_EQ(evs[3].prompt->kind, pb::PromptEvent::QUEST_SHARE);
  EXPECT_EQ(evs[3].prompt->from, "Hele");
  EXPECT_EQ(evs[3].prompt->text, "Vile Familiars");
  EXPECT_EQ(evs[4].prompt->kind, pb::PromptEvent::RESURRECT_OFFER);

  // Still open: nothing new.
  EXPECT_TRUE(watch.Update(s).empty());
}

TEST(PopupWatchTest, ReopenedOrChangedPopupIsReportedAgain) {
  PopupWatch watch;
  PopupState s;
  s.ready_check = true;
  s.lfg_proposal_id = 1;
  EXPECT_EQ(watch.Update(s).size(), 2u);
  EXPECT_TRUE(watch.Update({}).empty());  // Closed.
  s.lfg_proposal_id = 2;
  std::vector<pb::Event> evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 2u);
  EXPECT_EQ(evs[0].prompt->kind, pb::PromptEvent::READY_CHECK);
  EXPECT_EQ(evs[1].prompt->kind, pb::PromptEvent::LFG_PROPOSAL);
  // A different proposal id while one is open counts as new.
  s.lfg_proposal_id = 3;
  evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].prompt->kind, pb::PromptEvent::LFG_PROPOSAL);
}

TEST(PopupWatchTest, EachNewLootRollIsOneEvent) {
  PopupWatch watch;
  PopupState s;
  s.rolls.push_back({.item_guid = 10,
                     .item_slot = 0,
                     .item_id = 100,
                     .item_name = "Cruel Barb"});
  std::vector<pb::Event> evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 1u);
  ASSERT_TRUE(evs[0].loot_roll && evs[0].loot_roll->item);
  EXPECT_EQ(evs[0].loot_roll->item->name, "Cruel Barb");
  EXPECT_EQ(evs[0].loot_roll->item->id, 100u);

  s.rolls.push_back({.item_guid = 11,
                     .item_slot = 1,
                     .item_id = 101,
                     .item_name = "Smelting Pants"});
  evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].loot_roll->item->name, "Smelting Pants");
  EXPECT_TRUE(watch.Update(s).empty());
}

TEST(PopupWatchTest, ResetForgetsWhatWasSeen) {
  PopupWatch watch;
  PopupState s;
  s.ready_check = true;
  EXPECT_EQ(watch.Update(s).size(), 1u);
  watch.Reset();
  EXPECT_EQ(watch.Update(s).size(), 1u);
}

}  // namespace
}  // namespace llm_brain
