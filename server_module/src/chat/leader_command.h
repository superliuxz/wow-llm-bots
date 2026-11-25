// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CHAT_LEADER_COMMAND_H_
#define MOD_LLM_BRAIN_CHAT_LEADER_COMMAND_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llm_brain {

// The party-chat words a human leader types to direct the group, applied by
// C++ within a tick (no LLM round trip), then shown to the brain so the bot
// can answer in character (Now #2, the trusted fast channel).
enum class LeaderCommand {
  kWait,            // "wait", "stay", "hold"
  kFollow,          // "follow", "come", "on me"
  kAttackMyTarget,  // "attack", "assist", "attack my target"
  kAttackSkull,     // "kill skull", "rti skull", "skull"
  kDrink,           // "drink", "drink up", "oom", "rest"
  kMount,           // "mount", "mount up"
  kHealMe,          // "@heal", "heal me"
};

struct ParsedLeaderCommand {
  LeaderCommand command;
  // The names the line addressed ("loada wait"); empty: the whole group.
  std::vector<std::string> addressed;
};

// Parses a party line that is nothing but a command, optionally addressed to
// some of `names` (the group's bots) and with filler ("pls", "guys", "now").
// A sentence that merely contains a command word ("I'll wait for the boat")
// is not a command.
std::optional<ParsedLeaderCommand> ParseLeaderCommand(
    std::string_view text,
    const std::vector<std::string>& names);

// "wait", "follow", ...: how the brain is told what was applied.
const char* LeaderCommandName(LeaderCommand command);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CHAT_LEADER_COMMAND_H_
