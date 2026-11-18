// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/ui_state.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "proto/snapshot.h"

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

std::string UiState::Hover(const pb::HoverRef& ref) {
  if (ref.kind != pb::HoverRef::ITEM && ref.kind != pb::HoverRef::QUEST) {
    return "that can't be pointed at yet";
  }
  if (ref.id.empty()) {
    return "nothing to point at";
  }
  hover_ = ref;
  hover_snapshots_left_ = kHoverSnapshots;
  return "";
}

void UiState::Reset() {
  open_.clear();
  hover_.reset();
  hover_snapshots_left_ = 0;
}

bool UiState::IsOpen(pb::Panel panel) const {
  return std::find(open_.begin(), open_.end(), panel) != open_.end();
}

void UiState::FillAndAge(pb::Ui* ui) {
  ui->open_panels = open_;
  ui->hovering.clear();
  if (!hover_) {
    return;
  }
  ui->hovering.push_back(*hover_);
  if (--hover_snapshots_left_ <= 0) {
    hover_.reset();
  }
}

}  // namespace llm_brain
