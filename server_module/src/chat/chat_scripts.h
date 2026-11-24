// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CHAT_CHAT_SCRIPTS_H_
#define MOD_LLM_BRAIN_CHAT_CHAT_SCRIPTS_H_

#include <atomic>
#include <string>

namespace llm_brain {

class BehaviorTraceWriter;
class ChatInbox;

// Registers the chat scripts with AzerothCore (which owns them):
// - a player hook that puts every whisper to a brain-run bot into `inbox` and
//   logs human whispers to `*human_trace` when non-null;
// - the console command `.llmbrain whisper <from> <to> <text>`, which makes an
//   online character whisper another, as if typed (tests and live checks).
// `inbox` and `human_trace` must outlive the server.
void AddLlmBrainChatScripts(
    ChatInbox* inbox,
    std::atomic<BehaviorTraceWriter*>* human_trace = nullptr);

// Test builds only (`llmtest testhuman`): the chat hooks treat `name`'s lines
// as a human player's, so e2e cases can stand a bot in for the user. World
// thread only. A no-op in production builds.
void SetTestHuman(const std::string& name, bool human);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CHAT_CHAT_SCRIPTS_H_
