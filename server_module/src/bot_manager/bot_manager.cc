// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/bot_manager.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Action.h"
#include "AiFactory.h"
#include "AiObjectContext.h"
#include "ByteBuffer.h"
#include "Cell.h"
#include "CellImpl.h"  // IWYU pragma: keep
#include "CharacterCache.h"
#include "CharmInfo.h"
#include "DBCStructure.h"
#include "Define.h"
#include "Engine.h"
#include "Event.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"  // IWYU pragma: keep
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
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
#include "Unit.h"
#include "UpdateFields.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "bot_manager/playerbots_actions.h"
#include "bot_manager/tick_owner.h"
#include "bot_session.h"
#include "chat/chat_scripts.h"
#include "config/config_parsing.h"
#include "options/act_geometry.h"
#include "options/exec_shared.h"
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

// Global (non-per-GUID) state shared between world and map threads.
struct PolicyGlobalState {
  base::Lock lock;
  // Engines that currently own a BrainPolicyListener (keyed by uintptr_t so
  // ~BrainPolicyListener can deregister on Engine destruction).
  std::unordered_set<uintptr_t> listener_engines GUARDED_BY(lock);
  // Per-bot strategy overrides keyed by name.
  std::unordered_map<std::string, BotStrategyOverride> bot_overrides
      GUARDED_BY(lock);
};

PolicyGlobalState& PolicyState() {
  static auto* state = new PolicyGlobalState();
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

// Outdoors, a resurrector farther than this from the body won't come for it.
constexpr float kNearbyResurrectorYd = 100.0f;

// A living group member who can resurrect is in the same instance, or nearby
// on an open-world map (T12: not only in dungeons).
bool HasLivingResurrectorNearby(Player* bot) {
  if (!bot || !bot->GetGroup()) {
    return false;
  }
  const bool in_dungeon = bot->GetMap() && bot->GetMap()->IsDungeon();
  for (GroupReference* itr = bot->GetGroup()->GetFirstMember(); itr != nullptr;
       itr = itr->next()) {
    Player* member = itr->GetSource();
    if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
        member->GetMapId() == bot->GetMapId() &&
        member->GetInstanceId() == bot->GetInstanceId() &&
        (in_dungeon || bot->GetDistance(member) <= kNearbyResurrectorYd) &&
        CanResurrectOthers(member)) {
      return true;
    }
  }
  return false;
}

// Whether a human plays in `group` (a bot leading it must not wander off).
bool GroupHasHuman(Group* group) {
  if (!group) {
    return false;
  }
  for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
       itr = itr->next()) {
    Player* member = itr->GetSource();
    if (member && IsHumanPlayer(member)) {
      return true;
    }
  }
  return false;
}

// A hostile within 30 yd is held by crowd control that damage would break
// (Polymorph, Sap, Hex, ...): area damage would free it (Now #4).
bool CrowdControlledMobNearby(Player* bot) {
  std::list<Unit*> nearby;
  Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, 30.0f);
  Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(
      bot, nearby, check);
  Cell::VisitObjects(bot, searcher, 30.0f);
  for (Unit* u : nearby) {
    if (u && u->IsAlive() && u->HasBreakableByDamageCrowdControlAura()) {
      return true;
    }
  }
  return false;
}

