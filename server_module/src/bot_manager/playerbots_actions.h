// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_PLAYERBOTS_ACTIONS_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_PLAYERBOTS_ACTIONS_H_

#include <string>
#include <vector>

namespace llm_brain {

// Every action name playerbots' shared action factories (ActionContext,
// ChatActionContext, WorldPacketActionContext) can create, sorted. Builds
// the factories, so call it once (at startup).
std::vector<std::string> PlayerbotsActionNames();

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_PLAYERBOTS_ACTIONS_H_
