// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/bot_manager.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Action.h"
#include "AiFactory.h"
#include "ByteBuffer.h"
#include "CharacterCache.h"
#include "DBCStructure.h"
#include "Engine.h"
#include "Event.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "ServerScript.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "config/config_parsing.h"
#include "popup_watch.h"

namespace llm_brain {
namespace {

constexpr uint32_t kTickMs = 1000;
// A failed or dropped login is retried after this long.
constexpr uint32_t kLoginRetryMs = 10000;

struct BotStrategyOverride {
  std::string combat;
  std::string non_combat;
};

// Whether an LLM harness is currently connected (A-M9).
std::atomic<bool>& BrainConnectedFlag() {
  static auto* flag = new std::atomic<bool>(false);
  return *flag;
}

// Thread-safe state shared between the world thread (BotManager,
// OptionExecutor) and map threads (Playerbots' UpdateAI -> BrainPolicyListener,
// Engine destruction) so `MapUpdate.Threads > 1` is race-free (A-M10).
struct PolicySharedState {
  base::Lock lock;
  // Engines that currently own a BrainPolicyListener (keyed by uintptr_t so
  // ~BrainPolicyListener can deregister on Engine destruction).
  std::unordered_set<uintptr_t> listener_engines GUARDED_BY(lock);
  // `bot guid -> first dead ms` inside a dungeon (cleared when alive/offline).
  std::unordered_map<uint64_t, uint32_t> dungeon_dead_since_ms GUARDED_BY(lock);
  // `bot guid -> getMSTime() deadline` the brain chose to wait for rez until.
  std::unordered_map<uint64_t, uint32_t> rez_wait_until_ms GUARDED_BY(lock);
  // Per-bot strategy overrides.
  std::unordered_map<std::string, BotStrategyOverride> bot_overrides
      GUARDED_BY(lock);
  // Per-bot group policies keyed by bot guid.
  struct StoredGroupPolicy {
    uint64_t group_guid = 0;
    BotManager::GroupPolicy policy;
  };
  std::unordered_map<uint64_t, StoredGroupPolicy> group_policies
      GUARDED_BY(lock);
};

PolicySharedState& PolicyState() {
  static auto* state = new PolicySharedState();
  return *state;
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
  // ImbueWithStoneAction enchants mainhand + offhand; ImbueWithOilAction only
  // ever enchants mainhand (ImbueAction.cpp:166-175).
  const uint8_t slots[2] = {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND};
  const size_t slot_count = is_stone ? 2 : 1;
  for (size_t i = 0; i < slot_count; ++i) {
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slots[i]);
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

bool CanResurrectOthers(Player* member) {
  if (!member) {
    return false;
  }
  for (const auto& [spell_id, state] : member->GetSpellMap()) {
    if (!state || !state->Active || state->State == PLAYERSPELL_REMOVED) {
      continue;
    }
    const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
    if (!info) {
      continue;
    }
    for (uint8_t i = 0; i < MAX_SPELL_EFFECTS; ++i) {
      const uint32_t eff = info->Effects[i].Effect;
      if (eff == SPELL_EFFECT_RESURRECT || eff == SPELL_EFFECT_RESURRECT_NEW) {
        return true;
      }
    }
  }
  return false;
}

bool HasLivingResurrectorOnSameMap(Player* bot) {
  if (!bot || !bot->GetGroup()) {
    return false;
  }
  for (GroupReference* itr = bot->GetGroup()->GetFirstMember(); itr != nullptr;
       itr = itr->next()) {
    Player* member = itr->GetSource();
    if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
        member->GetMapId() == bot->GetMapId() &&
        member->GetInstanceId() == bot->GetInstanceId() &&
        CanResurrectOthers(member)) {
      return true;
    }
  }
  return false;
}

// Maximum time to suppress Playerbots' automatic `"auto release"` while waiting
// for a living resurrector in a dungeon; beyond this window the bot may release
// (or the brain can choose `Release spirit` / `Accept resurrection` at any
// time).
constexpr uint32_t kMaxWaitForRezMs = 90000;

struct PendingSharedQuestEntry {
  uint32_t quest_id = 0;
  uint32_t received_ms = 0;
};

constexpr uint32_t kPendingSharedQuestTimeoutMs = 60000;
constexpr uint32_t kPendingReadyCheckTimeoutMs = 30000;
constexpr uint32_t kPendingLfgProposalTimeoutMs = 45000;

struct PendingLfgProposalEntry {
  uint32_t proposal_id = 0;
  uint32_t received_ms = 0;
};

struct PendingPromptsState {
  base::Lock lock;
  std::unordered_map<uint64_t, PendingSharedQuestEntry> shared_quests
      GUARDED_BY(lock);
  std::unordered_map<uint64_t, uint32_t> ready_checks GUARDED_BY(lock);
  std::unordered_map<uint64_t, PendingLfgProposalEntry> lfg_proposals
      GUARDED_BY(lock);
};

PendingPromptsState& PendingPrompts() {
  static auto* state = new PendingPromptsState();
  return *state;
}

// Observes outgoing packets even on socketless (standalone) bot sessions so
// ready checks, shared quests, and LFG proposals are captured for the brain
// without relying on Playerbots' master-only packet handlers.
class LlmBrainPacketScript final : public ServerScript {
 public:
  LlmBrainPacketScript()
      : ServerScript("LlmBrainPacketScript", {SERVERHOOK_ON_PACKET_SENT}) {}

