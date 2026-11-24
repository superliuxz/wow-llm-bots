// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_BOT_MANAGER_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_BOT_MANAGER_H_

#include <cstdint>
#include <string>
#include <vector>

class Player;

namespace llm_brain {

// Owns which of our bots are online. Replaces playerbots' random-bot manager,
// which is switched off (Playerbots.RandomBotAutologin = 0): our bots are fixed
// characters with their own memory, so nothing may pick, rotate, teleport or
// refresh them behind our back (docs/playerbots-random-bot-audit.md).
//
// Bots log in through playerbots' public login (no master), so playerbots' own
// session and AI plumbing runs them; the stock AI is the bot's body. Playerbots
// resets a bot's strategies to its "companion" defaults on login and after
// many events, so the manager re-applies ours whenever that happened.
//
// World thread only.
class BotManager {
 public:
  // Strategy changes in playerbots' syntax ("-follow,+grind,+new rpg").
  struct Strategies {
    std::string combat;
    std::string non_combat;
    std::string dead;
  };

  BotManager(std::vector<std::string> names, Strategies strategies);
  BotManager(const BotManager&) = delete;
  BotManager& operator=(const BotManager&) = delete;
  ~BotManager();

  // Every world update: attaches policy listeners on every tick (O(1) hash
  // lookup) and runs Tick() at most once per second to log bots in and keep
  // their strategies applied.
  void Update(uint32_t diff_ms);
  // Logs our bots out (server shutdown).
  void Stop();

  // World thread only: optional per-bot strategy suffix applied on top of the
  // server-wide `strategies_` (used by test benchmarks to mix baseline and
  // stripped-strategy parties in the same wave).
  static void SetBotStrategyOverride(const std::string& bot_name,
                                     std::string combat_extra,
                                     std::string non_combat_extra);
  static void ClearBotStrategyOverride(const std::string& bot_name);
  static void ClearBotStrategyOverrides();

  // World thread only: pending shared-quest / ready-check / LFG-proposal
  // prompts captured via ServerScript::OnPacketSent (works for standalone
  // no-master bots) while BrainPolicyListener blocks Playerbots' auto-handlers.
  static uint32_t GetPendingSharedQuest(Player* bot);
  static void ClearPendingSharedQuest(Player* bot);
  static bool HasPendingReadyCheck(Player* bot);
  static void ClearPendingReadyCheck(Player* bot);
  static uint32_t GetPendingLfgProposal(Player* bot);
  static void ClearPendingLfgProposal(Player* bot);

 private:
  struct Bot {
    std::string name;
    uint64_t guid = 0;  // 0: unknown character name.
    uint64_t next_login_ms = 0;
  };

  void Tick();
  void EnsureStrategies(Player* player);

  const Strategies strategies_;
  std::vector<Bot> bots_;
  uint64_t now_ms_ = 0;
  uint32_t since_tick_ms_ = 0;
};

// Registers the ServerScript that captures outgoing server prompts
// (MSG_RAID_READY_CHECK, SMSG_QUESTGIVER_QUEST_DETAILS with divider,
// SMSG_LFG_PROPOSAL_UPDATE) for standalone bots.
void AddLlmBrainBotManagerScripts();

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_BOT_MANAGER_H_
