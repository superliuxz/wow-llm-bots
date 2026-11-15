// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_TEST_FIXTURE_LOADER_H_
#define MOD_LLM_BRAIN_TEST_FIXTURE_LOADER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace llm_brain {

// Test only. Resets test characters to a known state at server startup: each
// is erased and recreated from a player dump (AzerothCore's `pdump` format,
// tests/fixtures/characters/*.pdump), so every test run starts the same.
//
// Only characters on the test account are ever erased; the account is created
// if missing. AzerothCore queues character deletes, dump loads and account
// creation as async writes, so each step is confirmed with a direct query on a
// later tick before the next one starts. Those blocking queries run on the
// world thread, which is acceptable only because this runs in tests, during
// the first seconds after startup.
//
// World thread only.
class FixtureLoader {
 public:
  struct Fixture {
    std::string character;  // Name of the character to (re)create.
    std::string file;       // Path of the .pdump file.
  };

  FixtureLoader(std::string account, std::vector<Fixture> fixtures);
  FixtureLoader(const FixtureLoader&) = delete;
  FixtureLoader& operator=(const FixtureLoader&) = delete;
  ~FixtureLoader();

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

  void NextFixture();
  void Fail(const std::string& why);

  const std::string account_;
  const std::vector<Fixture> fixtures_;
  Step step_ = Step::kCreateAccount;
  uint32_t account_id_ = 0;
  size_t index_ = 0;
  // Polls left before the current wait step gives up.
  uint32_t polls_left_ = 0;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_TEST_FIXTURE_LOADER_H_
