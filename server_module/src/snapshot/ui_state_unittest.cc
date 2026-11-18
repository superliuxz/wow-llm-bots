// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/ui_state.h"

#include <optional>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "proto/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

TEST(UiStateTest, OpensOnlySupportedPanelsOnce) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  UiState ui;
  EXPECT_EQ(ui.Open(pb::BAGS), "");
  EXPECT_EQ(ui.Open(pb::BAGS), "");
  EXPECT_EQ(ui.Open(pb::QUEST_LOG), "");
  EXPECT_NE(ui.Open(pb::TALENTS), "");
  pb::Ui out;
  ui.FillAndAge(&out, t0);
  EXPECT_EQ(out.open_panels, (std::vector<pb::Panel>{pb::BAGS, pb::QUEST_LOG}));

  ui.Close(pb::BAGS);
  ui.FillAndAge(&out, t0);
  EXPECT_EQ(out.open_panels, (std::vector<pb::Panel>{pb::QUEST_LOG}));
  ui.CloseAll();
  ui.FillAndAge(&out, t0);
  EXPECT_TRUE(out.open_panels.empty());
}

TEST(UiStateTest, HoverLastsByTimeEvenAcrossFastCombatSnapshots) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  UiState ui;
  pb::HoverRef ref;
  ref.kind = pb::HoverRef::ITEM;
  ref.id = "4540";
  ref.name = "Tough Hunk of Bread";
  ASSERT_EQ(ui.Hover(ref, t0), "");
  pb::Ui out;
  // Combat snapshots every 250 ms: stays for the full 5 s (20 snapshots).
  for (int i = 0; i < 20; ++i) {
    const std::optional<pb::HoverRef> active =
        ui.FillAndAge(&out, t0 + base::Milliseconds(250 * i));
    ASSERT_TRUE(active.has_value()) << "snapshot " << i;
    ASSERT_EQ(out.hovering.size(), 1u) << "snapshot " << i;
    EXPECT_EQ(out.hovering[0].name, "Tough Hunk of Bread");
  }
  EXPECT_FALSE(ui.FillAndAge(&out, t0 + UiState::kHoverDuration).has_value());
  EXPECT_TRUE(out.hovering.empty());
}

TEST(UiStateTest, HoverShowsAtLeastOnceWhenFirstSnapshotIsLate) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  UiState ui;
  pb::HoverRef ref;
  ref.kind = pb::HoverRef::ITEM;
  ref.id = "6948";
  ref.name = "Hearthstone";
  ASSERT_EQ(ui.Hover(ref, t0), "");
  pb::Ui out;
  EXPECT_TRUE(
      ui.FillAndAge(&out, t0 + UiState::kHoverDuration + base::Seconds(1))
          .has_value());
  ASSERT_EQ(out.hovering.size(), 1u);
  EXPECT_FALSE(
      ui.FillAndAge(&out, t0 + UiState::kHoverDuration + base::Seconds(2))
          .has_value());
  EXPECT_TRUE(out.hovering.empty());
}

TEST(UiStateTest, RefusesWhatItCantPointAt) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  UiState ui;
  pb::HoverRef spell;
  spell.kind = pb::HoverRef::SPELL;
  spell.id = "585";
  EXPECT_NE(ui.Hover(spell, t0), "");
  pb::HoverRef empty;
  empty.kind = pb::HoverRef::QUEST;
  EXPECT_NE(ui.Hover(empty, t0), "");
  EXPECT_FALSE(ui.hover().has_value());
}

}  // namespace
}  // namespace llm_brain
