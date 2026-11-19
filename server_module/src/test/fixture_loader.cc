// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/fixture_loader.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "CharacterDatabase.h"
#include "Common.h"
#include "DatabaseEnv.h"
#include "DatabaseEnvFwd.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerDump.h"
#include "Random.h"
#include "SharedDefines.h"
#include "WorldSession.h"
#include "base/time/time.h"
#include "test/test_accounts.h"

namespace llm_brain {
namespace {

// Async writes normally land within a few seconds, even with dozens queued.
constexpr base::TimeDelta kWaitTimeout = base::Seconds(30);

bool CharacterExists(const std::string& name) {
  CharacterDatabasePreparedStatement* stmt =
      CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
  stmt->SetData(0, name);
  return CharacterDatabase.Query(stmt) != nullptr;
}

// Test accounts are never logged into; the password only has to exist.
std::string RandomPassword() {
  static constexpr char kChars[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::string password;
  for (size_t i = 0; i < MAX_PASS_STR; ++i) {
    password += kChars[urand(0, sizeof(kChars) - 2)];
  }
  return password;
}

bool ParseNewSpec(std::string_view spec, uint8_t* race_out, uint8_t* cls_out) {
  if (!spec.starts_with("new:")) {
    return false;
  }
  spec.remove_prefix(4);
  const size_t colon = spec.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  const std::string_view race_str = spec.substr(0, colon);
  const std::string_view cls_str = spec.substr(colon + 1);

  uint8_t race = 0;
  if (race_str == "human")
    race = RACE_HUMAN;
  else if (race_str == "orc")
    race = RACE_ORC;
  else if (race_str == "dwarf")
    race = RACE_DWARF;
  else if (race_str == "nightelf")
    race = RACE_NIGHTELF;
  else if (race_str == "undead")
    race = RACE_UNDEAD_PLAYER;
  else if (race_str == "tauren")
    race = RACE_TAUREN;
  else if (race_str == "gnome")
    race = RACE_GNOME;
  else if (race_str == "troll")
    race = RACE_TROLL;
  else if (race_str == "bloodelf")
    race = RACE_BLOODELF;
  else if (race_str == "draenei")
    race = RACE_DRAENEI;
  else
    return false;

  uint8_t cls = 0;
  if (cls_str == "warrior")
    cls = CLASS_WARRIOR;
  else if (cls_str == "paladin")
    cls = CLASS_PALADIN;
  else if (cls_str == "hunter")
    cls = CLASS_HUNTER;
  else if (cls_str == "rogue")
    cls = CLASS_ROGUE;
  else if (cls_str == "priest")
    cls = CLASS_PRIEST;
  else if (cls_str == "shaman")
    cls = CLASS_SHAMAN;
  else if (cls_str == "mage")
    cls = CLASS_MAGE;
  else if (cls_str == "warlock")
    cls = CLASS_WARLOCK;
  else if (cls_str == "druid")
    cls = CLASS_DRUID;
  else
    return false;

  *race_out = race;
  *cls_out = cls;
  return true;
}

bool CreateFreshCharacter(uint32_t account_id,
                          const std::string& name,
                          uint8_t race,
                          uint8_t cls) {
  auto session = std::make_unique<WorldSession>(
      account_id, "", 0x0, nullptr, SEC_PLAYER,
      EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0, false, false,
      0);
  CharacterCreateInfo info(name, race, cls, GENDER_MALE, 0, 0, 0, 0, 0);
  auto player = std::make_unique<Player>(session.get());
  player->GetMotionMaster()->Initialize();
  if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(),
                      &info)) {
    player->CleanupsBeforeDelete();
    return false;
  }
  player->setCinematic(2);
  player->SetAtLoginFlag(AT_LOGIN_NONE);
  player->SaveToDB(true, false);
  sCharacterCache->AddCharacterCacheEntry(
      player->GetGUID(), account_id, player->GetName(), player->getGender(),
      player->getRace(), player->getClass(), player->GetLevel());
  player->CleanupsBeforeDelete();
  return true;
}

}  // namespace

FixtureLoader::FixtureLoader(std::string account, std::vector<Fixture> fixtures)
    : account_(std::move(account)), fixtures_(std::move(fixtures)) {
  if (fixtures_.empty()) {
    step_ = Step::kDone;
  }
}

FixtureLoader::~FixtureLoader() = default;

void FixtureLoader::StartWait(Step step) {
  step_ = step;
  deadline_ = base::TimeTicks::Now() + kWaitTimeout;
}

