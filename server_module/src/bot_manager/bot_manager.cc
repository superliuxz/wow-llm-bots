// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/bot_manager.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Action.h"
#include "AiFactory.h"
#include "CharacterCache.h"
#include "Corpse.h"
#include "DBCStores.h"
#include "Engine.h"
#include "Event.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "config/config_parsing.h"

namespace llm_brain {
namespace {

constexpr uint32_t kTickMs = 1000;
// A failed or dropped login is retried after this long.
constexpr uint32_t kLoginRetryMs = 10000;
// Seconds a ghost whose corpse is inside a dungeon waits before cross-map
// dungeon entrance recovery fires (after Playerbots' FindCorpseAction fails
// to path across maps).
constexpr int64_t kStuckDungeonGhostSec = 30;

struct BotStrategyOverride {
  std::string combat;
  std::string non_combat;
};

// World-thread-only set of Engines that currently own a
// BrainPolicyListener. Keyed by uintptr_t so the listener's destructor can
// deregister itself when ActionExecutionListeners::~ActionExecutionListeners()
// deletes it on bot logout/shutdown, preventing duplicate registrations and
// address-reuse bugs.
std::unordered_set<uintptr_t>& PolicyListenerEngines() {
  static auto* set = new std::unordered_set<uintptr_t>();
  return *set;
}

// Matches AzerothCore Spell::CheckItems for SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY
// (Spell.cpp:7857-7869) so Playerbots' `"apply stone"` / `"apply oil"` reflex
// does not loop every 5 s with SPELL_FAILED_LOWLEVEL on low-ilvl weapons.
bool CanApplyTempEnchant(Player* bot, Item* consumable, Item* weapon) {
  if (!bot || !consumable || !weapon ||
      weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0) {
    return false;
  }
  if (bot->CanUseItem(consumable) != EQUIP_ERR_OK) {
    return false;
  }
  const ItemTemplate* cproto = consumable->GetTemplate();
  const ItemTemplate* wproto = weapon->GetTemplate();
  if (!cproto || !wproto) {
    return false;
  }
  uint32_t spell_id = 0;
  for (uint8_t i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
    if (cproto->Spells[i].SpellId > 0) {
      spell_id = static_cast<uint32_t>(cproto->Spells[i].SpellId);
      break;
    }
  }
  const SpellInfo* spell =
      spell_id ? sSpellMgr->GetSpellInfo(spell_id) : nullptr;
  if (!spell) {
    return false;
  }
  if (!spell->HasAttribute(SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF)) {
    uint32_t req_level = wproto->RequiredLevel;
    if (!req_level) {
      req_level = wproto->ItemLevel;
    }
    if (req_level < spell->BaseLevel) {
      return false;
    }
  }
  if (spell->MaxLevel > 0 && wproto->ItemLevel > spell->MaxLevel) {
    return false;
  }
  return true;
}

bool HasEligibleTempEnchant(Player* bot, PlayerbotAI* ai, bool is_stone) {
  if (!bot || !ai) {
    return false;
  }
  for (uint8_t slot : {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND}) {
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!weapon || weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0) {
      continue;
    }
    Item* consumable =
        is_stone ? ai->FindStoneFor(weapon) : ai->FindOilFor(weapon);
    if (CanApplyTempEnchant(bot, consumable, weapon)) {
      return true;
    }
  }
  return false;
}

bool HasLivingGroupMemberOnSameMap(Player* bot) {
  if (!bot || !bot->GetGroup()) {
    return false;
  }
  for (GroupReference* itr = bot->GetGroup()->GetFirstMember(); itr != nullptr;
       itr = itr->next()) {
    Player* member = itr->GetSource();
    if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
        member->GetMapId() == bot->GetMapId() &&
        member->GetInstanceId() == bot->GetInstanceId()) {
      return true;
    }
  }
  return false;
}

// Enforces clean ownership boundaries between the LLM brain and Playerbots'
// reflex layer on each Engine:
//  - Blocks `"accept invitation"` so the brain decides group invites.
//  - Blocks `"equip upgrades packet action"` / `"equip upgrade"` so Playerbots'
//    auto-equipper never fights the brain's `Equip` choices.
//  - Gates `"apply stone"` / `"apply oil"` on actual AzerothCore temp-enchant
//    eligibility to stop SPELL_FAILED_LOWLEVEL 5-second spam loops.
//  - Gates `"auto release"` inside dungeons when a living party member is on
//    the same map so standalone bots wait for a healer rez (instead of
//    releasing after 4 s to an outside graveyard).
// Owned by the Engine's ActionExecutionListeners (which `delete`s every
// listener on Engine destruction).
class BrainPolicyListener final : public ActionExecutionListener {
 public:
  BrainPolicyListener(uintptr_t engine_key, Player* bot, PlayerbotAI* ai)
      : engine_key_(engine_key), bot_(bot), ai_(ai) {
    PolicyListenerEngines().insert(engine_key_);
  }
  ~BrainPolicyListener() override {
    PolicyListenerEngines().erase(engine_key_);
  }
  bool Before(Action* /*action*/, Event /*event*/) override { return true; }
  bool AllowExecution(Action* action, Event /*event*/) override {
    if (!action) {
      return true;
    }
    const std::string& name = action->getName();
    if (name == "accept invitation" || name == "equip upgrades packet action" ||
        name == "equip upgrade") {
      return false;
    }
    if (name == "apply stone") {
      return HasEligibleTempEnchant(bot_, ai_, /*is_stone=*/true);
    }
    if (name == "apply oil") {
      return HasEligibleTempEnchant(bot_, ai_, /*is_stone=*/false);
    }
    if (name == "auto release" && bot_ && bot_->GetMap() &&
        bot_->GetMap()->IsDungeon() && HasLivingGroupMemberOnSameMap(bot_)) {
      return false;
    }
    return true;
  }
  void After(Action* /*action*/, bool /*executed*/, Event /*event*/) override {}
  bool OverrideResult(Action* /*action*/,
                      bool executed,
                      Event /*event*/) override {
    return executed;
  }

