// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/bot_manager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "AiFactory.h"
#include "CharacterCache.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "config/config_parsing.h"

namespace llm_brain {
namespace {

constexpr uint32_t kTickMs = 1000;
// A failed or dropped login is retried after this long.
constexpr uint32_t kLoginRetryMs = 10000;

// The solo combat extras playerbots' AiFactory gives random bots that aren't
// in a group (AddDefaultCombatStrategies): healers also deal damage.
std::string SoloHealerCombatExtras(Player* player) {
  const uint8_t tab = AiFactory::GetPlayerSpecTab(player);
  switch (player->getClass()) {
    case CLASS_PRIEST:
      return tab != PRIEST_TAB_SHADOW ? ",+holy dps,+shadow debuff,+shadow aoe"
                                      : "";
    case CLASS_DRUID:
      return tab == DRUID_TAB_RESTORATION ? ",+aoe" : "";
    case CLASS_SHAMAN:
      return tab == SHAMAN_TAB_RESTORATION ? ",+ele,+caster aoe" : "";
    case CLASS_PALADIN:
      return tab == PALADIN_TAB_HOLY ? ",+ret,+baoe" : "";
    default:
      return "";
  }
}

}  // namespace

BotManager::BotManager(std::vector<std::string> names, Strategies strategies)
    : strategies_(std::move(strategies)) {
  for (std::string& name : names) {
    bots_.push_back(Bot{std::move(name)});
  }
}

BotManager::~BotManager() = default;

void BotManager::Update(uint32_t diff_ms) {
  now_ms_ += diff_ms;
  since_tick_ms_ += diff_ms;
  if (since_tick_ms_ < kTickMs) {
    return;
  }
  since_tick_ms_ = 0;
  Tick();
}

void BotManager::Tick() {
  for (Bot& bot : bots_) {
    if (!bot.guid) {
      const ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(bot.name);
      if (guid.IsEmpty()) {
        if (now_ms_ >= bot.next_login_ms) {
          LOG_ERROR("module", "mod-llm-brain: no character named {}", bot.name);
          bot.next_login_ms = now_ms_ + kLoginRetryMs;
        }
        continue;
      }
      bot.guid = guid.GetRawValue();
    }
    const ObjectGuid guid(bot.guid);
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (player) {
      // Connected. Between maps (a teleport, an instance) it is briefly out
      // of the world: wait, never log it in a second time.
      if (player->IsInWorld()) {
        EnsureStrategies(player);
      }
      continue;
    }
    if (now_ms_ >= bot.next_login_ms) {
      // No master: playerbots runs it as a standalone bot. With autologin off,
      // its random-bot manager does nothing else with it.
      LOG_INFO("module", "mod-llm-brain: logging in {}", bot.name);
      RandomPlayerbotMgr::instance().AddPlayerBot(guid, 0);
      bot.next_login_ms = now_ms_ + kLoginRetryMs;
    }
  }
}

void BotManager::EnsureStrategies(Player* player) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
  if (!ai || player->InBattleground()) {
    return;  // Not set up yet, or playerbots' battleground strategies apply.
  }
  // Solo, a healer also deals damage; in a group it heals, so the same extras
  // are removed (",+holy dps" becomes ",-holy dps"): joining a group must undo
  // what being solo added.
  std::string extras = SoloHealerCombatExtras(player);
  if (player->GetGroup()) {
    for (size_t at = extras.find(",+"); at != std::string::npos;
         at = extras.find(",+", at)) {
      extras[at + 1] = '-';
    }
  }
  const std::string combat = strategies_.combat + extras;
  // Still as we left it in every state: playerbots hasn't reset any of them
  // since (it resets combat alone too, e.g. SelectiveResetStrategies).
  const auto holds = [&](const std::string& changes, BotState state) {
    for (const StrategyCheck& check : StrategyChecks(changes)) {
      if (ai->HasStrategy(check.name, state) != check.present) {
        return false;
      }
    }
    return true;
  };
  if (holds(combat, BOT_STATE_COMBAT) &&
      holds(strategies_.non_combat, BOT_STATE_NON_COMBAT) &&
      holds(strategies_.dead, BOT_STATE_DEAD)) {
    return;
  }
  ai->ChangeStrategy(combat, BOT_STATE_COMBAT);
  ai->ChangeStrategy(strategies_.non_combat, BOT_STATE_NON_COMBAT);
  ai->ChangeStrategy(strategies_.dead, BOT_STATE_DEAD);
  LOG_INFO("module", "mod-llm-brain: applied strategies to {}",
           player->GetName());
}

void BotManager::Stop() {
  for (const Bot& bot : bots_) {
    if (bot.guid) {
      RandomPlayerbotMgr::instance().LogoutPlayerBot(ObjectGuid(bot.guid));
    }
  }
}

}  // namespace llm_brain