bool FixtureLoader::TimedOut() const {
  return base::TimeTicks::Now() > deadline_;
}

bool FixtureLoader::Update() {
  const std::string account = TestAccountName(account_, pool_index_);
  switch (step_) {
    case Step::kCreateAccount:
      account_id_ = AccountMgr::GetId(account);
      if (account_id_) {
        step_ = Step::kDeleteCharacter;
        break;
      }
      if (sAccountMgr->CreateAccount(account, RandomPassword()) != AOR_OK) {
        Fail("cannot create test account " + account);
        break;
      }
      LOG_INFO("module", "mod-llm-brain: created test account {}", account);
      StartWait(Step::kWaitForAccount);
      break;

    case Step::kWaitForAccount:
      account_id_ = AccountMgr::GetId(account);
      if (account_id_) {
        step_ = Step::kDeleteCharacter;
      } else if (TimedOut()) {
        Fail("test account " + account + " never appeared");
      }
      break;

    case Step::kDeleteCharacter: {
      const Fixture& fixture = fixtures_[index_];
      // An invalid name would make LoadDump quietly fall back to a temporary
      // name (with a rename at login), so reject it here.
      if (ObjectMgr::CheckPlayerName(fixture.character, /*create=*/true) !=
          CHAR_NAME_SUCCESS) {
        LOG_ERROR("module",
                  "mod-llm-brain: {} is not a valid character name; skipped",
                  fixture.character);
        NextFixture();
        break;
      }
      const ObjectGuid guid =
          sCharacterCache->GetCharacterGuidByName(fixture.character);
      if (guid.IsEmpty()) {
        step_ = Step::kLoadDump;
        break;
      }
      // Never erase a character that isn't ours to reset.
      const uint32_t owner = sCharacterCache->GetCharacterAccountIdByGuid(guid);
      std::string owner_name;
      if (!AccountMgr::GetName(owner, owner_name) ||
          !IsTestAccountName(owner_name, account_)) {
        LOG_ERROR("module",
                  "mod-llm-brain: {} exists on account {}; not resetting it "
                  "(test characters must be on {}, {}2, ...)",
                  fixture.character, owner_name, account_, account_);
        NextFixture();
        break;
      }
      Player::DeleteFromDB(guid.GetCounter(), owner,
                           /*updateRealmChars=*/true, /*deleteFinally=*/true);
      StartWait(Step::kWaitForDelete);
      break;
    }

    case Step::kWaitForDelete:
      if (!CharacterExists(fixtures_[index_].character)) {
        step_ = Step::kLoadDump;
      } else if (TimedOut()) {
        Fail(fixtures_[index_].character + " was never deleted");
      }
      break;

    case Step::kLoadDump: {
      const Fixture& fixture = fixtures_[index_];
      uint8_t race = 0;
      uint8_t cls = 0;
      if (ParseNewSpec(fixture.file, &race, &cls)) {
        if (AccountMgr::GetCharactersCount(account_id_) >= 10) {
          ++pool_index_;
          step_ = Step::kCreateAccount;
          break;
        }
        if (!CreateFreshCharacter(account_id_, fixture.character, race, cls)) {
          LOG_ERROR(
              "module",
              "mod-llm-brain: cannot create {} from {} (race={} class={})",
              fixture.character, fixture.file, static_cast<int>(race),
              static_cast<int>(cls));
          NextFixture();
          break;
        }
        StartWait(Step::kWaitForLoad);
        break;
      }
      const DumpReturn result = PlayerDumpReader().LoadDumpFromFile(
          fixture.file, account_id_, fixture.character, /*guid=*/0);
      if (result == DUMP_TOO_MANY_CHARS) {
        // This account is full: same fixture, next account in the pool.
        ++pool_index_;
        step_ = Step::kCreateAccount;
        break;
      }
      if (result != DUMP_SUCCESS) {
        LOG_ERROR("module", "mod-llm-brain: cannot load {} from {} (code {})",
                  fixture.character, fixture.file, static_cast<int>(result));
        NextFixture();
        break;
      }
      StartWait(Step::kWaitForLoad);
      break;
    }

    case Step::kWaitForLoad:
      if (CharacterExists(fixtures_[index_].character)) {
        LOG_INFO("module", "mod-llm-brain: reset {} on {} from {}",
                 fixtures_[index_].character, account, fixtures_[index_].file);
        NextFixture();
      } else if (TimedOut()) {
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
