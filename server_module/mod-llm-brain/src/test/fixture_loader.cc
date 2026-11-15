// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/fixture_loader.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "CharacterDatabase.h"
#include "DatabaseEnv.h"
#include "DatabaseEnvFwd.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerDump.h"
#include "Random.h"

namespace llm_brain {
namespace {

// Async writes normally land within a few ticks; give up after this many polls.
constexpr uint32_t kMaxPolls = 500;

bool CharacterExists(const std::string& name) {
  CharacterDatabasePreparedStatement* stmt =
      CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
  stmt->SetData(0, name);
  return CharacterDatabase.Query(stmt) != nullptr;
}

// The test account is never logged into; its password only has to exist.
std::string RandomPassword() {
  static constexpr char kChars[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::string password;
  for (size_t i = 0; i < MAX_PASS_STR; ++i) {
    password += kChars[urand(0, sizeof(kChars) - 2)];
  }
  return password;
}

}  // namespace

FixtureLoader::FixtureLoader(std::string account, std::vector<Fixture> fixtures)
    : account_(std::move(account)), fixtures_(std::move(fixtures)) {
  if (fixtures_.empty()) {
    step_ = Step::kDone;
  }
}

FixtureLoader::~FixtureLoader() = default;

bool FixtureLoader::Update() {
  switch (step_) {
    case Step::kCreateAccount:
      account_id_ = AccountMgr::GetId(account_);
      if (account_id_) {
        step_ = Step::kDeleteCharacter;
        break;
      }
      if (sAccountMgr->CreateAccount(account_, RandomPassword()) != AOR_OK) {
        Fail("cannot create test account " + account_);
        break;
      }
      LOG_INFO("module", "mod-llm-brain: created test account {}", account_);
      step_ = Step::kWaitForAccount;
      polls_left_ = kMaxPolls;
      break;

    case Step::kWaitForAccount:
      account_id_ = AccountMgr::GetId(account_);
      if (account_id_) {
        step_ = Step::kDeleteCharacter;
      } else if (--polls_left_ == 0) {
        Fail("test account " + account_ + " never appeared");
      }
      break;

    case Step::kDeleteCharacter: {
      const Fixture& fixture = fixtures_[index_];
      const ObjectGuid guid =
          sCharacterCache->GetCharacterGuidByName(fixture.character);
      if (guid.IsEmpty()) {
        step_ = Step::kLoadDump;
        break;
      }
      // Never erase a character that isn't ours to reset.
      if (sCharacterCache->GetCharacterAccountIdByGuid(guid) != account_id_) {
        LOG_ERROR("module",
                  "mod-llm-brain: {} exists on another account; not resetting "
                  "it (test characters must be on {})",
                  fixture.character, account_);
        NextFixture();
        break;
      }
      Player::DeleteFromDB(guid.GetCounter(), account_id_,
                           /*updateRealmChars=*/true, /*deleteFinally=*/true);
      step_ = Step::kWaitForDelete;
      polls_left_ = kMaxPolls;
      break;
    }

    case Step::kWaitForDelete:
      if (!CharacterExists(fixtures_[index_].character)) {
        step_ = Step::kLoadDump;
      } else if (--polls_left_ == 0) {
        Fail(fixtures_[index_].character + " was never deleted");
      }
      break;

    case Step::kLoadDump: {
      const Fixture& fixture = fixtures_[index_];
      const DumpReturn result = PlayerDumpReader().LoadDumpFromFile(
          fixture.file, account_id_, fixture.character, /*guid=*/0);
      if (result != DUMP_SUCCESS) {
        LOG_ERROR("module", "mod-llm-brain: cannot load {} from {} (code {})",
                  fixture.character, fixture.file, static_cast<int>(result));
        NextFixture();
        break;
      }
      step_ = Step::kWaitForLoad;
      polls_left_ = kMaxPolls;
      break;
    }

    case Step::kWaitForLoad:
      if (CharacterExists(fixtures_[index_].character)) {
        LOG_INFO("module", "mod-llm-brain: reset {} from {}",
                 fixtures_[index_].character, fixtures_[index_].file);
        NextFixture();
      } else if (--polls_left_ == 0) {
        Fail(fixtures_[index_].character + " never appeared after loading");
      }
      break;

    case Step::kDone:
      break;
  }
  return step_ == Step::kDone;
}

void FixtureLoader::NextFixture() {
  ++index_;
  step_ = index_ < fixtures_.size() ? Step::kDeleteCharacter : Step::kDone;
}

void FixtureLoader::Fail(const std::string& why) {
  LOG_ERROR("module", "mod-llm-brain: fixture reset failed: {}", why);
  step_ = Step::kDone;
}

}  // namespace llm_brain