// Grouped bots play like playerbots' grouped defaults (AiFactory gives
// `boost` / `-threat` to solo bots only): they watch threat and don't open
// with cooldowns over someone else's tank (T3).
std::string GroupedCombatStrategies(const std::string& combat) {
  std::string out;
  for (const StrategyCheck& check : StrategyChecks(combat)) {
    bool present = check.present;
    if (check.name == "threat") {
      present = true;
    } else if (check.name == "boost") {
      present = false;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += (present ? "+" : "-") + check.name;
  }
  return out;
}

// Maximum time to suppress Playerbots' automatic `"auto release"` while waiting
// for a living resurrector nearby (any map); beyond this window the bot may
// release (or the brain can choose `Release spirit` / `Accept resurrection` at
// any time).
constexpr uint32_t kMaxWaitForRezMs = 90000;

constexpr uint32_t kPendingSharedQuestTimeoutMs = 60000;
constexpr uint32_t kPendingReadyCheckTimeoutMs = 30000;
constexpr uint32_t kPendingLfgProposalTimeoutMs = 45000;

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
            BotSessionRegistry::Instance().WithSession(key, [](BotSession& s) {
              s.pending_ready_check_ms = getMSTime();
            });
          }
        }
        break;
      }
      case MSG_RAID_READY_CHECK_FINISHED: {
        BotSessionRegistry::Instance().WithSession(
            key, [](BotSession& s) { s.pending_ready_check_ms.reset(); });
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
            BotSessionRegistry::Instance().WithSession(
                key, [quest_id](BotSession& s) {
                  s.pending_shared_quest_id = quest_id;
                  s.pending_shared_quest_ms = getMSTime();
                  s.pending_shared_quest_confirm = false;
                });
          }
        }
        break;
      }
      case SMSG_QUEST_CONFIRM_ACCEPT: {
        // Sent when a nearby party member starts a `QUEST_FLAGS_PARTY_ACCEPT`
        // escort quest (`QuestConfirmAccept::Write`: uint32 QuestId, C-string
        // QuestTitle, ObjectGuid PlayerGuid; `bot->GetDivider()` is the
        // starter).
        if (packet.size() >= 4 + 1 + 8) {
          ByteBuffer copy(packet);
          copy.rpos(0);
          uint32_t quest_id = 0;
          std::string title;
          ObjectGuid starter_guid;
          copy >> quest_id >> title;
          if (copy.rpos() + 8 <= copy.size()) {
            copy >> starter_guid;
          }
          if (quest_id != 0 && starter_guid && starter_guid != bot->GetGUID()) {
            BotSessionRegistry::Instance().WithSession(
                key, [quest_id](BotSession& s) {
                  s.pending_shared_quest_id = quest_id;
                  s.pending_shared_quest_ms = getMSTime();
                  s.pending_shared_quest_confirm = true;
                });
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
          BotSessionRegistry::Instance().WithSession(
              key, [self_unanswered, proposal_id](BotSession& s) {
                if (self_unanswered) {
                  s.pending_lfg_proposal_id = proposal_id;
                  s.pending_lfg_proposal_ms = getMSTime();
                } else {
                  s.pending_lfg_proposal_id = 0;
                  s.pending_lfg_proposal_ms = 0;
                }
              });
        }
        break;
      }
      case MSG_MINIMAP_PING: {
        // `Group::DoMinimapPing` (MinimapPing::Write: ObjectGuid SourceGuid,
        // float MapX, float MapY in world coordinates), broadcast to the
        // pinger's group but not the pinger; handled on the world thread
        // (PROCESS_THREADUNSAFE). The client shows it on the minimap.
        if (packet.size() >= 8 + 4 + 4) {
          ByteBuffer copy(packet);
          copy.rpos(0);
          ObjectGuid source;
          float x = 0.0f;
          float y = 0.0f;
          copy >> source >> x >> y;
          Group* group = bot->GetGroup();
          Player* from = ObjectAccessor::FindConnectedPlayer(source);
          if (source == bot->GetGUID() || !group || !group->IsMember(source) ||
              !from || !std::isfinite(x) || !std::isfinite(y)) {
            break;
          }
          BotSession::PartyPing ping{
              .from_guid = source.GetRawValue(),
              .from = from->GetName(),
              .x = x,
              .y = y,
              .map_id = from->GetMapId(),
              .instance_id = from->GetInstanceId(),
              .ms = getMSTime(),
          };
          BotSessionRegistry::Instance().WithSession(
              key, [&ping](BotSession& s) { s.party_ping = std::move(ping); });
        }
        break;
      }
      case SMSG_SUMMON_REQUEST: {
        if (packet.size() >= 8) {
          ByteBuffer copy(packet);
          copy.rpos(0);
          ObjectGuid summoner;
          copy >> summoner;
          std::string name;
          if (Player* from = ObjectAccessor::FindConnectedPlayer(summoner)) {
            name = from->GetName();
          }
          BotSessionRegistry::Instance().WithSession(
              key, [&name](BotSession& s) { s.pending_summoner = name; });
        }
        break;
      }
      case SMSG_LFG_BOOT_PROPOSAL_UPDATE: {
        if (packet.size() >= 3 + 8 + 16) {
          ByteBuffer copy(packet);
          copy.rpos(0);
          uint8_t in_progress = 0;
          uint8_t did_vote = 0;
          uint8_t agree = 0;
          ObjectGuid victim;
          uint32_t votes = 0;
          uint32_t agree_num = 0;
          uint32_t secs_left = 0;
          uint32_t needed = 0;
          std::string reason;
          copy >> in_progress >> did_vote >> agree >> victim >> votes >>
              agree_num >> secs_left >> needed;
          if (copy.rpos() < copy.size()) {
            copy >> reason;
          }
          std::string name;
          if (in_progress && !did_vote && victim != bot->GetGUID()) {
            if (Player* p = ObjectAccessor::FindConnectedPlayer(victim)) {
              name = p->GetName();
            } else {
              name = "a group member";
            }
          }
          BotSessionRegistry::Instance().WithSession(
              key, [&name, &reason, secs_left](BotSession& s) {
                s.pending_boot_victim = name;
                s.pending_boot_reason = name.empty() ? "" : reason;
                s.pending_boot_until_ms =
                    name.empty() ? 0
                                 : getMSTime() + std::max(secs_left, 5u) * 1000;
              });
        }
        break;
      }
      default:
        break;
    }
  }
};

