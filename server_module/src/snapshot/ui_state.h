// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_UI_STATE_H_
#define MOD_LLM_BRAIN_SNAPSHOT_UI_STATE_H_

#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

// What one bot has open and points at in its client
// (docs/plans/options-and-panels.md): the panels the `open` / `close` commands
// opened, and the thing the last `hover` names, whose tooltip shows for
// kHoverDuration. Nothing of it goes to the game: the client opens bags and the
// quest log without a packet.
class UiState {
 public:
  // How long a tooltip stays after a hover (steady across 1 s idle snapshots
  // and 250 ms combat snapshots; at least one snapshot always shows it).
  static constexpr base::TimeDelta kHoverDuration = base::Seconds(5);

  UiState();
  UiState(const UiState&);
  UiState& operator=(const UiState&);
  ~UiState();

  // Empty on success, else what went wrong, phrased like the client's error
  // line: only the panels the module fills can open.
  std::string Open(wlb::snapshot::Panel panel);
  void Close(wlb::snapshot::Panel panel);
  void CloseAll();
  std::string Hover(const wlb::snapshot::HoverRef& ref, base::TimeTicks now);
  void Reset();

  bool IsOpen(wlb::snapshot::Panel panel) const;
  const std::optional<wlb::snapshot::HoverRef>& hover() const { return hover_; }

  // Writes `ui` for a snapshot built at `now`, expiring the hover once
  // kHoverDuration has elapsed (after showing on at least one snapshot), and
  // returns the active hover for this snapshot.
  std::optional<wlb::snapshot::HoverRef> FillAndAge(wlb::snapshot::Ui* ui,
                                                    base::TimeTicks now);

 private:
  std::vector<wlb::snapshot::Panel> open_;
  std::optional<wlb::snapshot::HoverRef> hover_;
  base::TimeTicks hover_until_;
  bool hover_shown_ = false;
};

// The panels the module can fill so far.
bool IsSupportedPanel(wlb::snapshot::Panel panel);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_UI_STATE_H_
