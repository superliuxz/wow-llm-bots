// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_H_

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

  // Every world update: logs bots in (and back in after a drop) and keeps
  // their strategies applied. Does real work at most once per second.
  void Update(uint32_t diff_ms);
  // Logs our bots out (server shutdown).
  void Stop();

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

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_H_