// Enforces the ownership boundary between the LLM brain and Playerbots'
// reflex layer on each Engine, by the bot's mode (BotSession::tick_owner)
// and the one table of gated actions (tick_owner.h, checked against
// playerbots' action factories at startup):
//  - kBrainLed: popups (invites, loot rolls, ready checks, quest shares, LFG,
//    self resurrect) are the brain's while it leads, playerbots' otherwise.
//  - kAlways: `"equip upgrade(s packet action)"` never run, so Playerbots'
//    auto-equipper never fights the brain's `Equip` choices.
//  - kCondition: `"flee"` is blocked in a dungeon or a human's group
//    (GroupReflexLimits); `"stay"` yields to a brain act's forced walk;
//    `"apply stone"` / `"apply oil"` need real temp-enchant eligibility
//    (no SPELL_FAILED_LOWLEVEL loops); `"auto release"` waits while a living
//    party member nearby can resurrect, within `kMaxWaitForRezMs` (90 s).
//  - Spells that would root the bot (a cast time, a rooting channel) wait
//    while a brain dodge moves it out (BotSession::moving_out_until_ms).
// Owned by the Engine's ActionExecutionListeners (which `delete`s every
// listener on Engine destruction).
class BrainPolicyListener final : public ActionExecutionListener {
 public:
  BrainPolicyListener(uintptr_t engine_key, Player* bot, PlayerbotAI* ai)
      : engine_key_(engine_key), bot_(bot), ai_(ai) {
    PolicyGlobalState& st = PolicyState();
    base::AutoLock lock(st.lock);
    st.listener_engines.insert(engine_key_);
  }
  ~BrainPolicyListener() override {
    PolicyGlobalState& st = PolicyState();
    base::AutoLock lock(st.lock);
    st.listener_engines.erase(engine_key_);
  }
  bool Before(Action* /*action*/, Event /*event*/) override { return true; }
  bool AllowExecution(Action* action, Event event) override {
    if (!action) {
      return true;
    }
    const std::string& name = action->getName();
    // While a dodge moves the bot out (BotSession::moving_out_until_ms), a
    // reflex spell that would hold it still (a cast time, or a rooting
    // channel) waits until it is out; instants still go out on the move.
    if (bot_ && ai_) {
      if (auto* cast = dynamic_cast<CastSpellAction*>(action);
          cast && HoldsDodgedBotStill(cast->getSpell())) {
        return false;
      }
    }
    const GatedAction* gated = FindGatedAction(name);
    if (!gated) {
      return true;
    }
    if (gated->gate == ActionGate::kAlways) {
      return false;
    }
    if (gated->gate == ActionGate::kBrainLed) {
      return !BlockedFor(*gated, Owner());
    }
    if (name == "apply stone") {
      return HasEligibleTempEnchant(bot_, ai_, /*is_stone=*/true);
    }
    if (name == "apply oil") {
      return HasEligibleTempEnchant(bot_, ai_, /*is_stone=*/false);
    }
    // A brain act walking the bot (MOVEMENT_FORCED, act_executor.cc) outranks
    // `stay`, which would stop it at every step, and its walk back (`to
    // ping`, or a `use` walking into range, while told to wait).
    if ((name == "stay" || name == "return to stay position") && ai_) {
      if (auto* last = ai_->GetAiObjectContext()->GetValue<LastMovement&>(
              "last movement")) {
        const LastMovement& lm = last->Get();
        if (lm.priority == MovementPriority::MOVEMENT_FORCED &&
            static_cast<float>(GetMSTimeDiffToNow(lm.msTime)) <
                lm.lastdelayTime) {
          return false;
        }
      }
      return true;
    }
    // No reflex step back into unpulled packs in a dungeon or a human's
    // group (GroupReflexLimits); solo bots keep it.
    if (name == "flee") {
      Group* group = bot_ ? bot_->GetGroup() : nullptr;
      return !bot_ || !GroupReflexLimitsFor(
                           group != nullptr,
                           bot_->GetMap() && bot_->GetMap()->IsDungeon(),
                           GroupHasHuman(group))
                           .block_flee;
    }
    if (name == "auto release") {
      return AllowAutoRelease();
    }
    return true;
  }

