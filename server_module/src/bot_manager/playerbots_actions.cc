// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/playerbots_actions.h"

#include <algorithm>
#include <string>
#include <vector>

#include "ActionContext.h"
#include "ChatActionContext.h"
#include "WorldPacketActionContext.h"

namespace llm_brain {

std::vector<std::string> PlayerbotsActionNames() {
  std::vector<std::string> names;
  // supports() hands back playerbots' own ordered set of names.
  auto add = [&names](const auto& supported) {
    names.insert(names.end(), supported.begin(), supported.end());
  };
  add(ActionContext().supports());
  add(ChatActionContext().supports());
  add(WorldPacketActionContext().supports());
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

}  // namespace llm_brain
