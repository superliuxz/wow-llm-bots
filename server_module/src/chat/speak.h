// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CHAT_SPEAK_H_
#define MOD_LLM_BRAIN_CHAT_SPEAK_H_

#include <string>

#include "proto/snapshot.h"

class Player;

namespace llm_brain {

// Says `say` with the bot's own voice, as a player types it: out loud in its
// race's language (say, yell), or whispered to someone by name. Returns what
// went wrong, phrased like the client's error line, or "" when it was said.
// World thread only.
std::string Speak(Player* bot, const wlb::snapshot::Command::Say& say);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CHAT_SPEAK_H_