  // An explicit `rez:wait` from the brain holds the release while the bot is
  // brain-led. Otherwise it waits (up to kMaxWaitForRezMs) while a living
  // party member nearby can resurrect, or a soulstone or ankh is up (Now
  // #6), so bots never lie dead forever when no rez comes.
  bool AllowAutoRelease() {
    if (!bot_) {
      return true;
    }
    const uint64_t key = bot_->GetGUID().GetRawValue();
    if (bot_->IsAlive()) {
      if (bot_->GetMap()) {
        BotSessionRegistry::Instance().WithSession(
            key, [](BotSession& s) { s.dead_since_ms.reset(); });
      }
      return true;
    }
    const bool wait =
        BotSessionRegistry::Instance().WithSession(key, [](BotSession& s) {
          if (!s.rez_wait_until_ms) {
            return false;
          }
          if (s.tick_owner == TickOwner::kBrain &&
              static_cast<int32_t>(*s.rez_wait_until_ms - getMSTime()) > 0) {
            return true;
          }
          s.rez_wait_until_ms.reset();
          return false;
        });
    if (wait) {
      return false;
    }
    if (!bot_->GetMap()) {
      return true;
    }
    const uint32_t dead_since =
        BotSessionRegistry::Instance().WithSession(key, [](BotSession& s) {
          if (!s.dead_since_ms) {
            s.dead_since_ms = getMSTime();
          }
          return *s.dead_since_ms;
        });
    return GetMSTimeDiffToNow(dead_since) >= kMaxWaitForRezMs ||
           (!HasLivingResurrectorNearby(bot_) &&
            bot_->GetUInt32Value(PLAYER_SELF_RES_SPELL) == 0);
  }

  // This bot's mode (BotSession::tick_owner, set by BotManager::Update).
  TickOwner Owner() const {
    if (!bot_) {
      return TickOwner::kPlayerbots;
    }
    return BotSessionRegistry::Instance().InspectSession(
        bot_->GetGUID().GetRawValue(), [](const BotSession* s) {
          return s ? s->tick_owner : TickOwner::kPlayerbots;
        });
  }
  void After(Action* /*action*/, bool /*executed*/, Event /*event*/) override {}

  bool HoldsDodgedBotStill(const std::string& spell) {
    const uint32_t until = BotSessionRegistry::Instance().InspectSession(
        bot_->GetGUID().GetRawValue(),
        [](const BotSession* s) { return s ? s->moving_out_until_ms : 0u; });
    if (until == 0 || static_cast<int32_t>(until - getMSTime()) <= 0) {
      return false;
    }
    const SpellInfo* info = sSpellMgr->GetSpellInfo(
        ai_->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get());
    return info &&
           CastHoldsMoveOut(info->CalcCastTime(bot_), info->IsChanneled(),
                            info->IsActionAllowedChannel());
  }
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
      PolicyGlobalState& st = PolicyState();
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
  // A gated name playerbots renamed would silently stop being gated.
  const std::vector<std::string> missing =
      MissingActionNames(GatedActions(), PlayerbotsActionNames());
  for (const std::string& name : missing) {
    LOG_ERROR("module",
              "mod-llm-brain: gated action \"{}\" is not a playerbots "
              "action (renamed upstream?): its gate does nothing",
              name);
  }
  LOG_INFO("module",
           "mod-llm-brain: {} of {} gated action names found in playerbots",
           GatedActions().size() - missing.size(), GatedActions().size());
}

BotManager::~BotManager() = default;

void BotManager::Update(uint32_t diff_ms) {
  now_ms_ += diff_ms;
  // Set each bot's mode and attach BrainPolicyListener on every world tick
  // (O(1) hash lookup) so newly entered Engines (especially BOT_STATE_DEAD
  // on first dungeon death) are protected before the 1 s Tick() runs.
  const bool connected = IsBrainConnected();
  for (const Bot& bot : bots_) {
    if (!bot.guid) {
      continue;
    }
    Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(bot.guid));
    if (!player || !player->IsInWorld()) {
      continue;
    }
    const TickOwner owner = TickOwnerFor(connected, player->InBattleground());
    const bool changed = BotSessionRegistry::Instance().WithSession(
        bot.guid, [owner](BotSession& s) {
          return std::exchange(s.tick_owner, owner) != owner;
        });
    if (changed) {
      LOG_INFO("module", "mod-llm-brain: {} is {}", player->GetName(),
               TickOwnerName(owner));
    }
    if (!player->InBattleground()) {
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
        if (player->IsAlive()) {
          BotSessionRegistry::Instance().WithSession(
              bot.guid, [](BotSession& s) {
                s.dead_since_ms.reset();
                s.rez_wait_until_ms.reset();
              });
        }
        EnsureStrategies(player);
        KeepNearHumanLeader(player);
        LeaveAbandonedHumanGroup(player);
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
    BotSessionRegistry::Instance().ResetOnLogout(bot.guid);
    if (now_ms_ >= bot.next_login_ms) {
      // No master: playerbots runs it as a standalone bot. With autologin off,
      // its random-bot manager does nothing else with it.
      LOG_INFO("module", "mod-llm-brain: logging in {}", bot.name);
      RandomPlayerbotMgr::instance().AddPlayerBot(guid, 0);
      bot.next_login_ms = now_ms_ + kLoginRetryMs;
    }
  }
}

