// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/popup_watch.h"

#include <cstdint>
#include <optional>
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
  s.shared_quests.push_back(
      {.quest_id = 42, .title = "Vile Familiars", .from = "Hele"});
  s.resurrect_offer = true;
  s.summon = true;
  s.summon_from = "Zenji";
  s.boot_victim = "Loadc";
  s.boot_reason = "afk";
  std::vector<pb::Event> evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 7u);
  EXPECT_EQ(evs[0].prompt->kind, pb::PromptEvent::READY_CHECK);
  EXPECT_EQ(evs[1].prompt->kind, pb::PromptEvent::ROLE_CHECK);
  EXPECT_EQ(evs[2].prompt->kind, pb::PromptEvent::LFG_PROPOSAL);
  EXPECT_EQ(evs[3].prompt->kind, pb::PromptEvent::QUEST_SHARE);
  EXPECT_EQ(evs[3].prompt->from, "Hele");
  EXPECT_EQ(evs[3].prompt->text, "Vile Familiars");
  EXPECT_EQ(evs[4].prompt->kind, pb::PromptEvent::RESURRECT_OFFER);
  EXPECT_EQ(evs[5].prompt->kind, pb::PromptEvent::SUMMON);
  EXPECT_EQ(evs[5].prompt->from, "Zenji");
  EXPECT_EQ(evs[6].prompt->kind, pb::PromptEvent::LFG_BOOT_VOTE);
  EXPECT_EQ(evs[6].prompt->text, "Loadc: afk");

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

TEST(PopupWatchTest, TradeWindowOpenAndTheirAcceptEachWakeOnce) {
  PopupWatch watch;
  PopupState s;
  s.trader = "Hele";
  std::vector<pb::Event> evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].prompt->kind, pb::PromptEvent::TRADE);
  EXPECT_EQ(evs[0].prompt->from, "Hele");
  EXPECT_EQ(evs[0].prompt->text, "window open");
  EXPECT_TRUE(watch.Update(s).empty());  // Still open, nothing new.
  s.trader_accepted = true;
  evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].prompt->text, "they accepted");
  EXPECT_TRUE(watch.Update(s).empty());
  // Counter-test: changing an offer releases their Trade button; pressing it
  // again wakes the brain again.
  s.trader_accepted = false;
  EXPECT_TRUE(watch.Update(s).empty());
  s.trader_accepted = true;
  EXPECT_EQ(watch.Update(s).size(), 1u);
  // Closed, then a new trade with someone else: a new window.
  EXPECT_TRUE(watch.Update({}).empty());
  PopupState other;
  other.trader = "Zenji";
  evs = watch.Update(other);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].prompt->from, "Zenji");
  EXPECT_EQ(evs[0].prompt->text, "window open");
  // Reset (logout, disconnect) forgets the open window.
  watch.Reset();
  EXPECT_EQ(watch.Update(other).size(), 1u);
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

TEST(PopupWatchTest, HasTimedPopupDetectsTimedPopupsAndExcludesTrade) {
  EXPECT_FALSE(HasTimedPopup(PopupState{}, /*has_group_invite=*/false));
  EXPECT_TRUE(HasTimedPopup(PopupState{}, /*has_group_invite=*/true));

  PopupState ready;
  ready.ready_check = true;
  EXPECT_TRUE(HasTimedPopup(ready));

  PopupState role;
  role.role_check = true;
  EXPECT_TRUE(HasTimedPopup(role));

  PopupState lfg;
  lfg.lfg_proposal_id = 9;
  EXPECT_TRUE(HasTimedPopup(lfg));

  PopupState quest;
  quest.shared_quests.push_back(
      {.quest_id = 42, .title = "Vile Familiars", .from = "Hele"});
  EXPECT_TRUE(HasTimedPopup(quest));

  PopupState rez;
  rez.resurrect_offer = true;
  EXPECT_TRUE(HasTimedPopup(rez));

  PopupState summon;
  summon.summon = true;
  EXPECT_TRUE(HasTimedPopup(summon));

  PopupState boot;
  boot.boot_victim = "Loadc";
  EXPECT_TRUE(HasTimedPopup(boot));

  PopupState roll;
  roll.rolls.push_back(
      {.item_guid = 1, .item_slot = 0, .item_id = 100, .item_name = "Ring"});
  EXPECT_TRUE(HasTimedPopup(roll));

  // Adversarial counter-test: an open trade window is NOT a timed popup that
  // auto-hands off to playerbots after 15 s (a human may take longer to place
  // items, and playerbots would cancel a non-master's trade).
  PopupState trade;
  trade.trader = "Hele";
  trade.trader_accepted = true;
  EXPECT_FALSE(HasTimedPopup(trade));
}

