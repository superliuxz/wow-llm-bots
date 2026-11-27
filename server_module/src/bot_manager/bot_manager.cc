// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/bot_manager.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
#include "Common.h"
#include "Corpse.h"
#include "Creature.h"
#include "DBCStructure.h"
#include "Define.h"
#include "Engine.h"
#include "Event.h"
#include "G3D/Vector3.h"
#include "GameTime.h"
#include "GenericSpellActions.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"  // IWYU pragma: keep
#include "GridTerrainData.h"
#include "Group.h"
#include "IVMapMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "ServerScript.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TradeData.h"
#include "Unit.h"
#include "UpdateFields.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "bot_manager/corpse_run.h"
#include "bot_manager/errand_hold.h"
#include "bot_manager/human_group_grace.h"
#include "bot_manager/playerbots_actions.h"
#include "bot_manager/pull_discipline.h"
#include "bot_manager/spell_miss_packet.h"
#include "bot_manager/tank_swap.h"
#include "bot_manager/tick_owner.h"
#include "bot_session.h"
#include "chat/chat_scripts.h"
#include "config/config_parsing.h"
#include "options/act_geometry.h"
#include "options/exec_shared.h"
#include "popup_watch.h"
#include "snapshot/perception.h"
#include "snapshot/snapshot.h"

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

// Whether the connected harness reports its LLM backend as healthy.
std::atomic<bool>& LlmHealthyFlag() {
  static auto* flag = new std::atomic<bool>(true);
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

// Whether another member of `group` on the same map/instance is in combat.
bool GroupInCombat(Group* group, Player* self) {
  if (!group || !self) {
    return false;
  }
  for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
       itr = itr->next()) {
    Player* member = itr->GetSource();
    if (member && member != self && member->IsInWorld() &&
        member->GetMapId() == self->GetMapId() &&
        member->GetInstanceId() == self->GetInstanceId() &&
        member->IsInCombat()) {
      return true;
    }
  }
  return false;
}

ErrandHoldInput MakeErrandHoldInput(Player* bot) {
  Group* group = bot->GetGroup();
  return ErrandHoldInput{
      .in_group = group != nullptr,
      .is_leader = group != nullptr && group->IsLeader(bot->GetGUID()),
      .alive = bot->IsAlive(),
      .in_combat = bot->IsInCombat(),
      .party_in_combat = GroupInCombat(group, bot),
      .in_battleground = bot->InBattleground(),
      .harness_connected = BotManager::IsBrainConnected(),
      .map_id = bot->GetMapId(),
      .instance_id = bot->GetInstanceId(),
      .now_ms = getMSTime(),
  };
}

// Whether `member` is a tank in `group` (main-tank flag, LFG tank role, or
// tank spec/stance without `not_tank`).
bool IsGroupTankMember(Group* group, Player* member) {
  if (!group || !member || !member->IsInWorld()) {
    return false;
  }
  const ObjectGuid guid = member->GetGUID();
  for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
    if (slot.guid != guid) {
      continue;
    }
    if ((slot.flags & MEMBER_FLAG_MAINTANK) != 0) {
      return true;
    }
    if (slot.roles != 0) {
      return (slot.roles & lfg::PLAYER_ROLE_TANK) != 0;
    }
    return PlayerbotAI::IsTank(member) &&
           !BotManager::GetGroupPolicy(member).not_tank;
  }
  return false;
}

// Whether `bot`'s playerbots reflex taunt (`action_name`) should be suppressed
// to avoid taunt ping-pong with another living group tank (M1b).
bool ShouldBlockReflexTaunt(Player* bot,
                            PlayerbotAI* ai,
                            std::string_view action_name) {
  if (!bot || !ai || !IsReflexTauntActionName(action_name)) {
    return false;
  }
  Group* group = bot->GetGroup();
  if (!group) {
    return false;
  }
  Unit* target =
      ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
  if (!target) {
    target = bot->GetSelectedUnit();
  }
  if (!target) {
    return false;
  }
  // Righteous Defense targets a friendly player being attacked: never taunt
  // mobs off another living tank in the group.
  if (action_name == "righteous defense") {
    if (Player* ally = target->ToPlayer();
        ally && ally != bot && ally->IsAlive() &&
        group->IsMember(ally->GetGUID()) && IsGroupTankMember(group, ally)) {
      return true;
    }
  }
  bool victim_is_other_living_tank = false;
  if (Unit* victim = target->GetVictim()) {
    if (Player* vp = victim->ToPlayer(); vp && vp != bot && vp->IsAlive() &&
                                         group->IsMember(vp->GetGUID()) &&
                                         IsGroupTankMember(group, vp)) {
      victim_is_other_living_tank = true;
    }
  }
  bool target_taunted_by_other_tank = false;
  for (AuraEffect const* eff :
       target->GetAuraEffectsByType(SPELL_AURA_MOD_TAUNT)) {
    if (!eff) {
      continue;
    }
    if (Unit* caster = eff->GetCaster()) {
      if (Player* cp = caster->ToPlayer(); cp && cp != bot && cp->IsAlive() &&
                                           group->IsMember(cp->GetGUID()) &&
                                           IsGroupTankMember(group, cp)) {
        target_taunted_by_other_tank = true;
        break;
      }
    }
  }
  const uint32_t now_ms = getMSTime();
  const std::optional<TankSwapClaim> claim =
      TankSwapTracker::Instance().FindClaim(
          target->GetGUID().GetRawValue(), group->GetGUID().GetRawValue(),
          bot->GetMapId(), bot->GetInstanceId(), now_ms);
  bool claiming_tank_alive = false;
  bool claim_within_grace = false;
  uint64_t claiming_tank_guid = 0;
  if (claim) {
    claiming_tank_guid = claim->claiming_tank_guid;
    claim_within_grace = TankSwapTracker::IsWithinGrace(*claim, now_ms);
    if (Player* claimer = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid(claim->claiming_tank_guid))) {
      claiming_tank_alive = claimer->IsInWorld() && claimer->IsAlive() &&
                            claimer->GetMapId() == bot->GetMapId() &&
                            claimer->GetInstanceId() == bot->GetInstanceId();
    }
  }
  return ShouldSuppressReflexTaunt({
      .in_group = true,
      .bot_guid = bot->GetGUID().GetRawValue(),
      .target_guid = target->GetGUID().GetRawValue(),
      .victim_is_other_living_tank = victim_is_other_living_tank,
      .target_taunted_by_other_tank = target_taunted_by_other_tank,
      .has_active_claim = claim.has_value(),
      .claiming_tank_guid = claiming_tank_guid,
      .claiming_tank_alive = claiming_tank_alive,
      .claim_within_grace = claim_within_grace,
  });
}