  void OnPacketSent(WorldSession* session, WorldPacket const& packet) override {
    if (!session) {
      return;
    }
    Player* bot = session->GetPlayer();
    if (!bot || !GET_PLAYERBOT_AI(bot)) {
      return;
    }
    const uint64_t key = bot->GetGUID().GetRawValue();
    switch (packet.GetOpcode()) {
      case MSG_RAID_READY_CHECK: {
        if (packet.size() >= 8) {
          ByteBuffer copy(packet);
          copy.rpos(0);
          ObjectGuid starter;
          copy >> starter;
          if (starter != bot->GetGUID()) {
            PendingPromptsState& prompts = PendingPrompts();
            base::AutoLock lock(prompts.lock);
            prompts.ready_checks[key] = getMSTime();
          }
        }
        break;
      }
      case MSG_RAID_READY_CHECK_FINISHED: {
        PendingPromptsState& prompts = PendingPrompts();
        base::AutoLock lock(prompts.lock);
        prompts.ready_checks.erase(key);
        break;
      }
      case SMSG_QUESTGIVER_QUEST_DETAILS: {
        if (packet.size() >= 20) {
          ByteBuffer copy(packet);
          copy.rpos(0);
          ObjectGuid npc_guid;
          ObjectGuid divider_guid;
          uint32_t quest_id = 0;
          copy >> npc_guid >> divider_guid >> quest_id;
          // Real quest shares pass the recipient's own GUID as `npc_guid`
          // alongside a non-empty `divider_guid` (QuestHandler.cpp).
          if (npc_guid == bot->GetGUID() && divider_guid && quest_id != 0) {
            PendingPromptsState& prompts = PendingPrompts();
            base::AutoLock lock(prompts.lock);
            prompts.shared_quests[key] = {quest_id, getMSTime()};
          }
        }
        break;
      }
      case SMSG_LFG_PROPOSAL_UPDATE: {
        if (packet.size() >= 15) {
          ByteBuffer copy(packet);
          copy.rpos(0);
          uint32_t dungeon_entry = 0;
          uint8_t state = 0;
          uint32_t proposal_id = 0;
          uint32_t encounters = 0;
          uint8_t silent = 0;
          uint8_t count = 0;
          copy >> dungeon_entry >> state >> proposal_id >> encounters >>
              silent >> count;
          bool self_unanswered = false;
          if (state == lfg::LFG_PROPOSAL_INITIATING && proposal_id != 0 &&
              silent == 0 && packet.size() >= 15u + 9u * count) {
            for (uint8_t i = 0; i < count; ++i) {
              uint32_t role = 0;
              uint8_t is_self = 0;
              uint8_t in_dungeon = 0;
              uint8_t same_group = 0;
              uint8_t answered = 0;
              uint8_t accepted = 0;
              copy >> role >> is_self >> in_dungeon >> same_group >> answered >>
                  accepted;
              if (is_self != 0) {
                self_unanswered = (answered == 0);
                break;
              }
            }
          }
          PendingPromptsState& prompts = PendingPrompts();
          base::AutoLock lock(prompts.lock);
          if (self_unanswered) {
            prompts.lfg_proposals[key] = {proposal_id, getMSTime()};
          } else {
            prompts.lfg_proposals.erase(key);
          }
        }
        break;
      }
      default:
        break;
    }
  }
};

// Enforces clean ownership boundaries between the LLM brain and Playerbots'
// reflex layer on each Engine:
//  - Blocks `"accept invitation"` so the brain decides group invites.
//  - Blocks `"equip upgrades packet action"` / `"equip upgrade"` so Playerbots'
//    auto-equipper never fights the brain's `Equip` choices.
//  - Gates `"apply stone"` / `"apply oil"` on actual AzerothCore temp-enchant
//    eligibility to stop SPELL_FAILED_LOWLEVEL 5-second spam loops.
//  - Gates `"auto release"` inside dungeons only when a living party member on
//    the same map can resurrect AND within `kMaxWaitForRezMs` (90 s), so bots
//    never lie dead forever when no healer is alive or no rez arrives.
// Owned by the Engine's ActionExecutionListeners (which `delete`s every
// listener on Engine destruction).
class BrainPolicyListener final : public ActionExecutionListener {
 public:
  BrainPolicyListener(uintptr_t engine_key, Player* bot, PlayerbotAI* ai)
      : engine_key_(engine_key), bot_(bot), ai_(ai) {
    PolicySharedState& st = PolicyState();
    base::AutoLock lock(st.lock);
    st.listener_engines.insert(engine_key_);
  }
  ~BrainPolicyListener() override {
    PolicySharedState& st = PolicyState();
    base::AutoLock lock(st.lock);
    st.listener_engines.erase(engine_key_);
  }
  bool Before(Action* /*action*/, Event /*event*/) override { return true; }
  bool AllowExecution(Action* action, Event event) override {
    if (!action) {
      return true;
    }
    const std::string& name = action->getName();
    if (name == "apply stone") {
      return HasEligibleTempEnchant(bot_, ai_, /*is_stone=*/true);
    }
    if (name == "apply oil") {
      return HasEligibleTempEnchant(bot_, ai_, /*is_stone=*/false);
    }
    // When no LLM harness is connected, let Playerbots answer invites, loot
    // rolls, ready checks, quest shares, and LFG popups so human groupmates do
    // not stall waiting for timeouts (A-M9).
    const bool brain_connected = BotManager::IsBrainConnected();
    if (brain_connected &&
        (name == "accept invitation" || name == "equip upgrade" ||
         name == "equip upgrades packet action" || name == "loot roll" ||
         name == "roll" || name == "accept quest share" ||
         name == "ready check" || name == "lfg join" || name == "lfg accept" ||
         name == "lfg role check" || name == "lfg leave" ||
         name == "lfg teleport")) {
      return false;
    }
    if (name == "auto release" && bot_ && !bot_->IsAlive()) {
      const uint64_t key = bot_->GetGUID().GetRawValue();
      PolicySharedState& st = PolicyState();
      base::AutoLock lock(st.lock);
      if (auto it = st.rez_wait_until_ms.find(key);
          it != st.rez_wait_until_ms.end()) {
        // The brain chose to wait (signed difference: getMSTime wraps).
        if (brain_connected &&
            static_cast<int32_t>(it->second - getMSTime()) > 0) {
          return false;
        }
        st.rez_wait_until_ms.erase(it);
      }
    }
    if (name == "auto release" && bot_ && bot_->GetMap() &&
        bot_->GetMap()->IsDungeon()) {
      const uint64_t key = bot_->GetGUID().GetRawValue();
      PolicySharedState& st = PolicyState();
      if (bot_->IsAlive()) {
        base::AutoLock lock(st.lock);
        st.dungeon_dead_since_ms.erase(key);
        return true;
      }
      uint32_t dead_since = 0;
      {
        base::AutoLock lock(st.lock);
        auto [it, inserted] =
            st.dungeon_dead_since_ms.try_emplace(key, getMSTime());
        dead_since = it->second;
      }
      if (GetMSTimeDiffToNow(dead_since) < kMaxWaitForRezMs &&
          HasLivingResurrectorOnSameMap(bot_)) {
        return false;
      }
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
  // Non-owning raw pointers (CONTRIBUTING.md: upstream AzerothCore `Player` and
  // `PlayerbotAI` do not inherit `base::RawPtrTarget`; lifetime is strictly
  // bounded because `Player` owns `PlayerbotAI` which owns `Engine` which owns
  // this listener).
  Player* bot_;
  PlayerbotAI* ai_;
};

void EnsurePolicyListener(Player* player, PlayerbotAI* ai) {
  if (!player || !ai) {
    return;
  }
  if (Engine* engine = ai->GetCurrentEngine()) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(engine);
    {
      PolicySharedState& st = PolicyState();
      base::AutoLock lock(st.lock);
      if (st.listener_engines.contains(key)) {
        return;
      }
    }
    engine->AddActionExecutionListener(
        new BrainPolicyListener(key, player, ai));
  }
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
  // Attach BrainPolicyListener on every world tick (O(1) hash lookup) so newly
  // entered Engines (especially BOT_STATE_DEAD on first dungeon death) are
  // protected before the 1 s Tick() runs.
  for (const Bot& bot : bots_) {
    if (!bot.guid) {
      continue;
    }
    Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(bot.guid));
    if (player && player->IsInWorld() && !player->InBattleground()) {
      EnsurePolicyListener(player, GET_PLAYERBOT_AI(player));
    }
  }
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
        if (player->IsAlive() || !player->GetMap() ||
            !player->GetMap()->IsDungeon()) {
          PolicySharedState& st = PolicyState();
          base::AutoLock lock(st.lock);
          st.dungeon_dead_since_ms.erase(bot.guid);
        }
        if (player->IsAlive()) {
          PolicySharedState& st = PolicyState();
          base::AutoLock lock(st.lock);
          st.rez_wait_until_ms.erase(bot.guid);
        }
        EnsureStrategies(player);
      }
      bot.was_online = true;
      continue;
    }
    if (bot.was_online) {
      // Just dropped (logged out, kicked): wait like after a failed login
      // instead of logging it straight back in on the next tick.
      bot.was_online = false;
      bot.next_login_ms = now_ms_ + kLoginRetryMs;
    }
    {
      PolicySharedState& st = PolicyState();
      base::AutoLock lock(st.lock);
      st.dungeon_dead_since_ms.erase(bot.guid);
      st.rez_wait_until_ms.erase(bot.guid);
    }
    {
      PendingPromptsState& prompts = PendingPrompts();
      base::AutoLock lock(prompts.lock);
      prompts.shared_quests.erase(bot.guid);
      prompts.ready_checks.erase(bot.guid);
      prompts.lfg_proposals.erase(bot.guid);
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
  {
    PolicySharedState& st = PolicyState();
    base::AutoLock lock(st.lock);
    if (auto it = st.bot_overrides.find(player->GetName());
        it != st.bot_overrides.end()) {
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
  }
  // The brain's own choice for this group comes last, so it wins (RH-M1/M2).
  const GroupPolicy policy = GetGroupPolicy(player);  // Empty outside it.
  if (policy.not_tank) {
    combat += ",-tank assist,-pull back,+dps assist";
  }
  if (policy.stay) {
    non_combat += ",-follow,+stay";
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

void BotManager::SetBrainConnected(bool connected) {
  BrainConnectedFlag().store(connected, std::memory_order_release);
}

bool BotManager::IsBrainConnected() {
  return BrainConnectedFlag().load(std::memory_order_acquire);
}

void BotManager::SetBotStrategyOverride(const std::string& bot_name,
                                        std::string combat_extra,
                                        std::string non_combat_extra) {
  PolicySharedState& st = PolicyState();
  base::AutoLock lock(st.lock);
  st.bot_overrides[bot_name] =
      BotStrategyOverride{std::move(combat_extra), std::move(non_combat_extra)};
}

BotManager::GroupPolicy BotManager::GetGroupPolicy(Player* bot) {
  if (!bot) {
    return {};
  }
  const Group* group = bot->GetGroup();
  const uint64_t key = bot->GetGUID().GetRawValue();
  PolicySharedState& st = PolicyState();
  base::AutoLock lock(st.lock);
  auto it = st.group_policies.find(key);
  if (it == st.group_policies.end()) {
    return {};
  }
  if (!group || group->GetGUID().GetRawValue() != it->second.group_guid) {
    st.group_policies.erase(it);  // Chosen for a group it is no longer in.
    return {};
  }
  return it->second.policy;
}

void BotManager::SetGroupPolicy(Player* bot, GroupPolicy policy) {
  if (!bot) {
    return;
  }
  const Group* group = bot->GetGroup();
  const uint64_t key = bot->GetGUID().GetRawValue();
  PolicySharedState& st = PolicyState();
  base::AutoLock lock(st.lock);
  if (!group || policy == GroupPolicy{}) {
    st.group_policies.erase(key);
  } else {
    st.group_policies[key] = {group->GetGUID().GetRawValue(), policy};
  }
}

void BotManager::ClearBotStrategyOverride(const std::string& bot_name) {
  PolicySharedState& st = PolicyState();
  base::AutoLock lock(st.lock);
  st.bot_overrides.erase(bot_name);
}

void BotManager::ClearBotStrategyOverrides() {
  PolicySharedState& st = PolicyState();
  base::AutoLock lock(st.lock);
  st.bot_overrides.clear();
}

uint32_t BotManager::GetPendingSharedQuest(Player* bot) {
  if (!bot) {
    return 0;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  PendingPromptsState& prompts = PendingPrompts();
  if (!bot->GetDivider()) {
    base::AutoLock lock(prompts.lock);
    prompts.shared_quests.erase(key);
    return 0;
  }
  bool timed_out = false;
  uint32_t quest_id = 0;
  {
    base::AutoLock lock(prompts.lock);
    auto it = prompts.shared_quests.find(key);
    if (it == prompts.shared_quests.end()) {
      return 0;
    }
    if (GetMSTimeDiffToNow(it->second.received_ms) >=
        kPendingSharedQuestTimeoutMs) {
      prompts.shared_quests.erase(it);
      timed_out = true;
    } else {
      quest_id = it->second.quest_id;
    }
  }
  if (timed_out) {
    bot->SetDivider(ObjectGuid::Empty);
    return 0;
  }
  // An auto-accept quest is already in the log when its details popup comes
  // (QuestHandler.cpp HandlePushQuestToParty): nothing left to answer.
  if (bot->GetQuestStatus(quest_id) != QUEST_STATUS_NONE) {
    ClearPendingSharedQuest(bot);
    bot->SetDivider(ObjectGuid::Empty);
    return 0;
  }
  return quest_id;
}

void BotManager::ClearPendingSharedQuest(Player* bot) {
  if (!bot) {
    return;
  }
  PendingPromptsState& prompts = PendingPrompts();
  base::AutoLock lock(prompts.lock);
  prompts.shared_quests.erase(bot->GetGUID().GetRawValue());
}

bool BotManager::HasPendingReadyCheck(Player* bot) {
  if (!bot) {
    return false;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  PendingPromptsState& prompts = PendingPrompts();
  base::AutoLock lock(prompts.lock);
  if (!bot->GetGroup()) {
    prompts.ready_checks.erase(key);
    return false;
  }
  auto it = prompts.ready_checks.find(key);
  if (it == prompts.ready_checks.end()) {
    return false;
  }
  if (GetMSTimeDiffToNow(it->second) >= kPendingReadyCheckTimeoutMs) {
    prompts.ready_checks.erase(it);
    return false;
  }
  return true;
}

void BotManager::ClearPendingReadyCheck(Player* bot) {
  if (!bot) {
    return;
  }
  PendingPromptsState& prompts = PendingPrompts();
  base::AutoLock lock(prompts.lock);
  prompts.ready_checks.erase(bot->GetGUID().GetRawValue());
}

uint32_t BotManager::GetPendingLfgProposal(Player* bot) {
  if (!bot) {
    return 0;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  PendingPromptsState& prompts = PendingPrompts();
  base::AutoLock lock(prompts.lock);
  auto it = prompts.lfg_proposals.find(key);
  if (it == prompts.lfg_proposals.end()) {
    return 0;
  }
  if (GetMSTimeDiffToNow(it->second.received_ms) >=
      kPendingLfgProposalTimeoutMs) {
    prompts.lfg_proposals.erase(it);
    return 0;
  }
  return it->second.proposal_id;
}

void BotManager::ClearPendingLfgProposal(Player* bot) {
  if (!bot) {
    return;
  }
  PendingPromptsState& prompts = PendingPrompts();
  base::AutoLock lock(prompts.lock);
  prompts.lfg_proposals.erase(bot->GetGUID().GetRawValue());
}

PopupState BotManager::ReadPopupState(Player* bot) {
  PopupState s;
  if (!bot || !bot->IsInWorld()) {
    return s;
  }
  s.ready_check = HasPendingReadyCheck(bot);
  const lfg::LfgState lfg_state = sLFGMgr->GetState(bot->GetGUID());
  s.role_check = lfg_state == lfg::LFG_STATE_ROLECHECK;
  if (lfg_state == lfg::LFG_STATE_PROPOSAL) {
    s.lfg_proposal_id = GetPendingLfgProposal(bot);
  }
  if (bot->IsAlive()) {
    s.shared_quest_id = GetPendingSharedQuest(bot);
    if (s.shared_quest_id != 0) {
      if (const Quest* q = sObjectMgr->GetQuestTemplate(s.shared_quest_id)) {
        s.shared_quest_title = q->GetTitle();
      }
      if (Player* from =
              ObjectAccessor::FindConnectedPlayer(bot->GetDivider())) {
        s.shared_quest_from = from->GetName();
      }
    }
  } else {
    s.resurrect_offer = bot->isResurrectRequested();
  }
  if (Group* group = bot->GetGroup()) {
    for (const Roll* roll : group->GetRolls()) {
      if (!roll) {
        continue;
      }
      auto it = roll->playerVote.find(bot->GetGUID());
      if (it == roll->playerVote.end() || it->second != NOT_EMITED_YET) {
        continue;
      }
      PopupState::Roll r;
      r.item_guid = roll->itemGUID.GetRawValue();
      r.item_slot = roll->itemSlot;
      r.item_id = roll->itemid;
      if (const ItemTemplate* proto =
              sObjectMgr->GetItemTemplate(roll->itemid)) {
        r.item_name = proto->Name1;
      }
      s.rolls.push_back(std::move(r));
    }
  }
  return s;
}

void BotManager::WaitForResurrection(Player* bot, uint32_t wait_ms) {
  if (bot) {
    PolicySharedState& st = PolicyState();
    base::AutoLock lock(st.lock);
    st.rez_wait_until_ms[bot->GetGUID().GetRawValue()] = getMSTime() + wait_ms;
  }
}

void BotManager::Stop() {
  SetBrainConnected(false);
  {
    PolicySharedState& st = PolicyState();
    base::AutoLock lock(st.lock);
    st.bot_overrides.clear();
    st.group_policies.clear();
    st.rez_wait_until_ms.clear();
    st.dungeon_dead_since_ms.clear();
  }
  {
    PendingPromptsState& prompts = PendingPrompts();
    base::AutoLock lock(prompts.lock);
    prompts.shared_quests.clear();
    prompts.ready_checks.clear();
    prompts.lfg_proposals.clear();
  }
  for (const Bot& bot : bots_) {
    if (bot.guid) {
      RandomPlayerbotMgr::instance().LogoutPlayerBot(ObjectGuid(bot.guid));
    }
  }
}

void AddLlmBrainBotManagerScripts() {
  new LlmBrainPacketScript();
}

}  // namespace llm_brain