TEST(PopupWatchTest, UpdatePopupDeadlineHandsOffPastDeadlineAndResetsOnClose) {
  std::optional<uint64_t> open_since;

  // No popup open -> not missed, timer stays empty.
  EXPECT_FALSE(UpdatePopupDeadline(&open_since, /*has_timed_popup=*/false,
                                   /*now_ms=*/1000));
  EXPECT_FALSE(open_since.has_value());

  // Popup opens at t=1000 -> not missed yet.
  EXPECT_FALSE(UpdatePopupDeadline(&open_since, /*has_timed_popup=*/true,
                                   /*now_ms=*/1000));
  ASSERT_TRUE(open_since.has_value());
  EXPECT_EQ(*open_since, 1000u);

  // Just before deadline -> still brain's.
  EXPECT_FALSE(
      UpdatePopupDeadline(&open_since, /*has_timed_popup=*/true,
                          /*now_ms=*/1000 + kPopupFallbackDeadlineMs - 1));

  // At deadline -> missed, hand off to playerbots.
  EXPECT_TRUE(UpdatePopupDeadline(&open_since, /*has_timed_popup=*/true,
                                  /*now_ms=*/1000 + kPopupFallbackDeadlineMs));

  // Popup closes -> timer resets and ownership restores to brain.
  EXPECT_FALSE(
      UpdatePopupDeadline(&open_since, /*has_timed_popup=*/false,
                          /*now_ms=*/1000 + kPopupFallbackDeadlineMs + 500));
  EXPECT_FALSE(open_since.has_value());

  // Subsequent popup starts a fresh deadline from its own open time.
  EXPECT_FALSE(UpdatePopupDeadline(&open_since, /*has_timed_popup=*/true,
                                   /*now_ms=*/50000));
  ASSERT_TRUE(open_since.has_value());
  EXPECT_EQ(*open_since, 50000u);
  EXPECT_FALSE(UpdatePopupDeadline(&open_since, /*has_timed_popup=*/true,
                                   /*now_ms=*/55000));
  EXPECT_TRUE(UpdatePopupDeadline(&open_since, /*has_timed_popup=*/true,
                                  /*now_ms=*/50000 + kPopupFallbackDeadlineMs));
}

TEST(PopupWatchTest, RapidMultiShareEmitsOneEventPerNewQuest) {
  PopupWatch watch;
  PopupState s;
  s.shared_quests.push_back(
      {.quest_id = 101, .title = "Wanted: Hogger", .from = "Aldric"});
  std::vector<pb::Event> evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].prompt->kind, pb::PromptEvent::QUEST_SHARE);
  EXPECT_EQ(evs[0].prompt->text, "Wanted: Hogger");

  // Second and third shares arrive while the first is still open.
  s.shared_quests.push_back(
      {.quest_id = 102, .title = "Kobold Candles", .from = "Aldric"});
  s.shared_quests.push_back(
      {.quest_id = 103, .title = "Gold Dust Exchange", .from = "Aldric"});
  evs = watch.Update(s);
  ASSERT_EQ(evs.size(), 2u);
  EXPECT_EQ(evs[0].prompt->text, "Kobold Candles");
  EXPECT_EQ(evs[1].prompt->text, "Gold Dust Exchange");

  // Still open -> no duplicate wakes.
  EXPECT_TRUE(watch.Update(s).empty());
}

}  // namespace
}  // namespace llm_brain