// Whether another member of `group` is a non-pet tank (main-tank flag, LFG
// tank role, or tank spec/stance without `not_tank`).
bool GroupHasNonPetTank(Group* group, Player* self) {
  if (!group || !self) {
    return false;
  }
  for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
    if (slot.guid == self->GetGUID()) {
      continue;
    }
    if ((slot.flags & MEMBER_FLAG_MAINTANK) != 0) {
      return true;
    }
    if (slot.roles != 0) {
      if ((slot.roles & lfg::PLAYER_ROLE_TANK) != 0) {
        return true;
      }
      continue;
    }
    Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
    if (!member || !member->IsInWorld()) {
      continue;
    }
    if (PlayerbotAI::IsTank(member) &&
        !BotManager::GetGroupPolicy(member).not_tank) {
      return true;
    }
  }
  return false;
}

// Disables autocast on pet taunt spells (Growl, Torment, Suffering, Anguish)
// when `suppress_taunt` is true (grouped with a non-pet tank), and restores
// autocast when solo or when no non-pet tank is in the group.
void SyncPetTauntAutocast(Player* player, bool suppress_taunt) {
  Pet* pet = player ? player->GetPet() : nullptr;
  if (!pet) {
    return;
  }
  const bool want_autocast = !suppress_taunt;
  CharmInfo* charm = pet->GetCharmInfo();
  for (const auto& [spell_id, pet_spell] : pet->m_spells) {
    if (pet_spell.state == PETSPELL_REMOVED) {
      continue;
    }
    const uint32_t first_id = sSpellMgr->GetFirstSpellInChain(spell_id);
    if (!IsPetTauntSpell(spell_id) && !IsPetTauntSpell(first_id)) {
      continue;
    }
    const SpellInfo* spell_info = sSpellMgr->GetSpellInfo(spell_id);
    if (!spell_info || !spell_info->IsAutocastable()) {
      continue;
    }
    const bool is_autocast =
        std::find(pet->m_autospells.begin(), pet->m_autospells.end(),
                  spell_id) != pet->m_autospells.end();
    if (is_autocast == want_autocast) {
      continue;
    }
    pet->ToggleAutocast(spell_info, want_autocast);
    if (charm) {
      charm->ToggleCreatureAutocast(spell_info, want_autocast);
    }
    LOG_INFO("module", "mod-llm-brain: {}: pet {} autocast {}",
             player->GetName(), spell_info->SpellName[LOCALE_enUS],
             want_autocast ? "enabled" : "disabled");
  }
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
      case SMSG_TRADE_STATUS: {
        // WorldSession::SendTradeStatus: uint32 status first. BEGIN_TRADE is
        // the "X wants to trade" the client answers by opening the window.
        if (packet.size() >= 4) {
          ByteBuffer copy(packet);
          copy.rpos(0);
          uint32_t status = 0;
          copy >> status;
          if (status == TRADE_STATUS_BEGIN_TRADE ||
              status == TRADE_STATUS_OPEN_WINDOW ||
              status == TRADE_STATUS_CLOSE_WINDOW ||
              status == TRADE_STATUS_TRADE_CANCELED ||
              status == TRADE_STATUS_TRADE_COMPLETE) {
            const bool requested = status == TRADE_STATUS_BEGIN_TRADE;
            BotSessionRegistry::Instance().WithSession(
                key,
                [requested](BotSession& s) { s.trade_requested = requested; });
          }
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
      case SMSG_SPELL_GO: {
        // SMSG_SPELL_GO is broadcast to every player in visibility range:
        // record misses only when this bot is the real caster.
        if (packet.empty()) {
          break;
        }
        if (const std::optional<ParsedSpellGo> parsed = ParseSpellGoMisses(
                std::span<const uint8_t>(packet.contents(), packet.size()))) {
          if (parsed->caster_guid != key || parsed->misses.empty()) {
            break;
          }
          const SpellInfo* info = sSpellMgr->GetSpellInfo(parsed->spell_id);
          const char* raw_name =
              info ? info->SpellName[sWorld->GetDefaultDbcLocale()] : nullptr;
          if (!raw_name || raw_name[0] == '\0') {
            break;
          }
          const uint64_t selected_guid = bot->GetTarget().GetRawValue();
          const ParsedSpellGo::Miss* chosen = &parsed->misses.front();
          if (selected_guid != 0) {
            for (const ParsedSpellGo::Miss& m : parsed->misses) {
              if (m.target_guid == selected_guid) {
                chosen = &m;
                break;
              }
            }
          }
          std::string target_name;
          if (Unit* target = ObjectAccessor::GetUnit(
                  *bot, ObjectGuid(chosen->target_guid))) {
            target_name = target->GetName();
          }
          BotSession::SpellMiss miss{
              .spell_id = parsed->spell_id,
              .spell = raw_name,
              .target_guid = chosen->target_guid,
              .target = std::move(target_name),
              .reason = std::string(chosen->reason),
              .map_id = bot->GetMapId(),
              .instance_id = bot->GetInstanceId(),
              .ms = getMSTime(),
          };
          BotSessionRegistry::Instance().WithSession(
              key, [&miss](BotSession& s) {
                miss.seq = ++s.spell_miss_seq;
                s.spell_miss = std::move(miss);
              });
        }
        break;
      }
      case SMSG_CAST_FAILED: {
        // Sent directly to the caster (Spell::SendCastResult). Record the
        // failure for active option/act completion, and surface
        // SPELL_FAILED_IMMUNE as a caster-visible miss.
        if (packet.empty()) {
          break;
        }
        if (const std::optional<ParsedCastFailed> parsed = ParseCastFailed(
                std::span<const uint8_t>(packet.contents(), packet.size()))) {
          if (parsed->result == SPELL_FAILED_DONT_REPORT) {
            break;
          }
          const uint32_t now_ms = getMSTime();
          BotSessionRegistry::Instance().WithSession(
              key, [parsed, now_ms](BotSession& s) {
                s.cast_failure = BotSession::CastFailure{
                    .seq = ++s.cast_failure_seq,
                    .spell_id = parsed->spell_id,
                    .result = parsed->result,
                    .ms = now_ms,
                };
              });
          if (IsCastFailureImmune(parsed->result)) {
            const SpellInfo* info = sSpellMgr->GetSpellInfo(parsed->spell_id);
            const char* raw_name =
                info ? info->SpellName[sWorld->GetDefaultDbcLocale()] : nullptr;
            if (raw_name && raw_name[0] != '\0') {
              std::string target_name;
              uint64_t target_guid = 0;
              if (Unit* target = bot->GetSelectedUnit()) {
                target_guid = target->GetGUID().GetRawValue();
                target_name = target->GetName();
              }
              BotSession::SpellMiss miss{
                  .spell_id = parsed->spell_id,
                  .spell = raw_name,
                  .target_guid = target_guid,
                  .target = std::move(target_name),
                  .reason = "immune",
                  .map_id = bot->GetMapId(),
                  .instance_id = bot->GetInstanceId(),
                  .ms = now_ms,
              };
              BotSessionRegistry::Instance().WithSession(
                  key, [&miss](BotSession& s) {
                    miss.seq = ++s.spell_miss_seq;
                    s.spell_miss = std::move(miss);
                  });
            }
          }
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
//  - `"unstealth"` and all spell casts wait while a brain `use` of a
//    stealth-only spell walks in (BotSession::act_stealth_until_ms).
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
    // While the brain's `use` of a stealth-only spell walks in (Sap from
    // Stealth), playerbots' `unstealth` (its "often" non-combat trigger drops
    // stealth at random) and its casts would break the stealth the act
    // entered: they wait until the act casts, fails or is cancelled.
    if (bot_ && ai_ && HoldsActStealth() &&
        (name == "unstealth" || dynamic_cast<CastSpellAction*>(action))) {
      return false;
    }
    // While a dodge moves the bot out (BotSession::moving_out_until_ms), a
    // reflex spell that would hold it still (a cast time, or a rooting
    // channel) waits until it is out; instants still go out on the move.
    if (bot_ && ai_) {
      if (auto* cast = dynamic_cast<CastSpellAction*>(action);
          cast && HoldsDodgedBotStill(cast->getSpell())) {
        return false;
      }
    }
    // While a leader's "stop dps" is active, block offensive/attack actions
    // and non-positive spell casts while allowing heals, dispels, buffs, and
    // movement to continue uninterrupted.
    if (bot_ && BotManager::IsStopDps(bot_)) {
      if (IsStopDpsBlockedActionName(name)) {
        return false;
      }
      if (auto* cast = dynamic_cast<CastSpellAction*>(action); cast && ai_) {
        const SpellInfo* info = sSpellMgr->GetSpellInfo(
            ai_->GetAiObjectContext()
                ->GetValue<uint32>("spell id", cast->getSpell())
                ->Get());
        if (info && !info->IsPositive()) {
          return false;
        }
      }
    }
    // Tank-swap coordination (M1b): prevent playerbots' "lose aggro" reflex
    // from taunting a mob back off another living group tank after a swap.
    if (bot_ && ai_ && ShouldBlockReflexTaunt(bot_, ai_, name)) {
      return false;
    }
    // Pull discipline (M2a): belt-and-suspenders guard against Warlock
    // `fear on cc` (CastCrowdControlSpellAction("fear")) in groups or dungeons,
    // even during the brief window before EnsureStrategies strips `-cc`.
    if (bot_) {
      if (auto* cc = dynamic_cast<CastCrowdControlSpellAction*>(action);
          cc && ShouldBlockFearCcAction(
                    /*is_cc_spell_action=*/true, cc->getSpell(),
                    bot_->GetGroup() != nullptr,
                    bot_->GetMap() && bot_->GetMap()->IsDungeon(),
                    bot_->InBattleground())) {
        return false;
      }
    }
    // While a brain-initiated errand (loot, gather, talk, use, walk, rest,
    // NPC interaction) is active for a grouped follower, yield playerbots'
    // `follow` so it doesn't cancel the move or interrupt the cast.
    // Near unpulled hostile packs, also yield playerbots' ChaosFormation
    // `follow` (which jitters laterally into aggro edges) to BotManager's
    // safe follow.
    if (name == "follow" && bot_ && !bot_->IsInCombat() && bot_->GetGroup()) {
      if (BotManager::ErrandHoldActive(bot_)) {
        return false;
      }
      constexpr float kGuardRadiusYd = kUnpulledClearYd + 4.0f;
      std::vector<Unit*> units;
      Acore::AnyUnitInObjectRangeCheck check(bot_, kGuardRadiusYd);
      Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(
          bot_, units, check);
      Cell::VisitObjects(bot_, searcher, kGuardRadiusYd);
      for (Unit* u : units) {
        Creature* c = u->ToCreature();
        if (c && c->IsAlive() && !c->IsInCombat() &&
            c->HasReactState(REACT_AGGRESSIVE) && bot_->IsHostileTo(c) &&
            bot_->CanSeeOrDetect(c) &&
            !HiddenFromPlayers(c->IsTrigger(), c->GetDisplayId())) {
          return false;
        }
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
    // Keep playerbots' `toggle pet spell` from re-enabling Growl/Torment/
    // Suffering autocast while a non-pet tank is in the group.
    if (name == "toggle pet spell" && bot_) {
      Group* group = bot_->GetGroup();
      return !ShouldSuppressPetTaunt(group != nullptr,
                                     GroupHasNonPetTank(group, bot_));
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

  bool HoldsActStealth() {
    const uint32_t until = BotSessionRegistry::Instance().InspectSession(
        bot_->GetGUID().GetRawValue(),
        [](const BotSession* s) { return s ? s->act_stealth_until_ms : 0u; });
    return until != 0 && static_cast<int32_t>(until - getMSTime()) > 0;
  }

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

HealerSpec HealerSpecFor(Player* player) {
  const uint8_t tab = AiFactory::GetPlayerSpecTab(player);
  switch (player->getClass()) {
    case CLASS_PRIEST:
      if (tab == PRIEST_TAB_DISCIPLINE) {
        return HealerSpec::kPriestDiscipline;
      }
      if (tab == PRIEST_TAB_HOLY) {
        return HealerSpec::kPriestHoly;
      }
      return HealerSpec::kNone;
    case CLASS_DRUID:
      return tab == DRUID_TAB_RESTORATION ? HealerSpec::kDruidRestoration
                                          : HealerSpec::kNone;
    case CLASS_SHAMAN:
      return tab == SHAMAN_TAB_RESTORATION ? HealerSpec::kShamanRestoration
                                           : HealerSpec::kNone;
    case CLASS_PALADIN:
      return tab == PALADIN_TAB_HOLY ? HealerSpec::kPaladinHoly
                                     : HealerSpec::kNone;
    default:
      return HealerSpec::kNone;
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
  const bool llm_healthy = IsLlmHealthy();
  for (const Bot& bot : bots_) {
    if (!bot.guid) {
      continue;
    }
    Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(bot.guid));
    if (!player || !player->IsInWorld()) {
      continue;
    }
    const bool in_bg = player->InBattleground();
    const bool has_timed_popup =
        connected && !in_bg &&
        HasTimedPopup(ReadPopupState(player),
                      player->GetGroupInvite() != nullptr);
    TickOwner owner = TickOwner::kPlayerbots;
    const bool changed = BotSessionRegistry::Instance().WithSession(
        bot.guid, [&](BotSession& s) {
          const bool popup_deadline_missed = UpdatePopupDeadline(
              &s.popup_open_since_ms, has_timed_popup, now_ms_);
          owner = TickOwnerFor(connected, in_bg, llm_healthy,
                               popup_deadline_missed);
          return std::exchange(s.tick_owner, owner) != owner;
        });
    if (changed) {
      LOG_INFO("module", "mod-llm-brain: {} is {}", player->GetName(),
               TickOwnerName(owner));
    }
    if (!in_bg) {
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
                s.corpse_run_step.reset();
              });
        }
        if (!player->IsInCombat()) {
          TankSwapTracker::Instance().ClearForBot(bot.guid);
          if (!GroupInCombat(player->GetGroup(), player)) {
            BotManager::SetCombatPositionHold(player, false);
          }
        }
        EnsureStrategies(player);
        KeepCorpseRun(player);
        KeepNearHumanLeader(player);
        KeepSafeGroupFollow(player);
        KeepHealerInTankRangeAndLos(player);
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

namespace {

G3D::Vector3 PointAlongGhostPath(const Movement::PointsArray& points,
                                 float step_yd) {
  if (points.empty()) {
    return G3D::Vector3();
  }
  float acc = 0.0f;
  for (size_t i = 1; i < points.size(); ++i) {
    const G3D::Vector3 seg = points[i] - points[i - 1];
    const float len = seg.length();
    if (acc + len >= step_yd && len > 0.0f) {
      const float t = (step_yd - acc) / len;
      return points[i - 1] + seg * t;
    }
    acc += len;
  }
  return points.back();
}

// Walks a ghost toward `dest` on its current map using stepped navmesh paths
// so long exterior graveyard runs (e.g. Westfall -> Deadmines) succeed without
// cheat teleports. Returns the chosen step destination on success.
std::optional<wlb::snapshot::Vec3> StepGhostToward(Player* bot,
                                                   wlb::snapshot::Vec3 dest) {
  constexpr float kGhostStepYd = 70.0f;
  constexpr uint32_t kTypeOk =
      PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
  const float dist_to_dest = bot->GetExactDist(dest.x, dest.y, dest.z);

  {
    PathGenerator path(bot);
    path.CalculatePath(dest.x, dest.y, dest.z);
    const uint32_t path_type = path.GetPathType();
    if ((path_type & ~kTypeOk) == 0 && path.GetPath().size() >= 2) {
      const G3D::Vector3 cand =
          PointAlongGhostPath(path.GetPath(), kGhostStepYd);
      const G3D::Vector3& end = path.GetActualEndPosition();
      const float end_dist = std::hypot(end.x - dest.x, end.y - dest.y);
      if (bot->GetExactDist(cand.x, cand.y, cand.z) >= 1.5f &&
          ((path_type & PATHFIND_NORMAL) != 0 ||
           end_dist + 2.0f < dist_to_dest)) {
        if (WalkTo(bot, cand.x, cand.y, cand.z)) {
          return wlb::snapshot::Vec3{cand.x, cand.y, cand.z};
        }
      }
      // Portal AreaTriggers can sit slightly past the navmesh edge; walk to
      // the closest reachable point along the path if it moves us closer.
      if (dist_to_dest <= 15.0f && end_dist + 0.5f < dist_to_dest &&
          bot->GetExactDist(end.x, end.y, end.z) >= 1.0f &&
          WalkTo(bot, end.x, end.y, end.z)) {
        return wlb::snapshot::Vec3{end.x, end.y, end.z};
      }
    }
  }

  Map* map = bot->GetMap();
  const float base_angle = bot->GetAngle(dest.x, dest.y);
  const float step_len = std::min(kGhostStepYd, dist_to_dest);
  static constexpr float kOffsets[] = {0.0f, 0.7f, -0.7f};
  for (float offset : kOffsets) {
    const float angle = base_angle + offset;
    const float tx = bot->GetPositionX() + std::cos(angle) * step_len;
    const float ty = bot->GetPositionY() + std::sin(angle) * step_len;
    float tz = dest.z;
    if (map) {
      const float h = map->GetHeight(bot->GetPhaseMask(), tx, ty,
                                     bot->GetPositionZ() + 10.0f);
      if (h != INVALID_HEIGHT && h != VMAP_INVALID_HEIGHT_VALUE) {
        tz = h;
      }
    }
    PathGenerator path(bot);
    path.CalculatePath(tx, ty, tz);
    if ((path.GetPathType() & ~kTypeOk) == 0 && path.GetPath().size() >= 2) {
      const G3D::Vector3 cand =
          PointAlongGhostPath(path.GetPath(), kGhostStepYd);
      if (bot->GetExactDist(cand.x, cand.y, cand.z) >= 2.0f &&
          WalkTo(bot, cand.x, cand.y, cand.z)) {
        return wlb::snapshot::Vec3{cand.x, cand.y, cand.z};
      }
    }
  }
  return std::nullopt;
}

}  // namespace

void BotManager::KeepCorpseRun(Player* player) {
  if (!player || player->IsAlive() ||
      !player->HasPlayerFlag(PLAYER_FLAGS_GHOST) || !player->GetMap()) {
    return;
  }
  const uint64_t key = player->GetGUID().GetRawValue();
  const TickOwner owner = BotSessionRegistry::Instance().InspectSession(
      key, [](const BotSession* s) {
        return s ? s->tick_owner : TickOwner::kPlayerbots;
      });
  if (owner != TickOwner::kBrain) {
    return;
  }

  CorpseRunInput input;
  input.tick_owner = owner;
  input.alive = player->IsAlive();
  input.is_ghost = player->HasPlayerFlag(PLAYER_FLAGS_GHOST);
  input.in_battleground = player->InBattleground();
  input.in_arena = player->InArena();
  input.has_corpse = player->HasCorpse();
  input.being_teleported = player->IsBeingTeleported();
  input.can_move = CanClearBotMovement(player);
  input.bot_map_id = player->GetMapId();
  input.bot_pos = {player->GetPositionX(), player->GetPositionY(),
                   player->GetPositionZ()};

  if (input.has_corpse) {
    const WorldLocation corpse_loc = player->GetCorpseLocation();
    input.corpse_map_id = corpse_loc.GetMapId();
    input.corpse_pos = {corpse_loc.GetPositionX(), corpse_loc.GetPositionY(),
                        corpse_loc.GetPositionZ()};
    if (input.bot_map_id == input.corpse_map_id) {
      if (Corpse* corpse = player->GetCorpse()) {
        input.within_reclaim_range =
            corpse->IsInMap(player) &&
            corpse->IsWithinDist(player, CORPSE_RECLAIM_RADIUS, true);
        const time_t reclaim_at =
            corpse->GetGhostTime() +
            player->GetCorpseReclaimDelay(corpse->GetType() ==
                                          CORPSE_RESURRECTABLE_PVP);
        input.reclaim_ready =
            reclaim_at <= static_cast<time_t>(GameTime::GetGameTime().count());
      }
    } else {
      std::vector<InstanceEntranceCandidate> candidates;
      for (const auto& [trigger_id, at_tp] :
           sObjectMgr->GetAllAreaTriggerTeleports()) {
        if (at_tp.target_mapId != input.corpse_map_id) {
          continue;
        }
        const AreaTrigger* at = sObjectMgr->GetAreaTrigger(trigger_id);
        if (!at || at->map != input.bot_map_id) {
          continue;
        }
        candidates.push_back(InstanceEntranceCandidate{
            .trigger_id = trigger_id,
            .trigger_map_id = at->map,
            .trigger_pos = {at->x, at->y, at->z},
            .target_map_id = at_tp.target_mapId,
            .target_pos = {at_tp.target_X, at_tp.target_Y, at_tp.target_Z},
            .bot_inside_trigger = player->IsInAreaTriggerRadius(at, 0.0f),
        });
      }
      input.instance_entrance = SelectInstanceEntrance(
          input.bot_map_id, input.bot_pos, input.corpse_map_id,
          input.corpse_pos, candidates);
    }
  }

  const CorpseRunDecision decision = EvaluateCorpseRun(input);
  auto clear_step = [key]() {
    BotSessionRegistry::Instance().WithSession(
        key, [](BotSession& s) { s.corpse_run_step.reset(); });
  };
  switch (decision.action) {
    case CorpseRunAction::kNone:
      return;
    case CorpseRunAction::kWaitReclaimDelay:
      clear_step();
      if (player->isMoving()) {
        player->StopMoving();
        ClearLastMovement(player);
      }
      return;
    case CorpseRunAction::kReclaimCorpse: {
      clear_step();
      player->GetMotionMaster()->Clear();
      player->StopMoving();
      ClearLastMovement(player);
      if (WorldSession* sess = player->GetSession()) {
        WorldPacket pkt(CMSG_RECLAIM_CORPSE, 8);
        pkt << player->GetGUID();
        pkt.rpos(0);
        sess->HandleReclaimCorpseOpcode(pkt);
        LOG_INFO("module", "mod-llm-brain: {} reclaims corpse",
                 player->GetName());
      }
      return;
    }
    case CorpseRunAction::kEnterInstanceTrigger: {
      clear_step();
      player->GetMotionMaster()->Clear();
      player->StopMoving();
      ClearLastMovement(player);
      if (WorldSession* sess = player->GetSession()) {
        WorldPacket pkt(CMSG_AREATRIGGER, 4);
        pkt << uint32_t(decision.trigger_id);
        pkt.rpos(0);
        sess->HandleAreaTriggerOpcode(pkt);
        LOG_INFO("module",
                 "mod-llm-brain: {} enters instance portal trigger {} for "
                 "corpse on map {}",
                 player->GetName(), decision.trigger_id, input.corpse_map_id);
      }
      return;
    }
    case CorpseRunAction::kWalkToTarget: {
      constexpr float kStepAdvanceYd = 10.0f;
      const std::optional<BotSession::CorpseRunStep> cur_step =
          BotSessionRegistry::Instance().InspectSession(
              key, [](const BotSession* s) {
                return s ? s->corpse_run_step : std::nullopt;
              });
      if (cur_step && cur_step->map_id == input.bot_map_id &&
          player->isMoving() &&
          player->GetExactDist(cur_step->dest.x, cur_step->dest.y,
                               cur_step->dest.z) > kStepAdvanceYd) {
        return;
      }
      if (const std::optional<wlb::snapshot::Vec3> next =
              StepGhostToward(player, decision.target_pos)) {
        BotSessionRegistry::Instance().WithSession(
            key, [map_id = input.bot_map_id, dest = *next](BotSession& s) {
              s.corpse_run_step =
                  BotSession::CorpseRunStep{.map_id = map_id, .dest = dest};
            });
      }
      return;
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
      PingStayHolds(player) || ErrandHoldActive(player) ||
      !CanClearBotMovement(player)) {
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

namespace {

bool HasActiveForcedMove(PlayerbotAI* ai) {
  if (!ai) {
    return false;
  }
  auto* last =
      ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement");
  if (!last) {
    return false;
  }
  const LastMovement& lm = last->Get();
  return lm.priority == MovementPriority::MOVEMENT_FORCED &&
         static_cast<float>(GetMSTimeDiffToNow(lm.msTime)) < lm.lastdelayTime;
}

std::vector<wlb::snapshot::Vec3> CollectUnpulledHostiles(Player* bot,
                                                         float radius_yd) {
  std::vector<Unit*> units;
  Acore::AnyUnitInObjectRangeCheck check(bot, radius_yd);
  Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units,
                                                                     check);
  Cell::VisitObjects(bot, searcher, radius_yd);
  std::vector<wlb::snapshot::Vec3> out;
  for (Unit* u : units) {
    Creature* c = u->ToCreature();
    if (c && c->IsAlive() && !c->IsInCombat() &&
        c->HasReactState(REACT_AGGRESSIVE) && bot->IsHostileTo(c) &&
        bot->CanSeeOrDetect(c) &&
        !HiddenFromPlayers(c->IsTrigger(), c->GetDisplayId())) {
      out.push_back({c->GetPositionX(), c->GetPositionY(), c->GetPositionZ()});
    }
  }
  return out;
}

std::vector<Player*> FindCombatTanks(Player* healer, Group* group) {
  std::vector<Player*> tanks;
  if (!healer || !group) {
    return tanks;
  }
  auto add_if_eligible = [healer, &tanks](Player* m) {
    if (m && m != healer && m->IsInWorld() && m->IsAlive() && m->IsInCombat() &&
        m->GetMapId() == healer->GetMapId() &&
        m->GetInstanceId() == healer->GetInstanceId() &&
        std::find(tanks.begin(), tanks.end(), m) == tanks.end()) {
      tanks.push_back(m);
    }
  };
  for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
    Player* m = ObjectAccessor::FindConnectedPlayer(slot.guid);
    if (m && IsGroupTankMember(group, m)) {
      add_if_eligible(m);
    }
  }
  if (tanks.empty()) {
    add_if_eligible(
        ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID()));
  }
  return tanks;
}

// True when `healer` is already in heal range and LoS of an active friendly
// heal target (e.g. a selected injured ally, or an injured friendly combat NPC
// such as Valithria Dreamwalker or an escort NPC).
bool HasFriendlyHealTargetInRangeAndLos(Player* healer, float range_yd) {
  if (!healer || !healer->GetMap()) {
    return false;
  }
  if (Unit* sel = healer->GetSelectedUnit();
      sel && sel != healer && sel->IsAlive() && healer->IsFriendlyTo(sel) &&
      (sel->GetHealthPct() < 100.0f ||
       (sel->IsCreature() && sel->IsInCombat())) &&
      healer->GetDistance(sel) <= range_yd && healer->IsWithinLOSInMap(sel)) {
    return true;
  }
  std::vector<Unit*> units;
  Acore::AnyUnitInObjectRangeCheck check(healer, range_yd);
  Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(
      healer, units, check);
  Cell::VisitObjects(healer, searcher, range_yd);
  for (Unit* u : units) {
    Creature* c = u->ToCreature();
    if (c && c->IsAlive() && c->IsInCombat() && healer->IsFriendlyTo(c) &&
        c->GetHealthPct() < 100.0f &&
        !HiddenFromPlayers(c->IsTrigger(), c->GetDisplayId()) &&
        healer->GetDistance(c) <= range_yd && healer->IsWithinLOSInMap(c)) {
      return true;
    }
  }
  return false;
}

}  // namespace

void BotManager::KeepSafeGroupFollow(Player* player) {
  // Keeps grouped followers (with `+follow`, out of combat) trailing behind
  // their leader and off the aggro edge of nearby unpulled hostile packs
  // (Companion-lessons Gap 1).
  Group* group = player->GetGroup();
  PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
  if (!group || !ai || !player->IsAlive() || player->IsInCombat() ||
      group->IsLeader(player->GetGUID()) || !player->GetMap() ||
      !ai->HasStrategy("follow", BOT_STATE_NON_COMBAT) ||
      GetGroupPolicy(player).stay || PingStayHolds(player) ||
      ErrandHoldActive(player) || HasActiveForcedMove(ai) ||
      !CanClearBotMovement(player)) {
    return;
  }
  Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
  if (!leader || !leader->IsInWorld() || !leader->IsAlive() ||
      leader->IsInFlight() || leader->GetMapId() != player->GetMapId() ||
      leader->GetInstanceId() != player->GetInstanceId()) {
    return;
  }
  const wlb::snapshot::Vec3 bot_pos{
      player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()};
  const AnchorGeometry leader_anchor{
      .pos = {leader->GetPositionX(), leader->GetPositionY(),
              leader->GetPositionZ()},
      .facing_rad = leader->GetOrientation(),
      .radius_yd = leader->GetCombatReach()};
  const std::vector<wlb::snapshot::Vec3> unpulled =
      CollectUnpulledHostiles(player, 40.0f);
  const float dist_to_leader = player->GetDistance(leader);
  const bool encroaching =
      BotEncroachesUnpulled(bot_pos, leader_anchor.pos, unpulled);
  if (!encroaching && dist_to_leader <= kSafeFollowTriggerYd) {
    return;
  }
  const std::vector<wlb::snapshot::Vec3> spots =
      CandidateSpotsForSafeFollow(bot_pos, leader_anchor, unpulled);
  Map* map = player->GetMap();
  for (const wlb::snapshot::Vec3& cand : spots) {
    float z = cand.z;
    const float h = map->GetHeight(player->GetPhaseMask(), cand.x, cand.y,
                                   leader->GetPositionZ() + 4.0f);
    if (h != INVALID_HEIGHT && h != VMAP_INVALID_HEIGHT_VALUE) {
      z = h;
    }
    if (WalkTo(player, cand.x, cand.y, z)) {
      return;
    }
  }
  WalkTowards(player, leader->GetPositionX(), leader->GetPositionY(),
              leader->GetPositionZ());
}

void BotManager::KeepHealerInTankRangeAndLos(Player* player) {
  // Keeps a grouped healer within 40 yd (triggering at 34 yd) and line of
  // sight of a combat tank during a pull, yielding to stay policies, spread/
  // combat position holds, casts, brain forced-move acts, two-tank coverage
  // (already in range & LoS of any living combat tank), and active friendly
  // heal targets such as Valithria Dreamwalker (Companion-lessons Gap 1, M1b).
  Group* group = player->GetGroup();
  PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
  if (!group || !ai || !player->GetMap() || !CanClearBotMovement(player)) {
    return;
  }
  const std::vector<Player*> tanks = FindCombatTanks(player, group);
  if (tanks.empty()) {
    return;
  }
  Player* nearest_tank = tanks.front();
  float nearest_dist_yd = player->GetDistance(nearest_tank);
  bool any_tank_in_range_and_los = false;
  for (Player* t : tanks) {
    const float d = player->GetDistance(t);
    if (d < nearest_dist_yd) {
      nearest_dist_yd = d;
      nearest_tank = t;
    }
    if (d <= kHealerRepositionDistYd && player->IsWithinLOSInMap(t)) {
      any_tank_in_range_and_los = true;
    }
  }
  const bool stay = GetGroupPolicy(player).stay || PingStayHolds(player);
  const bool spread_or_position_hold = CombatPositionHoldActive(player);
  const bool forced_move = HasActiveForcedMove(ai);
  const bool casting = player->IsNonMeleeSpellCast(/*withDelayed=*/false);
  const bool in_los = player->IsWithinLOSInMap(nearest_tank);
  const bool heal_target_in_range_and_los =
      !any_tank_in_range_and_los &&
      HasFriendlyHealTargetInRangeAndLos(player, kHealerRepositionDistYd);
  if (!HealerNeedsTankReposition({
          .bot_alive = player->IsAlive(),
          .is_healer = PlayerbotAI::IsHeal(player),
          .in_group = true,
          .stay = stay,
          .spread_or_position_hold = spread_or_position_hold,
          .forced_move_active = forced_move,
          .casting = casting,
          .tank_alive = nearest_tank->IsAlive(),
          .tank_in_combat = nearest_tank->IsInCombat(),
          .any_tank_in_range_and_los = any_tank_in_range_and_los,
          .heal_target_in_range_and_los = heal_target_in_range_and_los,
          .dist_to_tank_yd = nearest_dist_yd,
          .in_los_of_tank = in_los,
      })) {
    return;
  }
  PathGenerator path(player);
  path.CalculatePath(nearest_tank->GetPositionX(), nearest_tank->GetPositionY(),
                     nearest_tank->GetPositionZ());
  const Movement::PointsArray& pts = path.GetPath();
  if (pts.size() >= 2) {
    // Stop at the earliest point along the path into LoS and <= 28 yd of the
    // tank so the healer rounds the corner without running into melee.
    for (size_t i = 1; i < pts.size(); ++i) {
      const G3D::Vector3& p = pts[i];
      const float d_tank = nearest_tank->GetExactDist(p.x, p.y, p.z);
      if (d_tank <= kHealerStopDistYd &&
          nearest_tank->IsWithinLOS(p.x, p.y, p.z + 1.5f)) {
        if (WalkTo(player, p.x, p.y, p.z)) {
          return;
        }
        break;
      }
    }
  }
  WalkTowards(player, nearest_tank->GetPositionX(),
              nearest_tank->GetPositionY(), nearest_tank->GetPositionZ());
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
  // stay offline past the grace period: 60 s in open world, 30 min inside a
  // dungeon or raid), the bots leave Dungeon Finder, teleport out of any
  // dungeon once out of combat, and disband the leftover bot group (Now #9 /
  // T11, M1a #1).
  const uint64_t key = player->GetGUID().GetRawValue();
  Group* group = player->GetGroup();
  const bool in_dungeon =
      player->GetMap() != nullptr && player->GetMap()->IsDungeon();

  HumanGroupGraceInput input{
      .group_guid = group ? group->GetGUID().GetRawValue() : 0,
      .has_online_human = group != nullptr && GroupHasHuman(group),
      .in_instance = in_dungeon,
      .in_combat = player->IsInCombat(),
      .now_ms = getMSTime(),
  };
  if (group && !input.has_online_human) {
    for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
      if (!ObjectAccessor::FindConnectedPlayer(slot.guid) &&
          !IsKnownBotGuid(slot.guid.GetRawValue())) {
        input.has_offline_human = true;
        break;
      }
    }
  }

  const HumanGroupAction action =
      BotSessionRegistry::Instance().WithSession(key, [&input](BotSession& s) {
        HumanGroupGraceState state{
            .human_group_guid = s.human_group_guid,
            .human_offline_since_ms = s.human_offline_since_ms,
        };
        const HumanGroupAction act = EvaluateHumanGroupGrace(state, input);
        s.human_group_guid = state.human_group_guid;
        s.human_offline_since_ms = state.human_offline_since_ms;
        return act;
      });
  if (action == HumanGroupAction::kKeep) {
    return;
  }

  sLFGMgr->LeaveLfg(player->GetGUID());
  ClearPendingLfgProposal(player);
  if (in_dungeon && !player->IsBeingTeleportedFar()) {
    sLFGMgr->TeleportPlayer(player, true);
  }
  if (action == HumanGroupAction::kAbandonGroup) {
    if (WorldSession* sess = player->GetSession(); sess && player->GetGroup()) {
      WorldPacket pkt(CMSG_GROUP_DISBAND, 0);
      sess->HandleGroupDisbandOpcode(pkt);
    }
  }
  if (in_dungeon && !player->IsBeingTeleportedFar() && player->GetMap() &&
      player->GetMap()->IsDungeon()) {
    player->TeleportToEntryPoint();
  }
  if (action == HumanGroupAction::kAbandonGroup || in_dungeon) {
    LOG_INFO("module", "mod-llm-brain: {} left group{} after human left",
             player->GetName(), in_dungeon ? " and teleported out" : "");
  }
}

void BotManager::EnsureStrategies(Player* player) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
  if (!ai || player->InBattleground()) {
    return;  // Not set up yet, or playerbots' battleground strategies apply.
  }
  // Solo, a healer also deals damage; in a group it heals, removing the solo
  // extras and explicitly restoring its primary healing strategy (playerbots'
  // combat strategy factories for Priest/Shaman/Paladin are exclusive).
  const HealerSpec healer_spec = HealerSpecFor(player);
  const bool is_healer = healer_spec != HealerSpec::kNone;
  const std::string extras(
      HealerCombatStrategyDelta(healer_spec, player->GetGroup() != nullptr));
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
    base_combat = GroupedCombatStrategies(base_combat, is_healer);
  }
  std::string combat = base_combat + extras;
  const bool brain_led =
      TickOwnerFor(IsBrainConnected(), player->InBattleground(),
                   IsLlmHealthy()) == TickOwner::kBrain;
  const bool is_leader =
      in_group && group->GetLeaderGUID() == player->GetGUID();
  const bool in_dungeon =
      player->GetMap() != nullptr && player->GetMap()->IsDungeon();
  const bool group_has_human = GroupHasHuman(group);
  std::string non_combat =
      AdjustNonCombatStrategies(strategies_.non_combat, in_group, is_leader,
                                in_dungeon, group_has_human, brain_led);
  const uint32_t map_id = player->GetMapId();
  const bool apply_instance_strategies =
      sPlayerbotAIConfig.ApplyInstanceStrategies;
  combat = AdjustRaidStrategies(combat, map_id, brain_led,
                                apply_instance_strategies);
  non_combat = AdjustRaidStrategies(non_combat, map_id, brain_led,
                                    apply_instance_strategies);
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
  // Suppress pet taunt autocast when grouped with a non-pet tank; restore it
  // when solo or when no non-pet tank is in the group.
  SyncPetTauntAutocast(
      player,
      ShouldSuppressPetTaunt(in_group, GroupHasNonPetTank(group, player)));
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
  // Warlock `cc` strategy casts Fear on the moon CC target, sending it into
  // adjacent packs (GenericWarlockStrategy.cpp:118-135). Suppress in groups
  // and dungeons (restored when solo in open world).
  if (std::string_view warlock_cc =
          WarlockCcStrategyDelta(player->getClass() == CLASS_WARLOCK, in_group,
                                 in_dungeon, player->InBattleground());
      !warlock_cc.empty()) {
    combat += warlock_cc;
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
  const bool combat_ok = holds(combat, BOT_STATE_COMBAT);
  const bool non_combat_ok = holds(non_combat, BOT_STATE_NON_COMBAT);
  const bool dead_ok = holds(strategies_.dead, BOT_STATE_DEAD);
  if (combat_ok && non_combat_ok && dead_ok) {
    return;
  }
  if (!combat_ok) {
    ai->ChangeStrategy(combat, BOT_STATE_COMBAT);
  }
  if (!non_combat_ok) {
    ai->ChangeStrategy(non_combat, BOT_STATE_NON_COMBAT);
  }
  if (!dead_ok) {
    ai->ChangeStrategy(strategies_.dead, BOT_STATE_DEAD);
  }
  LOG_INFO("module", "mod-llm-brain: applied strategies to {}",
           player->GetName());
}

void BotManager::SetBrainConnected(bool connected) {
  BrainConnectedFlag().store(connected, std::memory_order_release);
}

bool BotManager::IsBrainConnected() {
  return BrainConnectedFlag().load(std::memory_order_acquire);
}

void BotManager::SetLlmHealthy(bool healthy) {
  LlmHealthyFlag().store(healthy, std::memory_order_release);
}

bool BotManager::IsLlmHealthy() {
  return LlmHealthyFlag().load(std::memory_order_acquire);
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
  BotSessionRegistry::Instance().WithSession(bot->GetGUID().GetRawValue(),
                                             [](BotSession& s) {
                                               s.ping_stay.reset();
                                               s.combat_position_hold = false;
                                             });
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

bool BotManager::StartErrandHold(Player* bot, ErrandKind kind) {
  if (!bot) {
    return false;
  }
  const ErrandHoldInput input = MakeErrandHoldInput(bot);
  return BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(), [kind, &input](BotSession& s) {
        return llm_brain::StartErrandHold(s.errand_hold, kind, input);
      });
}

bool BotManager::ErrandHoldActive(Player* bot) {
  if (!bot) {
    return false;
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  // Fast path: avoid scanning group members when no errand hold is active.
  const bool maybe_active = BotSessionRegistry::Instance().InspectSession(
      key, [](const BotSession* s) { return s && s->errand_hold.active; });
  if (!maybe_active) {
    return false;
  }
  const ErrandHoldInput input = MakeErrandHoldInput(bot);
  return BotSessionRegistry::Instance().WithSession(
      key, [&input](BotSession& s) {
        return EvaluateErrandHold(s.errand_hold, input);
      });
}

void BotManager::ClearErrandHold(Player* bot) {
  if (!bot) {
    return;
  }
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(),
      [](BotSession& s) { llm_brain::ClearErrandHold(s.errand_hold); });
}

void BotManager::SetStopDps(Player* bot, bool stop) {
  if (!bot) {
    return;
  }
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(),
      [stop](BotSession& s) { s.stop_dps = stop; });
}

bool BotManager::IsStopDps(Player* bot) {
  if (!bot) {
    return false;
  }
  return BotSessionRegistry::Instance().InspectSession(
      bot->GetGUID().GetRawValue(),
      [](const BotSession* s) { return s ? s->stop_dps : false; });
}

void BotManager::SetCombatPositionHold(Player* bot, bool hold) {
  if (!bot) {
    return;
  }
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(),
      [hold](BotSession& s) { s.combat_position_hold = hold; });
}

bool BotManager::CombatPositionHoldActive(Player* bot) {
  if (!bot) {
    return false;
  }
  return BotSessionRegistry::Instance().InspectSession(
      bot->GetGUID().GetRawValue(),
      [](const BotSession* s) { return s ? s->combat_position_hold : false; });
}

void BotManager::RecordBrainTaunt(Player* bot, Unit* target) {
  if (!bot || !target) {
    return;
  }
  Group* group = bot->GetGroup();
  if (!group) {
    return;
  }
  TankSwapTracker::Instance().RecordClaim({
      .target_guid = target->GetGUID().GetRawValue(),
      .claiming_tank_guid = bot->GetGUID().GetRawValue(),
      .group_guid = group->GetGUID().GetRawValue(),
      .map_id = bot->GetMapId(),
      .instance_id = bot->GetInstanceId(),
      .claimed_at_ms = getMSTime(),
  });
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
  if (const TradeData* trade = bot->GetTradeData()) {
    if (const Player* trader = trade->GetTrader()) {
      s.trader = trader->GetName();
      const TradeData* theirs = trade->GetTraderData();
      s.trader_accepted = theirs && theirs->IsAccepted();
    }
  }
  return s;
}

void BotManager::OpenRequestedTrade(Player* bot) {
  // A request always comes with both sides' TradeData; a cancel, a close or
  // a finished trade clears the flag (LlmBrainPacketScript).
  if (!bot || !bot->IsInWorld() || !bot->GetSession() || !bot->GetTradeData()) {
    return;
  }
  const bool open = BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(), [](BotSession& s) {
        const bool requested = s.trade_requested;
        s.trade_requested = false;
        return requested && s.tick_owner == TickOwner::kBrain;
      });
  if (open) {
    WorldPacket p(CMSG_BEGIN_TRADE, 0);
    bot->GetSession()->HandleBeginTradeOpcode(p);
  }
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
  SetLlmHealthy(true);
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