 private:
  uintptr_t engine_key_;
  Player* bot_;
  PlayerbotAI* ai_;
};

void EnsurePolicyListener(Player* player, PlayerbotAI* ai) {
  if (!player || !ai) {
    return;
  }
  if (Engine* engine = ai->GetCurrentEngine()) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(engine);
    if (PolicyListenerEngines().contains(key)) {
      return;
    }
    engine->AddActionExecutionListener(
        new BrainPolicyListener(key, player, ai));
  }
}

// Recovers a ghost stuck at an outside graveyard whose corpse is inside a
// dungeon (Playerbots' FindCorpseAction fails cross-map pathing). Enters the
// dungeon entrance portal via AzerothCore's AreaTriggerTeleport (the standard
// WoW dungeon corpse-run mechanic).
void RecoverStuckDungeonGhost(Player* player) {
  if (!player || !player->HasPlayerFlag(PLAYER_FLAGS_GHOST) ||
      player->isMoving() || player->IsBeingTeleported()) {
    return;
  }
  Corpse* corpse = player->GetCorpse();
  if (!corpse || corpse->GetMapId() == player->GetMapId()) {
    return;
  }
  const MapEntry* corpse_map = sMapStore.LookupEntry(corpse->GetMapId());
  if (!corpse_map || !corpse_map->IsDungeon()) {
    return;
  }
  const int64_t dead_sec =
      static_cast<int64_t>(std::time(nullptr)) - corpse->GetGhostTime();
  if (dead_sec < kStuckDungeonGhostSec) {
    return;
  }
  const AreaTriggerTeleport* entrance =
      sObjectMgr->GetMapEntranceTrigger(corpse->GetMapId());
  player->ResurrectPlayer(1.0f);
  player->SpawnCorpseBones();
  if (entrance) {
    player->TeleportTo(entrance->target_mapId, entrance->target_X,
                       entrance->target_Y, entrance->target_Z,
                       entrance->target_Orientation);
  }
  LOG_INFO("module",
           "mod-llm-brain: recovered dungeon ghost {} into map {} entrance",
           player->GetName(), corpse->GetMapId());
}

// World-thread-only per-bot strategy overrides.
std::unordered_map<std::string, BotStrategyOverride>& BotOverrides() {
  static auto* map = new std::unordered_map<std::string, BotStrategyOverride>();
  return *map;
}

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
        RecoverStuckDungeonGhost(player);
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
  EnsurePolicyListener(player, ai);
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
  // Role-aware combat strategies: never apply "+dps assist" or "-threat" to
  // tanks (which would strip playerbots' tank assist / threat / pull behavior).
  std::string base_combat = strategies_.combat;
  if (PlayerbotAI::IsTank(player)) {
    std::vector<std::string> kept;
    for (const StrategyCheck& check : StrategyChecks(base_combat)) {
      if (check.name == "dps assist" || check.name == "threat") {
        continue;
      }
      kept.push_back((check.present ? "+" : "-") + check.name);
    }
    base_combat.clear();
    for (size_t i = 0; i < kept.size(); ++i) {
      if (i > 0) {
        base_combat.push_back(',');
      }
      base_combat += kept[i];
    }
  }
  std::string combat = base_combat + extras;
  Group* group = player->GetGroup();
  const bool in_group = group != nullptr;
  const bool is_leader =
      in_group && group->GetLeaderGUID() == player->GetGUID();
  const bool in_dungeon =
      player->GetMap() != nullptr && player->GetMap()->IsDungeon();
  std::string non_combat = AdjustNonCombatStrategies(
      strategies_.non_combat, in_group, is_leader, in_dungeon);
  if (auto it = BotOverrides().find(player->GetName());
      it != BotOverrides().end()) {
    if (!it->second.combat.empty()) {
      if (!combat.empty()) {
        combat.push_back(',');
      }
      combat += it->second.combat;
    }
    if (!it->second.non_combat.empty()) {
      if (!non_combat.empty()) {
        non_combat.push_back(',');
      }
      non_combat += it->second.non_combat;
    }
  }
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
      holds(non_combat, BOT_STATE_NON_COMBAT) &&
      holds(strategies_.dead, BOT_STATE_DEAD)) {
    return;
  }
  ai->ChangeStrategy(combat, BOT_STATE_COMBAT);
  ai->ChangeStrategy(non_combat, BOT_STATE_NON_COMBAT);
  ai->ChangeStrategy(strategies_.dead, BOT_STATE_DEAD);
  LOG_INFO("module", "mod-llm-brain: applied strategies to {}",
           player->GetName());
}

void BotManager::SetBotStrategyOverride(const std::string& bot_name,
                                        std::string combat_extra,
                                        std::string non_combat_extra) {
  BotOverrides()[bot_name] =
      BotStrategyOverride{std::move(combat_extra), std::move(non_combat_extra)};
}

void BotManager::ClearBotStrategyOverride(const std::string& bot_name) {
  BotOverrides().erase(bot_name);
}

void BotManager::ClearBotStrategyOverrides() {
  BotOverrides().clear();
}

void BotManager::Stop() {
  ClearBotStrategyOverrides();
  for (const Bot& bot : bots_) {
    if (bot.guid) {
      RandomPlayerbotMgr::instance().LogoutPlayerBot(ObjectGuid(bot.guid));
    }
  }
}

}  // namespace llm_brain
