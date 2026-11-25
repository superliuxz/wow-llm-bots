// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_CHARACTER_LOADER_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_CHARACTER_LOADER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/time/time.h"

namespace llm_brain {

// Creates bot characters at server startup. Two kinds of entries:
//  - Test fixtures (test builds): each character is erased and recreated from
//    a player dump (AzerothCore's `pdump` format,
//    server_module/testdata/characters/*.pdump) or as a fresh `new:race:class`
//    character, so every test run starts the same.
//  - `create_only` entries (LlmBrain.CreateBots, every build): a fresh
//    `new:race:class` character is created only when none by that name exists,
//    so a play preset's party exists on first start and keeps its progress.
//
// Characters live on a pool of bot accounts (`account`, `account`2, ...;
// bot_accounts.h), created when missing; a load that finds an account full
// moves on to the next. Only characters on pool accounts are ever erased.
//
// AzerothCore queues character deletes, dump loads and account creation as
// async writes, so each step is confirmed with a direct query on later ticks
// before the next one starts. Those blocking queries run on the world thread,
// which is acceptable only because this runs once, during the first seconds
// after startup, before any bot logs in.
//
// World thread only.
class CharacterLoader {
 public:
  struct Fixture {
    std::string character;  // Name of the character to (re)create.
    std::string file;       // Path of the .pdump file, or `new:race:class`.
    // Never erase or reset: only create a missing `new:` character.
    bool create_only = false;
  };

  CharacterLoader(std::string account, std::vector<Fixture> fixtures);
  CharacterLoader(const CharacterLoader&) = delete;
  CharacterLoader& operator=(const CharacterLoader&) = delete;
  ~CharacterLoader();

  // Advances the reset by at most one step per call. Returns true once every
  // fixture is loaded or has failed (failures are logged).
  bool Update();

 private:
  enum class Step {
    kCreateAccount,
    kWaitForAccount,
    kDeleteCharacter,
    kWaitForDelete,
    kLoadDump,
    kWaitForLoad,
    kDone,
  };

  void StartWait(Step step);
  bool TimedOut() const;
  void NextFixture();
  void Fail(const std::string& why);

  const std::string account_;
  const std::vector<Fixture> fixtures_;
  Step step_ = Step::kCreateAccount;
  size_t index_ = 0;
  // The pool account fixtures currently go on, and its id once known.
  size_t pool_index_ = 0;
  uint32_t account_id_ = 0;
  base::TimeTicks deadline_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_CHARACTER_LOADER_H_
