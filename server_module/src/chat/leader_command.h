// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_CHAT_LEADER_COMMAND_H_
#define MOD_LLM_BRAIN_CHAT_LEADER_COMMAND_H_

#include <cstddef>
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
  kStop,            // "stop": halt moving
  kFollow,          // "follow", "come", "on me"
  kAttackMyTarget,  // "attack", "assist", "attack my target"
  kAttackMark,      // "kill skull", "rti cross", "skull", "cross"
  kDrink,           // "drink", "drink up", "oom", "rest"
  kMount,           // "mount", "mount up"
  kHealMe,          // "@heal", "heal me"
  kMoveToPing,      // "go there", "flag": to the party's minimap ping
  kPullBack,        // "los", "pull back", "fall back": to the speaker, hold
  kPull,            // "pull", "pull skull", "pull my target"
  kLust,            // "lust", "bloodlust", "bl", "hero", "heroism"
  kSpread,          // "spread", "spread out": fan out around the speaker, hold
  kStack,           // "stack", "stack up", "stack on me": stack on speaker
  kStopDps,         // "stop dps", "hold dps", "cease fire": halt attacks
  kResumeDps,       // "resume", "resume dps", "dps", "burn": lift stop dps
  kWipe,            // "wipe", "release", "release spirit": release immediately
  kGoRepair,        // "go repair", "repair", "go vendor": visit repair/vendor
  kGoTrain,         // "go train", "train", "train spells": visit class trainer
};

struct ParsedLeaderCommand {
  LeaderCommand command;
  // The names the line addressed ("loada wait"); empty: the whole group.
  std::vector<std::string> addressed;
  // kAttackMark, kPull: the raid icon named (kRaidIconNames index); a bare
  // "pull" names none.
  std::optional<size_t> mark;
};

// Parses a party line that is nothing but a command, optionally addressed to
// some of `names` (the group's bots) and with filler ("pls", "guys", "now").
// A sentence that merely contains a command word ("I'll wait for the boat")
// is not a command.
std::optional<ParsedLeaderCommand> ParseLeaderCommand(
    std::string_view text,
    const std::vector<std::string>& names);

// "wait", "follow", "kill", "pull", ...: the command's kind, for logs.
const char* LeaderCommandName(LeaderCommand command);

// "wait", "kill skull", "pull cross", ...: how the brain is told what the
// leader said.
std::string DescribeLeaderCommand(const ParsedLeaderCommand& parsed);

// True when `bot_name` should execute `parsed` sent by a group member:
// non-humans never issue leader commands; addressed commands apply only to
// named bots; unaddressed whispers apply to the recipient bot; unaddressed
// party/raid commands require either the group/raid leader or a bot-led group
// (`!leader_is_human`).
bool IsLeaderCommandForBot(bool sender_is_human,
                           bool sender_is_leader,
                           bool leader_is_human,
                           bool is_whisper,
                           const ParsedLeaderCommand& parsed,
                           std::string_view bot_name);

// True when a bot should move for `/rw spread` or `/rw stack`: tanks are
// excluded by default so they don't spin or drag the boss, unless explicitly
// addressed by name (M11).
bool ShouldApplySpreadOrStack(bool is_tank, bool explicitly_addressed);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_CHAT_LEADER_COMMAND_H_
