// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/ui_state.h"

#include <vector>

#include "gtest/gtest.h"
#include "proto/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

TEST(UiStateTest, OpensOnlySupportedPanelsOnce) {
  UiState ui;
  EXPECT_EQ(ui.Open(pb::BAGS), "");
  EXPECT_EQ(ui.Open(pb::BAGS), "");
  EXPECT_EQ(ui.Open(pb::QUEST_LOG), "");
  EXPECT_NE(ui.Open(pb::TALENTS), "");
  pb::Ui out;
  ui.FillAndAge(&out);
  EXPECT_EQ(out.open_panels, (std::vector<pb::Panel>{pb::BAGS, pb::QUEST_LOG}));

  ui.Close(pb::BAGS);
  ui.FillAndAge(&out);
  EXPECT_EQ(out.open_panels, (std::vector<pb::Panel>{pb::QUEST_LOG}));
  ui.CloseAll();
  ui.FillAndAge(&out);
  EXPECT_TRUE(out.open_panels.empty());
}

TEST(UiStateTest, HoverLastsAFewSnapshots) {
  UiState ui;
  pb::HoverRef ref;
  ref.kind = pb::HoverRef::ITEM;
  ref.id = "4540";
  ref.name = "Tough Hunk of Bread";
  ASSERT_EQ(ui.Hover(ref), "");
  pb::Ui out;
  for (int i = 0; i < UiState::kHoverSnapshots; ++i) {
    ui.FillAndAge(&out);
    ASSERT_EQ(out.hovering.size(), 1u) << "snapshot " << i;
    EXPECT_EQ(out.hovering[0].name, "Tough Hunk of Bread");
  }
  ui.FillAndAge(&out);
  EXPECT_TRUE(out.hovering.empty());
}

TEST(UiStateTest, RefusesWhatItCantPointAt) {
  UiState ui;
  pb::HoverRef spell;
  spell.kind = pb::HoverRef::SPELL;
  spell.id = "585";
  EXPECT_NE(ui.Hover(spell), "");
  pb::HoverRef empty;
  empty.kind = pb::HoverRef::QUEST;
  EXPECT_NE(ui.Hover(empty), "");
  EXPECT_FALSE(ui.hover().has_value());
}

}  // namespace
}  // namespace llm_brain