void BotManager::KeepNearHumanLeader(Player* player) {
  // Outdoors, a follower more than 40 yd from the human leading its group
  // comes back on its own, without an LLM call (Now #8). `Wait here` (stay),
  // flights, loss of control, and fights keep it where it is.
  constexpr float kStrayYd = 40.0f;
  constexpr uint32_t kRecallCooldownMs = 4000;
  Group* group = player->GetGroup();
  PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
  if (!group || !ai || !player->IsAlive() || player->IsInCombat() ||
      group->IsLeader(player->GetGUID()) || !player->GetMap() ||
      player->GetMap()->IsDungeon() || GetGroupPolicy(player).stay ||
      PingStayHolds(player) || !CanClearBotMovement(player)) {
    return;
  }
  Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
  if (!leader || !leader->IsInWorld() || !IsHumanPlayer(leader) ||
      leader->IsInFlight() || leader->GetMapId() != player->GetMapId() ||
      leader->GetInstanceId() != player->GetInstanceId() ||
      player->GetDistance(leader) <= kStrayYd) {
    return;
  }
  const uint64_t key = player->GetGUID().GetRawValue();
  const bool moving = player->isMoving();
  const bool throttled =
      BotSessionRegistry::Instance().WithSession(key, [moving](BotSession& s) {
        if (moving && s.stray_recalled_ms != 0 &&
            GetMSTimeDiffToNow(s.stray_recalled_ms) < kRecallCooldownMs) {
          return true;
        }
        s.stray_recalled_ms = getMSTime();
        return false;
      });
  if (throttled) {
    return;
  }
  ClearLastMovement(player);
  if (!ai->DoSpecificAction("follow", Event(), /*silent=*/true) &&
      !player->isMoving()) {
    WalkTowards(player, leader->GetPositionX(), leader->GetPositionY(),
                leader->GetPositionZ());
  }
  LOG_INFO("module", "mod-llm-brain: {} strayed {:.0f} yd from {}; following",
           player->GetName(), player->GetDistance(leader), leader->GetName());
}

bool BotManager::IsKnownBotGuid(uint64_t raw_guid) const {
  for (const Bot& b : bots_) {
    if (b.guid == raw_guid) {
      return true;
    }
  }
  return sRandomPlayerbotMgr.IsRandomBot(ObjectGuid(raw_guid).GetCounter());
}

