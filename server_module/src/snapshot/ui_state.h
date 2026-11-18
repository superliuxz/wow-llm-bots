// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_UI_STATE_H_
#define MOD_LLM_BRAIN_SNAPSHOT_UI_STATE_H_

#include <optional>
#include <string>
#include <vector>

#include "proto/snapshot.h"

namespace llm_brain {

// What one bot has open and points at in its client
// (docs/plans/options-and-panels.md): the panels the `open` / `close` commands
// opened, and the thing the last `hover` names, whose tooltip shows for a few
// snapshots. Nothing of it goes to the game: the client opens bags and the
// quest log without a packet.
class UiState {
 public:
  // Snapshots a tooltip stays after a hover (about 5 s out of combat).
  static constexpr int kHoverSnapshots = 5;

  UiState();
  UiState(const UiState&);
  UiState& operator=(const UiState&);
  ~UiState();

  // Empty on success, else what went wrong, phrased like the client's error
  // line: only the panels the module fills can open.
  std::string Open(wlb::snapshot::Panel panel);
  void Close(wlb::snapshot::Panel panel);
  void CloseAll();
  std::string Hover(const wlb::snapshot::HoverRef& ref);
  void Reset();

  bool IsOpen(wlb::snapshot::Panel panel) const;
  const std::optional<wlb::snapshot::HoverRef>& hover() const { return hover_; }

  // Writes `ui` for a snapshot being built, and counts that snapshot against
  // the hover (gone after kHoverSnapshots snapshots).
  void FillAndAge(wlb::snapshot::Ui* ui);

 private:
  std::vector<wlb::snapshot::Panel> open_;
  std::optional<wlb::snapshot::HoverRef> hover_;
  int hover_snapshots_left_ = 0;
};

// The panels the module can fill so far.
bool IsSupportedPanel(wlb::snapshot::Panel panel);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_UI_STATE_H_
