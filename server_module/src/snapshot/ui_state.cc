// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/ui_state.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

namespace pb = wlb::snapshot;

bool IsSupportedPanel(pb::Panel panel) {
  return panel == pb::BAGS || panel == pb::CHARACTER || panel == pb::QUEST_LOG;
}

UiState::UiState() = default;
UiState::UiState(const UiState&) = default;
UiState& UiState::operator=(const UiState&) = default;
UiState::~UiState() = default;

std::string UiState::Open(pb::Panel panel) {
  if (!IsSupportedPanel(panel)) {
    return "that panel can't be opened yet";
  }
  if (!IsOpen(panel)) {
    open_.push_back(panel);
  }
  return "";
}

void UiState::Close(pb::Panel panel) {
  std::erase(open_, panel);
}

void UiState::CloseAll() {
  open_.clear();
}

std::string UiState::Hover(const pb::HoverRef& ref, base::TimeTicks now) {
  if (ref.kind != pb::HoverRef::ITEM && ref.kind != pb::HoverRef::QUEST) {
    return "that can't be pointed at yet";
  }
  if (ref.id.empty()) {
    return "nothing to point at";
  }
  hover_ = ref;
  hover_until_ = now + kHoverDuration;
  hover_shown_ = false;
  return "";
}

void UiState::Reset() {
  open_.clear();
  hover_.reset();
  hover_until_ = base::TimeTicks();
  hover_shown_ = false;
}

bool UiState::IsOpen(pb::Panel panel) const {
  return std::find(open_.begin(), open_.end(), panel) != open_.end();
}

std::optional<pb::HoverRef> UiState::FillAndAge(pb::Ui* ui,
                                                base::TimeTicks now) {
  ui->open_panels = open_;
  ui->hovering.clear();
  if (!hover_) {
    return std::nullopt;
  }
  if (hover_shown_ && now >= hover_until_) {
    hover_.reset();
    return std::nullopt;
  }
  hover_shown_ = true;
  ui->hovering.push_back(*hover_);
  return hover_;
}

}  // namespace llm_brain
