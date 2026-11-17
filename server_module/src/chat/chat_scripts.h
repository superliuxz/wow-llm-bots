// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CHAT_CHAT_SCRIPTS_H_
#define MOD_LLM_BRAIN_CHAT_CHAT_SCRIPTS_H_

namespace llm_brain {

class ChatInbox;

// Registers the chat scripts with AzerothCore (which owns them):
// - a player hook that puts every whisper to a brain-run bot into `inbox`;
// - the console command `.llmbrain whisper <from> <to> <text>`, which makes an
//   online character whisper another, as if typed (tests and live checks).
// `inbox` must outlive the server.
void AddLlmBrainChatScripts(ChatInbox* inbox);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CHAT_CHAT_SCRIPTS_H_