void BotManager::LeaveAbandonedHumanGroup(Player* player) {
  // When a group formed with a human loses its human (they leave the party, or
  // stay offline >= 60 s), the bots leave Dungeon Finder, teleport out of any
  // dungeon once out of combat, and disband the leftover bot group (Now #9 /
  // T11).
  constexpr uint32_t kOfflineGraceMs = 60000;
  const uint64_t key = player->GetGUID().GetRawValue();
  Group* group = player->GetGroup();
  if (!group) {
    const bool had_human =
        BotSessionRegistry::Instance().WithSession(key, [](BotSession& s) {
          const bool had = s.human_group_guid != 0;
          if (!had) {
            s.human_offline_since_ms.reset();
          }
          return had;
        });
    if (!had_human || player->IsInCombat()) {
      return;
    }
    BotSessionRegistry::Instance().WithSession(key, [](BotSession& s) {
      s.human_group_guid = 0;
      s.human_offline_since_ms.reset();
    });
    sLFGMgr->LeaveLfg(player->GetGUID());
    ClearPendingLfgProposal(player);
    if (player->GetMap() && player->GetMap()->IsDungeon() &&
        !player->IsBeingTeleportedFar()) {
      sLFGMgr->TeleportPlayer(player, true);
      if (!player->IsBeingTeleportedFar() && player->GetMap() &&
          player->GetMap()->IsDungeon()) {
        player->TeleportToEntryPoint();
      }
      LOG_INFO("module",
               "mod-llm-brain: {} left group and teleported out after human "
               "left",
               player->GetName());
    }
    return;
  }

  const uint64_t gguid = group->GetGUID().GetRawValue();
  if (GroupHasHuman(group)) {
    BotSessionRegistry::Instance().WithSession(key, [gguid](BotSession& s) {
      s.human_group_guid = gguid;
      s.human_offline_since_ms.reset();
    });
    return;
  }

  bool offline_human_slot = false;
  for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
    if (!ObjectAccessor::FindConnectedPlayer(slot.guid) &&
        !IsKnownBotGuid(slot.guid.GetRawValue())) {
      offline_human_slot = true;
      break;
    }
  }
  const bool abandoned = BotSessionRegistry::Instance().WithSession(
      key, [gguid, offline_human_slot](BotSession& s) {
        if (s.human_group_guid != gguid) {
          s.human_group_guid = 0;
          s.human_offline_since_ms.reset();
          return false;
        }
        if (!offline_human_slot) {
          return true;
        }
        if (!s.human_offline_since_ms) {
          s.human_offline_since_ms = getMSTime();
          return false;
        }
        return GetMSTimeDiffToNow(*s.human_offline_since_ms) >= kOfflineGraceMs;
      });
  if (!abandoned || player->IsInCombat()) {
    return;
  }
  BotSessionRegistry::Instance().WithSession(key, [](BotSession& s) {
    s.human_group_guid = 0;
    s.human_offline_since_ms.reset();
  });
  const bool was_in_dungeon = player->GetMap() && player->GetMap()->IsDungeon();
  sLFGMgr->LeaveLfg(player->GetGUID());
  ClearPendingLfgProposal(player);
  if (was_in_dungeon && !player->IsBeingTeleportedFar()) {
    sLFGMgr->TeleportPlayer(player, true);
  }
  if (WorldSession* sess = player->GetSession(); sess && player->GetGroup()) {
    WorldPacket pkt(CMSG_GROUP_DISBAND, 0);
    sess->HandleGroupDisbandOpcode(pkt);
  }
  if (was_in_dungeon && !player->IsBeingTeleportedFar() && player->GetMap() &&
      player->GetMap()->IsDungeon()) {
    player->TeleportToEntryPoint();
  }
  LOG_INFO("module", "mod-llm-brain: {} left group{} after human left",
           player->GetName(), was_in_dungeon ? " and teleported out" : "");
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
  Group* group = player->GetGroup();
  const bool in_group = group != nullptr;
  if (in_group) {
    base_combat = GroupedCombatStrategies(base_combat);
  }
  std::string combat = base_combat + extras;
  const bool is_leader =
      in_group && group->GetLeaderGUID() == player->GetGUID();
  const bool in_dungeon =
      player->GetMap() != nullptr && player->GetMap()->IsDungeon();
  const bool group_has_human = GroupHasHuman(group);
  std::string non_combat =
      AdjustNonCombatStrategies(strategies_.non_combat, in_group, is_leader,
                                in_dungeon, is_leader && group_has_human);
  // A grouped bot's pet never runs off after mobs nobody pulled
  // (GroupReflexLimits): aggressive becomes defensive, as a player sets it.
  if (Pet* pet = player->GetPet();
      pet &&
      GroupReflexLimitsFor(in_group, in_dungeon, group_has_human)
          .pet_defensive &&
      pet->HasReactState(REACT_AGGRESSIVE)) {
    pet->SetReactState(REACT_DEFENSIVE);
    if (CharmInfo* charm = pet->GetCharmInfo()) {
      charm->SetPlayerReactState(REACT_DEFENSIVE);
    }
    LOG_INFO("module", "mod-llm-brain: {}: grouped, pet set to defensive",
             player->GetName());
  }
  {
    PolicyGlobalState& st = PolicyState();
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
  // Don't break crowd control: no area damage while a CC'd mob is near; the
  // area strategies the bot had come back after (Now #4).
  if (player->GetGroup()) {
    const bool cc_near = CrowdControlledMobNearby(player);
    std::vector<std::string> now_aoe;
    if (cc_near) {
      for (const std::string& name : ai->GetStrategies(BOT_STATE_COMBAT)) {
        if (name == "aoe" || name.ends_with(" aoe") || name == "baoe") {
          now_aoe.push_back(name);
        }
      }
    }
    const std::vector<std::string> off =
        BotSessionRegistry::Instance().WithSession(
            player->GetGUID().GetRawValue(), [&](BotSession& s) {
              for (const std::string& n : now_aoe) {
                if (std::find(s.suppressed_aoe.begin(), s.suppressed_aoe.end(),
                              n) == s.suppressed_aoe.end()) {
                  s.suppressed_aoe.push_back(n);
                }
              }
              std::vector<std::string> was = s.suppressed_aoe;
              if (!cc_near) {
                s.suppressed_aoe.clear();
              }
              return was;
            });
    for (const std::string& name : off) {
      combat += (cc_near ? ",-" : ",+") + name;
    }
    if (!now_aoe.empty()) {  // Only when it is switched off.
      LOG_INFO("module",
               "mod-llm-brain: {}: crowd control nearby, area damage off ({})",
               player->GetName(), now_aoe.front());
    }
  }
  // Both directions are spelled out: clearing a policy must undo it (T4).
  if (policy.not_tank) {
    combat += ",-tank assist,-pull back,+dps assist";
  } else if (PlayerbotAI::IsTank(player)) {
    combat += ",+tank assist,+pull back,-dps assist";
  }
  if (policy.stay || PingStayHolds(player)) {
    non_combat += ",-follow,+stay";
  } else {
    non_combat += ",-stay";
  }
  // Still as we left it in every state: playerbots hasn't reset any of them
  // since (it resets combat alone too, e.g. SelectiveResetStrategies).
  // A strategy this bot's class doesn't have (a warrior has no `boost`) can
  // never be added: don't count it, or the bot is re-strategized every tick.
  const auto supported = ai->GetAiObjectContext()->GetSupportedStrategies();
  const auto holds = [&](const std::string& changes, BotState state) {
    for (const StrategyCheck& check : StrategyChecks(changes)) {
      if (check.present && !supported.contains(check.name)) {
        continue;
      }
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
  PolicyGlobalState& st = PolicyState();
  base::AutoLock lock(st.lock);
  st.bot_overrides[bot_name] =
      BotStrategyOverride{std::move(combat_extra), std::move(non_combat_extra)};
}

BotManager::GroupPolicy BotManager::GetGroupPolicy(Player* bot) {
  if (!bot) {
    return {};
  }
  const Group* group = bot->GetGroup();
  const uint64_t group_guid = group ? group->GetGUID().GetRawValue() : 0;
  const uint64_t key = bot->GetGUID().GetRawValue();
  return BotSessionRegistry::Instance().WithSession(
      key, [group_guid](BotSession& s) -> GroupPolicy {
        if (s.group_guid == 0 || s.group_guid != group_guid) {
          s.group_guid = 0;
          s.group_policy = {};
          return {};
        }
        return {s.group_policy.not_tank, s.group_policy.stay};
      });
}

void BotManager::SetGroupPolicy(Player* bot, GroupPolicy policy) {
  if (!bot) {
    return;
  }
  const Group* group = bot->GetGroup();
  const uint64_t key = bot->GetGUID().GetRawValue();
  BotSessionRegistry::Instance().WithSession(
      key, [group, policy](BotSession& s) {
        if (!group || policy == GroupPolicy{}) {
          s.group_guid = 0;
          s.group_policy = {};
        } else {
          s.group_guid = group->GetGUID().GetRawValue();
          s.group_policy = {policy.not_tank, policy.stay};
        }
      });
}

bool BotManager::PingStayHolds(Player* bot) {
  if (!bot) {
    return false;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  const std::optional<BotSession::PingStay> stay =
      BotSessionRegistry::Instance().InspectSession(
          key,
          [](const BotSession* s) { return s ? s->ping_stay : std::nullopt; });
  if (!stay) {
    return false;
  }
  const Group* group = bot->GetGroup();
  bool holds = group && stay->map_id == bot->GetMapId() &&
               stay->instance_id == bot->GetInstanceId();
  if (holds && !bot->IsInCombat()) {
    const Player* leader =
        ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
    holds = leader && leader->IsInWorld() &&
            leader->GetMapId() == bot->GetMapId() &&
            leader->GetInstanceId() == bot->GetInstanceId() &&
            (leader == bot || bot->GetDistance(leader) <= kPingStayLeaderYd);
  }
  if (!holds) {
    BotSessionRegistry::Instance().WithSession(key, [&stay](BotSession& s) {
      if (s.ping_stay == stay) {
        s.ping_stay.reset();
      }
    });
    LOG_INFO("module", "mod-llm-brain: {} stops holding at the ping",
             bot->GetName());
  }
  return holds;
}

void BotManager::ClearPingStay(Player* bot) {
  if (!bot) {
    return;
  }
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(), [](BotSession& s) { s.ping_stay.reset(); });
}

void BotManager::StartPingStay(Player* bot) {
  if (!bot) {
    return;
  }
  const BotSession::PingStay stay{.map_id = bot->GetMapId(),
                                  .instance_id = bot->GetInstanceId(),
                                  .x = bot->GetPositionX(),
                                  .y = bot->GetPositionY(),
                                  .z = bot->GetPositionZ()};
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(),
      [&stay](BotSession& s) { s.ping_stay = stay; });
}

void BotManager::ClearBotStrategyOverride(const std::string& bot_name) {
  PolicyGlobalState& st = PolicyState();
  base::AutoLock lock(st.lock);
  st.bot_overrides.erase(bot_name);
}

void BotManager::ClearBotStrategyOverrides() {
  PolicyGlobalState& st = PolicyState();
  base::AutoLock lock(st.lock);
  st.bot_overrides.clear();
}

uint32_t BotManager::GetPendingSharedQuest(Player* bot) {
  if (!bot) {
    return 0;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  if (!bot->GetDivider()) {
    BotSessionRegistry::Instance().WithSession(key, [](BotSession& s) {
      s.pending_shared_quest_id = 0;
      s.pending_shared_quest_ms = 0;
      s.pending_shared_quest_confirm = false;
    });
    return 0;
  }
  bool timed_out = false;
  const uint32_t quest_id = BotSessionRegistry::Instance().WithSession(
      key, [&timed_out](BotSession& s) -> uint32_t {
        if (s.pending_shared_quest_id == 0) {
          return 0;
        }
        if (GetMSTimeDiffToNow(s.pending_shared_quest_ms) >=
            kPendingSharedQuestTimeoutMs) {
          s.pending_shared_quest_id = 0;
          s.pending_shared_quest_ms = 0;
          s.pending_shared_quest_confirm = false;
          timed_out = true;
          return 0;
        }
        return s.pending_shared_quest_id;
      });
  if (timed_out) {
    bot->SetDivider(ObjectGuid::Empty);
    return 0;
  }
  if (quest_id == 0) {
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

bool BotManager::IsPendingSharedQuestConfirm(Player* bot) {
  if (!bot || GetPendingSharedQuest(bot) == 0) {
    return false;
  }
  return BotSessionRegistry::Instance().InspectSession(
      bot->GetGUID().GetRawValue(),
      [](const BotSession* s) { return s && s->pending_shared_quest_confirm; });
}

void BotManager::ClearPendingSharedQuest(Player* bot) {
  if (!bot) {
    return;
  }
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(), [](BotSession& s) {
        s.pending_shared_quest_id = 0;
        s.pending_shared_quest_ms = 0;
        s.pending_shared_quest_confirm = false;
      });
}

bool BotManager::HasPendingReadyCheck(Player* bot) {
  if (!bot) {
    return false;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  const bool in_group = bot->GetGroup() != nullptr;
  return BotSessionRegistry::Instance().WithSession(
      key, [in_group](BotSession& s) {
        if (!in_group || !s.pending_ready_check_ms) {
          s.pending_ready_check_ms.reset();
          return false;
        }
        if (GetMSTimeDiffToNow(*s.pending_ready_check_ms) >=
            kPendingReadyCheckTimeoutMs) {
          s.pending_ready_check_ms.reset();
          return false;
        }
        return true;
      });
}

void BotManager::ClearPendingReadyCheck(Player* bot) {
  if (!bot) {
    return;
  }
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(),
      [](BotSession& s) { s.pending_ready_check_ms.reset(); });
}

uint32_t BotManager::GetPendingLfgProposal(Player* bot) {
  if (!bot) {
    return 0;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  return BotSessionRegistry::Instance().WithSession(
      key, [](BotSession& s) -> uint32_t {
        if (s.pending_lfg_proposal_id == 0) {
          return 0;
        }
        if (GetMSTimeDiffToNow(s.pending_lfg_proposal_ms) >=
            kPendingLfgProposalTimeoutMs) {
          s.pending_lfg_proposal_id = 0;
          s.pending_lfg_proposal_ms = 0;
          return 0;
        }
        return s.pending_lfg_proposal_id;
      });
}

void BotManager::ClearPendingLfgProposal(Player* bot) {
  if (!bot) {
    return;
  }
  BotSessionRegistry::Instance().WithSession(bot->GetGUID().GetRawValue(),
                                             [](BotSession& s) {
                                               s.pending_lfg_proposal_id = 0;
                                               s.pending_lfg_proposal_ms = 0;
                                             });
}

std::pair<std::string, std::string> BotManager::GetPendingBootVote(
    Player* bot) {
  if (!bot || !bot->GetGroup()) {
    return {};
  }
  return BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(),
      [](BotSession& s) -> std::pair<std::string, std::string> {
        if (s.pending_boot_victim.empty()) {
          return {};
        }
        if (s.pending_boot_until_ms != 0 &&
            static_cast<int32_t>(s.pending_boot_until_ms - getMSTime()) <= 0) {
          s.pending_boot_victim.clear();
          s.pending_boot_reason.clear();
          s.pending_boot_until_ms = 0;
          return {};
        }
        return {s.pending_boot_victim, s.pending_boot_reason};
      });
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
  auto [boot_victim, boot_reason] = GetPendingBootVote(bot);
  s.boot_victim = std::move(boot_victim);
  s.boot_reason = std::move(boot_reason);
  if (bot->IsAlive()) {
    if (bot->GetSummonExpireTimer() > GameTime::GetGameTime().count()) {
      s.summon = true;
      s.summon_from = BotSessionRegistry::Instance().InspectSession(
          bot->GetGUID().GetRawValue(), [](const BotSession* bs) {
            return bs ? bs->pending_summoner : std::string();
          });
    }
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
    BotSessionRegistry::Instance().WithSession(
        bot->GetGUID().GetRawValue(), [wait_ms](BotSession& s) {
          s.rez_wait_until_ms = getMSTime() + wait_ms;
        });
  }
}

void BotManager::Stop() {
  SetBrainConnected(false);
  {
    PolicyGlobalState& st = PolicyState();
    base::AutoLock lock(st.lock);
    st.bot_overrides.clear();
  }
  BotSessionRegistry::Instance().ClearAll();
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
