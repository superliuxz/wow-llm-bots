// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_social.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AiFactory.h"
#include "Cell.h"
#include "CellImpl.h"  // IWYU pragma: keep
#include "CharmInfo.h"
#include "Creature.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"  // IWYU pragma: keep
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "LootMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "Timer.h"
#include "TradeData.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "Vehicle.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "bot_manager/bot_manager.h"
#include "bot_manager/pull_discipline.h"
#include "bot_session.h"
#include "chat/chat_scripts.h"
#include "options/act_executor.h"
#include "options/action_types.h"
#include "options/bot_leader_guard.h"
#include "options/exec_items.h"
#include "options/exec_shared.h"
#include "options/group_roles.h"
#include "options/loot_roll_policy.h"
#include "options/options.h"
#include "options/riding_mounts.h"
#include "options/vehicle_controls.h"
#include "quest_helpers.h"
#include "snapshot/snapshot.h"
#include "snapshot/world_util.h"

namespace llm_brain {
namespace {

bool HasAnyGroupTargetIcon(Group* group, ObjectGuid guid) {
  if (!group || guid.IsEmpty()) {
    return false;
  }
  for (uint8_t icon = 0; icon < TARGETICONCOUNT; ++icon) {
    if (group->GetTargetIcon(icon) == guid) {
      return true;
    }
  }
  return false;
}

namespace pb = wlb::snapshot;

static_assert(kLfgRoleTank == lfg::PLAYER_ROLE_TANK);
static_assert(kLfgRoleHealer == lfg::PLAYER_ROLE_HEALER);
static_assert(kLfgRoleDamage == lfg::PLAYER_ROLE_DAMAGE);
static_assert(WARRIOR_TAB_PROTECTION == 2);
static_assert(PALADIN_TAB_HOLY == 0 && PALADIN_TAB_PROTECTION == 1);
static_assert(PRIEST_TAB_SHADOW == 2);
static_assert(DEATH_KNIGHT_TAB_BLOOD == 0);
static_assert(SHAMAN_TAB_RESTORATION == 2);
static_assert(DRUID_TAB_FERAL == 1 && DRUID_TAB_RESTORATION == 2);
static_assert(kRollMaskPass == ROLL_FLAG_TYPE_PASS);
static_assert(kRollMaskNeed == ROLL_FLAG_TYPE_NEED);
static_assert(kRollMaskGreed == ROLL_FLAG_TYPE_GREED);
static_assert(kRollMaskDisenchant == ROLL_FLAG_TYPE_DISENCHANT);
static_assert(static_cast<uint8_t>(LootRollChoice::kPass) == ROLL_PASS);
static_assert(static_cast<uint8_t>(LootRollChoice::kNeed) == ROLL_NEED);
static_assert(static_cast<uint8_t>(LootRollChoice::kGreed) == ROLL_GREED);
static_assert(static_cast<uint8_t>(LootRollChoice::kDisenchant) ==
              ROLL_DISENCHANT);

std::vector<LfgRoleChoice> SpecLfgRoles(Player* bot) {
  const bool has_talents =
      bot->GetFreeTalentPoints() < bot->CalculateTalentsPoints();
  return SpecLfgRoleChoices(bot->getClass(), AiFactory::GetPlayerSpecTab(bot),
                            has_talents, bot->GetLevel());
}

// Non-empty reason when `bot` or a connected party member has Dungeon Deserter
// or Random Dungeon cooldown, which blocks joining the random LFG queue (Now
// #9).
std::string PartyLfgBlockReason(Player* bot) {
  if (bot->HasAura(lfg::LFG_SPELL_DUNGEON_DESERTER)) {
    return "you have Dungeon Deserter";
  }
  if (bot->HasAura(lfg::LFG_SPELL_DUNGEON_COOLDOWN)) {
    return "Random Dungeon is on cooldown";
  }
  if (Group* group = bot->GetGroup()) {
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
         itr = itr->next()) {
      Player* m = itr->GetSource();
      if (!m || m == bot || !m->IsInWorld()) {
        continue;
      }
      if (m->HasAura(lfg::LFG_SPELL_DUNGEON_DESERTER)) {
        return m->GetName() + " has Dungeon Deserter";
      }
      if (m->HasAura(lfg::LFG_SPELL_DUNGEON_COOLDOWN)) {
        return m->GetName() + " is on Random Dungeon cooldown";
      }
    }
  }
  return "";
}

// The normal-difficulty random dungeon for the bot's level (server L2: never
// a Heroic, which needs level 80 and gear).
uint32_t FindRandomLfgDungeonId(Player* bot) {
  if (!bot) {
    return 0;
  }
  const uint8_t lvl = bot->GetLevel();
  uint32_t best_id = 0;
  uint32_t best_max_lvl = 0;
  for (uint32_t i = 0; i < sLFGDungeonStore.GetNumRows(); ++i) {
    const LFGDungeonEntry* d = sLFGDungeonStore.LookupEntry(i);
    if (!d || d->TypeID != lfg::LFG_TYPE_RANDOM ||
        d->Difficulty != DUNGEON_DIFFICULTY_NORMAL ||
        d->ExpansionLevel > bot->GetSession()->Expansion()) {
      continue;
    }
    if (lvl < d->MinLevel || lvl > d->MaxLevel) {
      continue;
    }
    if (d->MaxLevel >= best_max_lvl) {
      best_max_lvl = d->MaxLevel;
      best_id = d->ID;
    }
  }
  return best_id;
}

std::vector<uint8_t> SlotsFor(Player* bot, uint32_t inventory_type) {
  switch (inventory_type) {
    case INVTYPE_HEAD:
      return {EQUIPMENT_SLOT_HEAD};
    case INVTYPE_NECK:
      return {EQUIPMENT_SLOT_NECK};
    case INVTYPE_SHOULDERS:
      return {EQUIPMENT_SLOT_SHOULDERS};
    case INVTYPE_BODY:
      return {EQUIPMENT_SLOT_BODY};
    case INVTYPE_CHEST:
    case INVTYPE_ROBE:
      return {EQUIPMENT_SLOT_CHEST};
    case INVTYPE_WAIST:
      return {EQUIPMENT_SLOT_WAIST};
    case INVTYPE_LEGS:
      return {EQUIPMENT_SLOT_LEGS};
    case INVTYPE_FEET:
      return {EQUIPMENT_SLOT_FEET};
    case INVTYPE_WRISTS:
      return {EQUIPMENT_SLOT_WRISTS};
    case INVTYPE_HANDS:
      return {EQUIPMENT_SLOT_HANDS};
    case INVTYPE_FINGER:
      return {EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2};
    case INVTYPE_TRINKET:
      return {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2};
    case INVTYPE_CLOAK:
      return {EQUIPMENT_SLOT_BACK};
    case INVTYPE_WEAPON:
      if (bot->CanDualWield()) {
        return {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND};
      }
      return {EQUIPMENT_SLOT_MAINHAND};
    case INVTYPE_2HWEAPON:
    case INVTYPE_WEAPONMAINHAND:
      return {EQUIPMENT_SLOT_MAINHAND};
    case INVTYPE_SHIELD:
    case INVTYPE_WEAPONOFFHAND:
    case INVTYPE_HOLDABLE:
      return {EQUIPMENT_SLOT_OFFHAND};
    case INVTYPE_RANGED:
    case INVTYPE_RANGEDRIGHT:
    case INVTYPE_THROWN:
    case INVTYPE_RELIC:
      return {EQUIPMENT_SLOT_RANGED};
    default:
      return {};
  }
}

const char* ArmorTypeName(uint32_t subclass) {
  switch (subclass) {
    case ITEM_SUBCLASS_ARMOR_CLOTH:
      return "cloth";
    case ITEM_SUBCLASS_ARMOR_LEATHER:
      return "leather";
    case ITEM_SUBCLASS_ARMOR_MAIL:
      return "mail";
    case ITEM_SUBCLASS_ARMOR_PLATE:
      return "plate";
    default:
      return nullptr;
  }
}

// The heaviest body armor the bot can wear (its class's main armor type).
uint32_t BestArmorSubclass(Player* bot) {
  if (bot->HasSkill(SKILL_PLATE_MAIL)) {
    return ITEM_SUBCLASS_ARMOR_PLATE;
  }
  if (bot->HasSkill(SKILL_MAIL)) {
    return ITEM_SUBCLASS_ARMOR_MAIL;
  }
  if (bot->HasSkill(SKILL_LEATHER)) {
    return ITEM_SUBCLASS_ARMOR_LEATHER;
  }
  return ITEM_SUBCLASS_ARMOR_CLOTH;
}

// "agility, stamina, spell power" (the stats a player reads first).
std::string PrimaryStats(const ItemTemplate* proto) {
  std::string out;
  const auto add = [&out](const char* name) {
    if (out.find(name) == std::string::npos) {
      out += out.empty() ? name : std::string(", ") + name;
    }
  };
  for (uint32_t i = 0; i < proto->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i) {
    if (proto->ItemStat[i].ItemStatValue <= 0) {
      continue;
    }
    switch (proto->ItemStat[i].ItemStatType) {
      case ITEM_MOD_STRENGTH:
        add("strength");
        break;
      case ITEM_MOD_AGILITY:
        add("agility");
        break;
      case ITEM_MOD_INTELLECT:
        add("intellect");
        break;
      case ITEM_MOD_SPIRIT:
        add("spirit");
        break;
      case ITEM_MOD_STAMINA:
        add("stamina");
        break;
      case ITEM_MOD_SPELL_POWER:
        add("spell power");
        break;
      case ITEM_MOD_ATTACK_POWER:
      case ITEM_MOD_RANGED_ATTACK_POWER:
        add("attack power");
        break;
      case ITEM_MOD_MANA_REGENERATION:
        add("mp5");
        break;
      case ITEM_MOD_DEFENSE_SKILL_RATING:
        add("defense");
        break;
      case ITEM_MOD_DODGE_RATING:
        add("dodge");
        break;
      case ITEM_MOD_PARRY_RATING:
        add("parry");
        break;
      case ITEM_MOD_BLOCK_RATING:
      case ITEM_MOD_BLOCK_VALUE:
        add("block");
        break;
      case ITEM_MOD_EXPERTISE_RATING:
        add("expertise");
        break;
      case ITEM_MOD_ARMOR_PENETRATION_RATING:
        add("armor pen");
        break;
      default:
        break;
    }
  }
  return out;
}

ItemStatFlags ExtractItemStatFlags(const ItemTemplate* proto) {
  ItemStatFlags flags;
  if (!proto) {
    return flags;
  }
  for (uint32_t i = 0; i < proto->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i) {
    if (proto->ItemStat[i].ItemStatValue <= 0) {
      continue;
    }
    switch (proto->ItemStat[i].ItemStatType) {
      case ITEM_MOD_STRENGTH:
        flags.strength = true;
        break;
      case ITEM_MOD_AGILITY:
        flags.agility = true;
        break;
      case ITEM_MOD_INTELLECT:
        flags.intellect = true;
        break;
      case ITEM_MOD_SPIRIT:
        flags.spirit = true;
        break;
      case ITEM_MOD_STAMINA:
        flags.stamina = true;
        break;
      case ITEM_MOD_SPELL_POWER:
        flags.spell_power = true;
        break;
      case ITEM_MOD_ATTACK_POWER:
      case ITEM_MOD_RANGED_ATTACK_POWER:
        flags.attack_power = true;
        break;
      case ITEM_MOD_MANA_REGENERATION:
        flags.mp5 = true;
        break;
      case ITEM_MOD_DEFENSE_SKILL_RATING:
      case ITEM_MOD_DODGE_RATING:
      case ITEM_MOD_PARRY_RATING:
      case ITEM_MOD_BLOCK_RATING:
      case ITEM_MOD_BLOCK_VALUE:
        flags.tank_avoidance = true;
        break;
      case ITEM_MOD_EXPERTISE_RATING:
      case ITEM_MOD_ARMOR_PENETRATION_RATING:
        flags.melee_dps = true;
        break;
      default:
        break;
    }
  }
  return flags;
}

LootStatProfile BotLootStatProfile(Player* bot) {
  const bool has_talents =
      bot->GetFreeTalentPoints() < bot->CalculateTalentsPoints();
  return LootStatProfileForSpec(
      bot->getClass(), AiFactory::GetPlayerSpecTab(bot), has_talents,
      PlayerbotAI::IsTank(bot), PlayerbotAI::IsHeal(bot));
}

std::string RollItemFacts(Player* bot, const ItemTemplate* proto) {
  std::string out = "ilvl " + std::to_string(proto->ItemLevel);
  if (proto->Bonding == BIND_WHEN_EQUIPPED) {
    out += ", BoE";
  }
  const std::vector<uint8_t> slots = SlotsFor(bot, proto->InventoryType);
  if (slots.empty()) {
    return out + ", not gear";
  }
  if (const std::string stats = PrimaryStats(proto); !stats.empty()) {
    out += ", " + stats;
  }
  if (proto->Class == ITEM_CLASS_ARMOR &&
      proto->InventoryType != INVTYPE_CLOAK) {
    if (const char* type = ArmorTypeName(proto->SubClass)) {
      out += std::string(", ") + type;
      const uint32_t best = BestArmorSubclass(bot);
      if (proto->SubClass != best) {
        out += std::string(" (you wear ") + ArmorTypeName(best) + ")";
      }
    }
  }
  if (bot->CanUseItem(proto) != EQUIP_ERR_OK) {
    return out + ", you can't use it";
  }
  const LootStatProfile profile = BotLootStatProfile(bot);
  const StatFit stat_fit =
      EvaluateStatFit(profile, bot->getClass(), ExtractItemStatFlags(proto));
  const bool off_spec = stat_fit == StatFit::kMismatch ||
                        (proto->InventoryType == INVTYPE_SHIELD &&
                         IsShieldDisallowedForProfile(profile));
  uint32_t worn = UINT32_MAX;
  bool empty_slot = false;
  for (const uint8_t slot : slots) {
    const Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    const ItemTemplate* p = item ? item->GetTemplate() : nullptr;
    if (!p) {
      empty_slot = true;
      break;
    }
    worn = std::min(worn, p->ItemLevel);
  }
  if (off_spec) {
    if (empty_slot) {
      return out + ", usable, off-spec stats (empty slot)";
    }
    return out + ", usable, off-spec stats (you wear ilvl " +
           std::to_string(worn) + ")";
  }
  if (empty_slot) {
    return out + ", usable, upgrade (empty slot)";
  }
  if (proto->ItemLevel > worn) {
    return out + ", usable, upgrade over ilvl " + std::to_string(worn);
  }
  return out + ", usable, not an upgrade (you wear ilvl " +
         std::to_string(worn) + ")";
}

// The trade window's choices while it is open (accept, cancel, put up one
// more bag item), else a trade with each party member close enough. Only
// what the server would allow: the brain decides what to give and whether
// the trade is fair.
constexpr size_t kMaxTradeItemOptions = 8;

void CollectTradeActions(Player* bot,
                         std::vector<pb::OptionFacts_Action>* out) {
  if (!bot->IsAlive()) {
    return;
  }
  if (const TradeData* mine = bot->GetTradeData()) {
    const Player* trader = mine->GetTrader();
    if (!trader) {
      return;
    }
    const std::string with = trader->GetName();
    if (!mine->IsAccepted()) {
      out->push_back({"trade:accept", "Accept the trade with " + with});
    }
    out->push_back({"trade:cancel", "Cancel the trade with " + with});
    bool slot_free = false;
    for (uint8_t slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot) {
      slot_free |= mine->GetItem(static_cast<TradeSlots>(slot)) == nullptr;
    }
    if (!slot_free) {
      return;
    }
    // One option per kind of item (its first stack), in bag order.
    std::vector<uint32_t> offered;
    for (const BagSlotItem& b : BagItems(bot)) {
      if (offered.size() >= kMaxTradeItemOptions) {
        break;
      }
      const ItemTemplate* proto = b.item->GetTemplate();
      if (!proto || mine->HasItem(b.item->GetGUID()) ||
          !b.item->CanBeTraded(false, true) ||
          b.item->IsBindedNotWith(trader) ||
          std::find(offered.begin(), offered.end(), proto->ItemId) !=
              offered.end()) {
        continue;
      }
      offered.push_back(proto->ItemId);
      const uint32_t count = b.item->GetCount();
      out->push_back(
          {base::StringPrintf("trade:put:%u:%u:%u", b.bag, b.slot,
                              proto->ItemId),
           "Put " + proto->Name1 +
               (count > 1 ? " x" + std::to_string(count) : std::string()) +
               " in the trade"});
    }
    return;
  }
  Group* group = bot->GetGroup();
  if (!group || bot->IsInCombat()) {
    return;
  }
  for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
       itr = itr->next()) {
    Player* m = itr->GetSource();
    if (m && m != bot && m->IsInWorld() && m->IsAlive() && !m->GetTradeData() &&
        m->GetTeamId() == bot->GetTeamId() &&
        m->IsWithinDistInMap(bot, TRADE_DISTANCE, false)) {
      out->push_back({base::StringPrintf("trade:open:0x%016llx",
                                         static_cast<unsigned long long>(
                                             m->GetGUID().GetRawValue())),
                      "Open a trade with " + m->GetName()});
    }
  }
}

}  // namespace

void CollectGroupAndUpkeepActions(Player* bot,
                                  const pb::Snapshot& snapshot,
                                  std::vector<pb::OptionFacts_Action>* out) {
  out->clear();
  if (!bot || !bot->IsInWorld()) {
    return;
  }

  // 1. Need / Greed / Pass loot rolls.
  if (Group* group = bot->GetGroup()) {
    size_t rolls_added = 0;
    for (const Roll* roll : group->GetRolls()) {
      if (!roll || rolls_added >= 4) {
        break;
      }
      auto it = roll->playerVote.find(bot->GetGUID());
      if (it == roll->playerVote.end() || it->second != NOT_EMITED_YET) {
        continue;
      }
      const ItemTemplate* proto = sObjectMgr->GetItemTemplate(roll->itemid);
      const std::string item_name =
          proto ? proto->Name1 : ("item #" + std::to_string(roll->itemid));
      std::string detail =
          proto ? " (" + RollItemFacts(bot, proto) + ")" : std::string();
      // Who already rolled Need on it (Now #7: never Need against them).
      std::vector<std::string> needers;
      for (const auto& [guid, vote] : roll->playerVote) {
        if (vote == NEED && guid != bot->GetGUID()) {
          if (Player* p = ObjectAccessor::FindConnectedPlayer(guid)) {
            needers.push_back(p->GetName());
          }
        }
      }
      if (!needers.empty()) {
        std::string who;
        for (const std::string& n : needers) {
          who += (who.empty() ? "" : ", ") + n;
        }
        detail += " [Need rolled by " + who + "]";
      }
      const std::string prefix = base::StringPrintf(
          "roll:0x%016llx:%u:",
          static_cast<unsigned long long>(roll->itemGUID.GetRawValue()),
          static_cast<unsigned>(roll->itemSlot));
      if (roll->rollVoteMask & ROLL_FLAG_TYPE_NEED) {
        out->push_back({prefix + "1", "Need " + item_name + detail});
      }
      if (roll->rollVoteMask & ROLL_FLAG_TYPE_GREED) {
        out->push_back({prefix + "2", "Greed " + item_name + detail});
      }
      if (roll->rollVoteMask & ROLL_FLAG_TYPE_DISENCHANT) {
        out->push_back({prefix + "3", "Disenchant " + item_name});
      }
      out->push_back({prefix + "0", "Pass " + item_name});
      ++rolls_added;
    }
  }

  // 2. Ready check.
  if (BotManager::HasPendingReadyCheck(bot)) {
    out->push_back({"ready:yes", "Ready (confirm ready check)"});
    out->push_back({"ready:no", "Not ready (decline ready check)"});
  }

  // 2. (leader) A ready check before a boss or a pull (Now #5).
  // Guarded when grouped with a human: requires a human to ask/approve (M7).
  const base::TimeTicks now = base::TimeTicks::Now();
  bool group_has_human = false;
  if (Group* group = bot->GetGroup()) {
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
         itr = itr->next()) {
      if (Player* m = itr->GetSource(); m && IsHumanPlayer(m)) {
        group_has_human = true;
        break;
      }
    }
    if (!group_has_human) {
      const uint64_t gguid = group->GetGUID().GetRawValue();
      group_has_human = BotSessionRegistry::Instance().InspectSession(
          bot->GetGUID().GetRawValue(), [gguid](const BotSession* s) {
            return s && s->human_group_guid == gguid;
          });
    }
  }
  const BotLeaderApprovalState leader_approvals =
      BotSessionRegistry::Instance().InspectSession(
          bot->GetGUID().GetRawValue(), [](const BotSession* s) {
            return s ? s->leader_approvals : BotLeaderApprovalState{};
          });
  if (bot->IsAlive() && !bot->IsInCombat() && bot->GetGroup() &&
      bot->GetGroup()->IsLeader(bot->GetGUID()) &&
      !BotManager::HasPendingReadyCheck(bot) &&
      bot->GetGroup()->GetMembersCount() > 1 &&
      CanBotLeaderPerformAction(BotLeaderActionKind::kReadyCheck,
                                group_has_human, leader_approvals, now)) {
    out->push_back({"ready:start", "Start ready check"});
  }

  // 2a. A summon and a Dungeon Finder kick vote (Now #3).
  if (bot->GetSummonExpireTimer() > GameTime::GetGameTime().count()) {
    const std::string from = BotSessionRegistry::Instance().InspectSession(
        bot->GetGUID().GetRawValue(), [](const BotSession* s) {
          return s ? s->pending_summoner : std::string();
        });
    out->push_back({"summon:accept", from.empty()
                                         ? "Accept summon"
                                         : "Accept summon from " + from});
    out->push_back({"summon:decline", "Decline summon"});
  }
  {
    const auto [victim, reason] = BotManager::GetPendingBootVote(bot);
    if (!victim.empty()) {
      const std::string why = reason.empty() ? "" : " (" + reason + ")";
      out->push_back({"boot:yes", "Vote to kick " + victim + why});
      out->push_back({"boot:no", "Vote not to kick " + victim});
    }
  }

  // 2b. The trade window (Objective 9a).
  CollectTradeActions(bot, out);

  // 3. Shared / party-escort quest accept / decline (multiple queued shares).
  if (bot->IsAlive()) {
    size_t shown = 0;
    constexpr size_t kMaxShownSharedQuests = 4;
    for (const PendingSharedQuest& sq :
         BotManager::GetPendingSharedQuests(bot)) {
      if (shown >= kMaxShownSharedQuests) {
        break;
      }
      if (const Quest* q = sObjectMgr->GetQuestTemplate(sq.quest_id)) {
        const char* kind =
            sq.is_confirm ? "escort quest \"" : "shared quest \"";
        out->push_back({"qshare:accept:" + std::to_string(sq.quest_id),
                        std::string("Accept ") + kind + q->GetTitle() + "\""});
        out->push_back({"qshare:decline:" + std::to_string(sq.quest_id),
                        std::string("Decline ") + kind + q->GetTitle() + "\""});
        ++shown;
      }
    }
  }

  // 4. Dungeon Finder (LFG / RDF).
  if (bot->IsAlive()) {
    const lfg::LfgState st = sLFGMgr->GetState(bot->GetGUID());
    if (st == lfg::LFG_STATE_ROLECHECK) {
      for (const LfgRoleChoice& rc : SpecLfgRoles(bot)) {
        out->push_back(
            {"lfg:role:" + std::to_string(rc.mask),
             std::string("Confirm Dungeon Finder role (") + rc.name + ")"});
      }
    } else if (st == lfg::LFG_STATE_PROPOSAL) {
      if (const uint32_t pid = BotManager::GetPendingLfgProposal(bot);
          pid != 0) {
        out->push_back({"lfg:accept:" + std::to_string(pid),
                        "Accept Dungeon Finder proposal"});
        out->push_back({"lfg:decline:" + std::to_string(pid),
                        "Decline Dungeon Finder proposal"});
      }
    } else if (st == lfg::LFG_STATE_QUEUED) {
      out->push_back({"lfg:leave", "Leave Dungeon Finder queue"});
    } else if ((st == lfg::LFG_STATE_DUNGEON || st == lfg::LFG_STATE_BOOT ||
                st == lfg::LFG_STATE_FINISHED_DUNGEON) &&
               !bot->IsInCombat()) {
      if (bot->GetMap() && !bot->GetMap()->IsDungeon()) {
        out->push_back({"lfg:tele_in", "Teleport to LFG dungeon"});
      } else {
        out->push_back({"lfg:tele_out", "Teleport out of LFG dungeon"});
      }
    } else if (st == lfg::LFG_STATE_NONE && !bot->IsInCombat() &&
               bot->GetLevel() >= 15 && bot->GetMap() &&
               !bot->GetMap()->IsDungeon() &&
               (!bot->GetGroup() ||
                bot->GetGroup()->IsLeader(bot->GetGUID())) &&
               CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue,
                                         group_has_human, leader_approvals,
                                         now) &&
               PartyLfgBlockReason(bot).empty() &&
               FindRandomLfgDungeonId(bot) != 0) {
      for (const LfgRoleChoice& rc : SpecLfgRoles(bot)) {
        out->push_back(
            {"lfg:queue:" + std::to_string(rc.mask),
             std::string("Queue Random Dungeon (LFG: ") + rc.name + ")"});
      }
    }
  }

  // 5. Close-range Follow / Regroup with party leader.
  if (bot->IsAlive() && !bot->IsInCombat()) {
    if (Group* group = bot->GetGroup()) {
      const ObjectGuid leader_guid = group->GetLeaderGUID();
      if (leader_guid && leader_guid != bot->GetGUID()) {
        if (Player* leader = ObjectAccessor::FindConnectedPlayer(leader_guid);
            leader && leader->IsInWorld() && leader->IsAlive() &&
            !leader->HasPlayerFlag(PLAYER_FLAGS_GHOST) &&
            leader->GetMapId() == bot->GetMapId() &&
            leader->GetInstanceId() == bot->GetInstanceId()) {
          const float dist = bot->GetDistance(leader);
          if (dist >= 8.0f && dist <= 25.0f) {
            out->push_back({base::StringPrintf("follow:0x%016llx",
                                               static_cast<unsigned long long>(
                                                   leader_guid.GetRawValue())),
                            "Follow " + leader->GetName() + " (party leader)"});
          }
        }
      }
    }
  }

  // 5a. Share a quest with the party.
  if (bot->IsAlive() && !bot->IsInCombat()) {
    if (Group* group = bot->GetGroup()) {
      size_t shares = 0;
      for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE && shares < 2; ++slot) {
        const uint32_t qid = bot->GetQuestSlotQuestId(slot);
        const Quest* quest = qid ? sObjectMgr->GetQuestTemplate(qid) : nullptr;
        if (!quest || !bot->CanShareQuest(qid)) {
          continue;
        }
        bool someone_lacks_it = false;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
             itr = itr->next()) {
          Player* m = itr->GetSource();
          if (m && m != bot && m->IsInWorld() &&
              m->GetMapId() == bot->GetMapId() &&
              bot->GetDistance(m) <= 40.0f &&
              m->GetQuestStatus(qid) == QUEST_STATUS_NONE &&
              m->CanTakeQuest(quest, false)) {
            someone_lacks_it = true;
            break;
          }
        }
        if (someone_lacks_it) {
          out->push_back(
              {"qpush:" + std::to_string(qid),
               "Share quest \"" + quest->GetTitle() + "\" with the party"});
          ++shares;
        }
      }
    }
  }

  // 5b. Group policies.
  if (bot->IsAlive() && !bot->IsInCombat()) {
    if (Group* group = bot->GetGroup()) {
      const bool auto_popups = BotSessionRegistry::Instance().InspectSession(
          bot->GetGUID().GetRawValue(),
          [](const BotSession* s) { return s && s->auto_popups; });
      if (auto_popups) {
        out->push_back({"popups:manual", "Answer popups yourself again"});
      } else {
        out->push_back({"popups:auto",
                        "Answer popups automatically (accept rez, summons, "
                        "shared quests, ready checks and Dungeon Finder; Need "
                        "usable spec upgrades, else Disenchant/Greed)"});
      }
      const BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
      const bool self_effective_tank =
          snapshot.base.self.unit.group &&
          (snapshot.base.self.unit.group->role == pb::TANK ||
           snapshot.base.self.unit.group->main_tank);
      if (policy.not_tank) {
        out->push_back({"grouppolicy:tank", "Tank for the group again"});
      } else if (self_effective_tank) {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
             itr = itr->next()) {
          Player* m = itr->GetSource();
          if (m && m != bot && m->IsInWorld() &&
              m->GetMapId() == bot->GetMapId() &&
              PlayerbotAI::IsTank(m,
                                  /*bySpec=*/GET_PLAYERBOT_AI(m) == nullptr)) {
            out->push_back({"grouppolicy:notank",
                            "Let " + m->GetName() + " tank (stop tanking)"});
            break;
          }
        }
      }
      const ObjectGuid leader_guid = group->GetLeaderGUID();
      if (leader_guid && leader_guid != bot->GetGUID()) {
        if (policy.stay) {
          out->push_back(
              {"grouppolicy:follow", "Follow the party leader again"});
        } else {
          out->push_back(
              {"grouppolicy:stay", "Wait here (stop following the leader)"});
        }
      }

      // 5c. Bot-led group controls when a human is in the group (M6): promote
      //     human to leader, toggle Heroic/Normal dungeon difficulty, change
      //     loot method, or reset instances.
      if (leader_guid == bot->GetGUID()) {
        GroupLeaderControlsInput gl_in;
        gl_in.in_group = true;
        gl_in.self_is_leader = true;
        gl_in.alive = true;
        gl_in.in_combat = false;
        gl_in.is_raid = group->isRaidGroup();
        gl_in.is_lfg_restricted = group->isLFGGroup(true);
        gl_in.current_heroic =
            group->GetDungeonDifficulty() == DUNGEON_DIFFICULTY_HEROIC;
        gl_in.current_loot_method =
            static_cast<uint32_t>(group->GetLootMethod());
        gl_in.all_members_in_world = true;
        gl_in.any_member_in_dungeon =
            bot->GetMap() && bot->GetMap()->IsDungeon();
        for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
          if (slot.guid == bot->GetGUID()) {
            continue;
          }
          Player* m = ObjectAccessor::FindConnectedPlayer(slot.guid);
          if (!m || !m->IsInWorld()) {
            gl_in.all_members_in_world = false;
            if (m && IsHumanPlayer(m)) {
              gl_in.human_members.push_back(
                  {base::StringPrintf("0x%016llx",
                                      static_cast<unsigned long long>(
                                          slot.guid.GetRawValue())),
                   slot.name, /*online_in_world=*/false});
            }
            continue;
          }
          if (m->GetMap() && m->GetMap()->IsNonRaidDungeon()) {
            gl_in.any_member_in_dungeon = true;
          }
          if (IsHumanPlayer(m)) {
            gl_in.human_members.push_back(
                {base::StringPrintf(
                     "0x%016llx",
                     static_cast<unsigned long long>(slot.guid.GetRawValue())),
                 m->GetName(), /*online_in_world=*/true});
          }
        }
        for (pb::OptionFacts_Action& act : CollectGroupLeaderControls(gl_in)) {
          out->push_back(std::move(act));
        }
      }

      // 5d. Nearby Meeting Stone summon options (M13): let the brain pick
      //     which remote group member to summon at a usable Meeting Stone.
      for (const pb::GameObject& obj : snapshot.base.nearby.objects) {
        if (obj.kind != pb::GameObject::MEETING_STONE || !obj.usable ||
            obj.guid.empty()) {
          continue;
        }
        GameObject* go =
            ObjectAccessor::GetGameObject(*bot, ParseSnapshotGuid(obj.guid));
        if (!go || !go->IsInWorld() || !go->isSpawned() ||
            go->GetGoType() != GAMEOBJECT_TYPE_MEETINGSTONE) {
          continue;
        }
        const GameObjectTemplate* info = go->GetGOInfo();
        const uint8_t min_level =
            info ? static_cast<uint8_t>(info->meetingstone.minLevel) : 0;
        std::vector<MeetingStoneMember> members;
        for (const Group::MemberSlot& slot : group->GetMemberSlots()) {
          if (slot.guid == bot->GetGUID()) {
            continue;
          }
          Player* m = ObjectAccessor::FindConnectedPlayer(slot.guid);
          if (!m || !m->IsInWorld()) {
            continue;
          }
          const bool same_map = m->GetMapId() == bot->GetMapId() &&
                                m->GetInstanceId() == bot->GetInstanceId();
          const float dist = same_map ? bot->GetDistance(m)
                                      : std::numeric_limits<float>::infinity();
          members.push_back(MeetingStoneMember{
              .guid = base::StringPrintf(
                  "0x%016llx",
                  static_cast<unsigned long long>(slot.guid.GetRawValue())),
              .name = m->GetName(),
              .level = m->GetLevel(),
              .is_human = IsHumanPlayer(m),
              .online = true,
              .same_map_and_close = same_map && dist <= 40.0f,
              .dist_yd = dist,
          });
        }
        CollectMeetingStoneSummonActions(
            bot->IsAlive(), bot->IsInCombat(), bot->GetLevel(),
            MeetingStoneFact{.guid = obj.guid,
                             .name = obj.name,
                             .min_level = min_level,
                             .dist_yd = obj.rel.dist_yd},
            members, out);
        break;
      }
    }
  }

  // 6. Raid target mark: the leader, an assistant or the tank marks (T14);
  //    only offer mobs already in active combat with the group that are neither
  //    CC'd nor already marked — playerbots' AttackersValue treats any
  //    Skull-marked mob as an active attacker, so marking an unpulled mob
  //    chain-pulls its pack.
  if (bot->IsAlive()) {
    const bool self_effective_tank =
        snapshot.base.self.unit.group &&
        (snapshot.base.self.unit.group->role == pb::TANK ||
         snapshot.base.self.unit.group->main_tank);
    if (Group* group = bot->GetGroup();
        group &&
        (group->IsLeader(bot->GetGUID()) ||
         group->IsAssistant(bot->GetGUID()) ||
         (!group->isRaidGroup() && self_effective_tank &&
          !BotManager::GetGroupPolicy(bot).not_tank)) &&
        group->GetTargetIcon(7).IsEmpty()) {
      const pb::Unit* best = nullptr;
      for (const pb::Unit& u : snapshot.base.nearby.units) {
        Unit* live = !u.guid.empty() ? ObjectAccessor::GetUnit(
                                           *bot, ParseSnapshotGuid(u.guid))
                                     : nullptr;
        const bool is_cc =
            u.crowd_control.has_value() ||
            std::find(u.visual_state.begin(), u.visual_state.end(),
                      pb::Unit::BANISHED) != u.visual_state.end() ||
            (live && live->HasBreakableByDamageCrowdControlAura());
        const SkullMarkCandidate cand{
            .alive = !u.dead && live && live->IsAlive(),
            .is_npc = u.kind == pb::Unit::NPC,
            .hostile = u.reaction == pb::Unit::HOSTILE,
            .has_guid = !u.guid.empty(),
            .dist_yd = u.rel.dist_yd,
            .in_combat_with_group = IsUnitInCombatWithGroup(bot, group, live),
            .is_crowd_controlled = is_cc,
            .has_raid_marker =
                u.raid_marker != pb::RAID_MARKER_UNSPECIFIED ||
                (live && HasAnyGroupTargetIcon(group, live->GetGUID())),
        };
        if (!IsEligibleSkullMarkTarget(cand)) {
          continue;
        }
        if (!best || u.rel.dist_yd < best->rel.dist_yd) {
          best = &u;
        }
      }
      if (best) {
        out->push_back({"mark:" + best->guid + ":7",
                        "Mark " + best->name + " with Skull"});
      }
    }
  }

  // 6a. Crowd control on a mob the leader marked for it (moon, square, ...:
  //     any mark but skull and cross), with the bot's own CC spell (Now #4).
  if (bot->IsAlive() && bot->GetGroup()) {
    const uint32_t locale = sWorld->GetDefaultDbcLocale();
    const bool suppress_fear_cc = ShouldSuppressWarlockCcStrategy(
        /*is_warlock=*/true, /*in_group=*/true,
        bot->GetMap() && bot->GetMap()->IsDungeon(), bot->InBattleground());
    static constexpr const char* kCcSpells[] = {
        "Polymorph", "Hex",         "Sap",        "Shackle Undead",
        "Hibernate", "Banish",      "Repentance", "Entangling Roots",
        "Fear",      "Wyvern Sting"};
    const SpellInfo* cc = nullptr;
    for (const char* wanted : kCcSpells) {
      if (suppress_fear_cc && std::string_view(wanted) == "Fear") {
        continue;
      }
      for (const auto& [spell_id, state] : bot->GetSpellMap()) {
        if (!state || !state->Active || state->State == PLAYERSPELL_REMOVED) {
          continue;
        }
        const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
        const char* name = info ? info->SpellName[locale] : nullptr;
        if (name && std::string_view(name) == wanted &&
            (!cc || info->GetRank() > cc->GetRank())) {
          cc = info;
        }
      }
      if (cc) {
        break;
      }
    }
    size_t cc_added = 0;
    for (const pb::Unit& u : snapshot.base.nearby.units) {
      if (!cc || cc_added >= 2) {
        break;
      }
      if (u.dead || u.guid.empty() || u.reaction == pb::Unit::FRIENDLY ||
          u.raid_marker == pb::RAID_MARKER_UNSPECIFIED ||
          u.raid_marker == pb::SKULL || u.raid_marker == pb::CROSS ||
          u.rel.dist_yd > 35.0f) {
        continue;
      }
      // Only a target the spell can hold (Spell.dbc TargetCreatureType: rank-1
      // Sap takes humanoids only).
      Unit* live = ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(u.guid));
      if (!live || !cc->CheckTargetCreatureType(live)) {
        continue;
      }
      std::string mark(pb::EnumName(u.raid_marker));
      for (char& c : mark) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      out->push_back({"cc:" + std::to_string(cc->Id) + ":" + u.guid,
                      "Crowd-control " + u.name + " (" + mark +
                          "): " + cc->SpellName[locale]});
      ++cc_added;
    }
  }

  // 6b. Single-target Taunt on an enemy targeting someone else (M5.1: tank
  //     swaps and loose mobs). Discovered generically via SPELL_AURA_MOD_TAUNT
  //     + TARGET_UNIT_TARGET_ENEMY; prefers shortest cooldown (e.g. Dark
  //     Command over Death Grip, Taunt over Mocking Blow).
  if (bot->IsAlive() && bot->GetGroup() &&
      !BotManager::GetGroupPolicy(bot).not_tank &&
      snapshot.base.self.unit.group &&
      (snapshot.base.self.unit.group->role == pb::TANK ||
       snapshot.base.self.unit.group->main_tank)) {
    const SpellInfo* best_taunt = nullptr;
    uint32_t best_cd_ms = 0;
    for (const auto& [spell_id, state] : bot->GetSpellMap()) {
      if (!state || !state->Active || state->State == PLAYERSPELL_REMOVED) {
        continue;
      }
      const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
      if (!info || !info->HasAura(SPELL_AURA_MOD_TAUNT) ||
          bot->HasSpellCooldown(info->Id)) {
        continue;
      }
      bool targets_enemy = false;
      for (uint8_t i = 0; i < MAX_SPELL_EFFECTS; ++i) {
        if (info->Effects[i].TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY) {
          targets_enemy = true;
          break;
        }
      }
      if (!targets_enemy) {
        continue;
      }
      const uint32_t cd_ms =
          std::max(info->RecoveryTime, info->CategoryRecoveryTime);
      if (!best_taunt || cd_ms < best_cd_ms ||
          (cd_ms == best_cd_ms && info->GetRank() > best_taunt->GetRank())) {
        best_taunt = info;
        best_cd_ms = cd_ms;
      }
    }
    if (best_taunt) {
      CollectTauntActions(snapshot, best_taunt->Id, /*not_tank=*/false, out);
    }
  }

  // 6b. Battle resurrection (Druid Rebirth in combat; Warlock Soulstone).
  if (bot->IsAlive() && bot->GetGroup()) {
    BattleResCapability cap;
    // Druid Rebirth ranks (highest to lowest, WotLK 3.3.5a).
    constexpr uint32_t kRebirthSpellIds[] = {48477, 26994, 24239, 20747,
                                             20742, 20739, 20484};
    constexpr uint32_t kGlyphOfUnburdenedRebirth = 57855;
    const bool no_reagent_needed = bot->HasAura(kGlyphOfUnburdenedRebirth);
    for (uint32_t sid : kRebirthSpellIds) {
      if (!bot->HasActiveSpell(sid) || bot->HasSpellCooldown(sid)) {
        continue;
      }
      const SpellInfo* info = sSpellMgr->GetSpellInfo(sid);
      if (!info) {
        continue;
      }
      const int32_t cost = info->CalcPowerCost(bot, info->GetSchoolMask());
      if (cost > 0 && info->PowerType < MAX_POWERS &&
          bot->GetPower(static_cast<Powers>(info->PowerType)) <
              static_cast<uint32_t>(cost)) {
        continue;
      }
      bool reagents_ok = no_reagent_needed;
      if (!reagents_ok) {
        reagents_ok = true;
        for (uint8_t r = 0; r < MAX_SPELL_REAGENTS; ++r) {
          if (info->Reagent[r] > 0 && info->ReagentCount[r] > 0 &&
              !bot->HasItemCount(static_cast<uint32_t>(info->Reagent[r]),
                                 info->ReagentCount[r])) {
            reagents_ok = false;
            break;
          }
        }
      }
      if (reagents_ok) {
        cap.rebirth_spell_id = sid;
        if (const char* sn = info->SpellName[sWorld->GetDefaultDbcLocale()];
            sn && *sn) {
          cap.rebirth_spell_name = sn;
        }
        break;
      }
    }
    // Warlock Soulstone items in bags (highest to lowest, WotLK 3.3.5a).
    constexpr uint32_t kSoulstoneItemEntries[] = {36895, 22116, 16896, 16895,
                                                  16893, 16892, 5232};
    for (uint32_t entry : kSoulstoneItemEntries) {
      Item* item = bot->GetItemByEntry(entry);
      if (!item) {
        continue;
      }
      const ItemTemplate* proto = item->GetTemplate();
      if (!proto || proto->Spells[0].SpellId == 0 ||
          bot->HasSpellCooldown(proto->Spells[0].SpellId)) {
        continue;
      }
      cap.soulstone_item_entry = entry;
      cap.soulstone_item_name = proto->Name1;
      break;
    }
    if (cap.rebirth_spell_id != 0 || cap.soulstone_item_entry != 0) {
      CollectBattleResActions(snapshot, cap, out);
    }
  }

  // 7. Mount / Dismount.
  const bool incapacitated = bot->HasUnitState(
      UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING |
      UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED);
  if (CanOfferDismount(bot->IsAlive(), bot->IsMounted(), bot->IsInFlight(),
                       incapacitated)) {
    out->push_back({"dismount", "Dismount"});
  }
  const MountState mount_state{
      .alive = bot->IsAlive(),
      .in_combat = bot->IsInCombat(),
      .mounted = bot->IsMounted(),
      .in_flight = bot->IsInFlight(),
      .in_vehicle = bot->GetVehicle() != nullptr,
      .in_dungeon = !bot->GetMap() || bot->GetMap()->IsDungeon(),
      .outdoors = bot->IsOutdoors(),
      .swimming = bot->IsInWater(),
  };
  if (CanOfferMount(mount_state)) {
    const uint32_t locale = sWorld->GetDefaultDbcLocale();
    std::vector<KnownMountSpell> known_mounts;
    for (const auto& [spell_id, state] : bot->GetSpellMap()) {
      if (!state || !state->Active || state->State == PLAYERSPELL_REMOVED) {
        continue;
      }
      const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
      if (!info || !info->HasAura(SPELL_AURA_MOUNTED)) {
        continue;
      }
      const char* name = info->SpellName[locale];
      if (!name || !*name) {
        continue;
      }
      uint32_t ground_spd = 0;
      uint32_t flight_spd = 0;
      for (uint8_t i = 0; i < MAX_SPELL_EFFECTS; ++i) {
        const uint32_t aura = info->Effects[i].ApplyAuraName;
        if (aura == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED) {
          const int32_t val = info->Effects[i].CalcValue();
          if (val > 0 && static_cast<uint32_t>(val) > ground_spd) {
            ground_spd = static_cast<uint32_t>(val);
          }
        } else if (aura == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) {
          const int32_t val = info->Effects[i].CalcValue();
          if (val > 0 && static_cast<uint32_t>(val) > flight_spd) {
            flight_spd = static_cast<uint32_t>(val);
          }
        }
      }
      known_mounts.push_back(KnownMountSpell{
          .spell_id = spell_id,
          .name = name,
          .ground_speed_pct = ground_spd,
          .flight_speed_pct = flight_spd,
          .is_flying = flight_spd > 0,
      });
    }
    const bool can_fly =
        bot->GetMap() &&
        (bot->GetMapId() == 530 ||
         (bot->GetMapId() == 571 && bot->HasSpell(kSpellColdWeatherFlying)));
    if (std::optional<KnownMountSpell> best =
            PickBestMount(known_mounts, can_fly)) {
      out->push_back(
          {"mount:" + std::to_string(best->spell_id), "Mount " + best->name});
    }
  }

  // 8. Vehicle controls (action-bar abilities + exit).
  if (Vehicle* veh = bot->GetVehicle()) {
    const VehicleSeatEntry* seat = veh->GetSeatForPassenger(bot);
    const Unit* base = veh->GetBase();
    const bool can_cast = base && base->IsAlive() && seat &&
                          (seat->CanControl() ||
                           (seat->m_flags & VEHICLE_SEAT_FLAG_CAN_CAST) != 0);
    for (const VehicleSpellOption& opt :
         CollectVehicleSpellOptions(snapshot, can_cast, incapacitated)) {
      out->push_back(
          {"vehicle:spell:" + std::to_string(opt.spell_id), opt.label});
    }
    const bool can_exit = seat && seat->CanEnterOrExit();
    if (CanOfferVehicleExit(bot->IsAlive(), /*in_vehicle=*/true, can_exit,
                            incapacitated)) {
      out->push_back({"vehicle:leave", "Leave vehicle"});
    }
  }
}

ActionOutcome ExecuteInviteAction(Player* bot, const InviteAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  std::string player_name = action.player_name;
  Player* other = normalizePlayerName(player_name)
                      ? ObjectAccessor::FindPlayerByName(player_name)
                      : nullptr;
  if (!other || !other->IsInWorld()) {
    return {.error = action.player_name + " is not online"};
  }
  if (other == bot) {
    return {.error = "you can't invite yourself"};
  }
  if (other->GetGroup() != nullptr) {
    return {.error = action.player_name + " is already in a group"};
  }
  if (Group* g = bot->GetGroup()) {
    if (!g->IsLeader(bot->GetGUID()) && !g->IsAssistant(bot->GetGUID())) {
      return {.error = "only the group leader can invite"};
    }
    bool group_has_human = false;
    for (GroupReference* itr = g->GetFirstMember(); itr != nullptr;
         itr = itr->next()) {
      if (Player* m = itr->GetSource(); m && IsHumanPlayer(m)) {
        group_has_human = true;
        break;
      }
    }
    if (!group_has_human) {
      const uint64_t gguid = g->GetGUID().GetRawValue();
      group_has_human = BotSessionRegistry::Instance().InspectSession(
          bot->GetGUID().GetRawValue(), [gguid](const BotSession* s) {
            return s && s->human_group_guid == gguid;
          });
    }
    const BotLeaderApprovalState approvals =
        BotSessionRegistry::Instance().InspectSession(
            bot->GetGUID().GetRawValue(), [](const BotSession* s) {
              return s ? s->leader_approvals : BotLeaderApprovalState{};
            });
    if (!CanBotLeaderPerformAction(BotLeaderActionKind::kInvite,
                                   group_has_human, approvals,
                                   base::TimeTicks::Now(), other->GetName())) {
      return {.error = "cannot invite without a human group member asking"};
    }
  }
  WorldPacket pkt(CMSG_GROUP_INVITE, player_name.size() + 5);
  pkt << player_name << uint32(0);
  session->HandleGroupInviteOpcode(pkt);
  BotSessionRegistry::Instance().WithSession(
      bot->GetGUID().GetRawValue(), [](BotSession& s) {
        ConsumeBotLeaderApproval(s.leader_approvals,
                                 BotLeaderActionKind::kInvite);
      });
  return {.used = true, .info = "invited " + action.player_name + " to group"};
}

ActionOutcome ExecuteAcceptInviteAction(Player* bot,
                                        const AcceptInviteAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session || !bot->GetGroupInvite()) {
    return {.error = "no pending group invite"};
  }
  WorldPacket pkt(CMSG_GROUP_ACCEPT, 4);
  pkt << uint32(0);
  session->HandleGroupAcceptOpcode(pkt);
  if (!bot->GetGroup()) {
    return {.error = "couldn't join group"};
  }
  return {.used = true, .info = "joined " + action.from + "'s group"};
}

ActionOutcome ExecuteDeclineInviteAction(Player* bot,
                                         const DeclineInviteAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session || !bot->GetGroupInvite()) {
    return {.error = "no pending group invite"};
  }
  WorldPacket pkt(CMSG_GROUP_DECLINE, 0);
  session->HandleGroupDeclineOpcode(pkt);
  return {.used = true, .info = "declined group invite from " + action.from};
}

ActionOutcome ExecuteLeaveGroupAction(Player* bot) {
  WorldSession* session = bot->GetSession();
  if (!session || !bot->GetGroup()) {
    return {.error = "you are not in a group"};
  }
  WorldPacket pkt(CMSG_GROUP_DISBAND, 0);
  session->HandleGroupDisbandOpcode(pkt);
  return {.used = true, .info = "left the group"};
}

ActionOutcome ExecuteResurrectAction(Player* bot,
                                     const ResurrectAction& action) {
  switch (action.mode) {
    case ResurrectAction::Mode::kAccept:
      if (bot->IsAlive())
        return {.error = "you are already alive"};
      if (!bot->isResurrectRequested()) {
        return {.error = "no resurrection pending"};
      }
      if (bot->HasPreventResurectionAura()) {
        return {.error = "resurrection is currently prevented"};
      }
      bot->ResurectUsingRequestData();
      return {.used = true, .info = "accepted resurrection"};
    case ResurrectAction::Mode::kWait:
      if (bot->IsAlive() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
        return {.error = "you are not lying dead"};
      }
      BotManager::WaitForResurrection(bot, /*wait_ms=*/5 * 60 * 1000);
      // Leave `used = false` so `PlanRunner` holds the wait step until a
      // resurrection offer arrives or the step timeout re-checks, rather than
      // waking the brain on every snapshot at 1 Hz.
      return {.used = false, .info = "waiting for a resurrection"};
    case ResurrectAction::Mode::kSelf: {
      if (bot->IsAlive() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
        return {.error = "you are not lying dead"};
      }
      if (bot->GetUInt32Value(PLAYER_SELF_RES_SPELL) == 0) {
        return {.error = "you have no way to resurrect yourself"};
      }
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {.error = "no session"};
      }
      WorldPacket p(CMSG_SELF_RES, 0);
      session->HandleSelfResOpcode(p);
      if (!bot->IsAlive()) {
        return {.error = "could not resurrect yourself now"};
      }
      return {.used = true, .info = "resurrected yourself"};
    }
    case ResurrectAction::Mode::kRelease: {
      if (bot->IsAlive())
        return {.error = "you are already alive"};
      if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
        return {.error = "already released"};
      }
      if (bot->HasPreventResurectionAura()) {
        return {.error = "release spirit is currently prevented"};
      }
      WorldSession* session = bot->GetSession();
      if (!session)
        return {.error = "no session"};
      WorldPacket p(CMSG_REPOP_REQUEST, 1);
      p << uint8_t(0);
      p.rpos(0);
      session->HandleRepopRequestOpcode(p);
      return {.used = true, .info = "released spirit"};
    }
    case ResurrectAction::Mode::kSpiritHealer: {
      if (bot->IsAlive()) {
        return {.error = "you are already alive"};
      }
      if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
        return {.error = "you must release spirit first"};
      }
      if (bot->HasPreventResurectionAura()) {
        return {.error = "resurrection is currently prevented"};
      }
      WorldSession* session = bot->GetSession();
      if (!session) {
        return {.error = "no session"};
      }
      auto find_spirit_healer = [bot]() -> Creature* {
        std::vector<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, INTERACTION_DISTANCE);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(
            bot, units, check);
        Cell::VisitObjects(bot, searcher, INTERACTION_DISTANCE);
        for (Unit* u : units) {
          if (Creature* c = u->ToCreature();
              c && c->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER) &&
              bot->GetNPCIfCanInteractWith(c->GetGUID(),
                                           UNIT_NPC_FLAG_SPIRITHEALER)) {
            return c;
          }
        }
        return nullptr;
      };
      bot->GetMotionMaster()->Clear();
      bot->StopMoving();
      ClearLastMovement(bot);
      Creature* healer = find_spirit_healer();
      if (healer) {
        WorldPacket p(CMSG_SPIRIT_HEALER_ACTIVATE, 8);
        p << healer->GetGUID();
        p.rpos(0);
        session->HandleSpiritHealerActivateOpcode(p);
      } else {
        // Ghost is stuck away from the graveyard: return to graveyard first
        // (equivalent to WoW's "Return to Graveyard" button) and then apply
        // the legitimate Spirit Healer resurrection with sickness & durability
        // loss.
        bot->RepopAtGraveyard();
        if (Creature* gy_healer = find_spirit_healer()) {
          WorldPacket p(CMSG_SPIRIT_HEALER_ACTIVATE, 8);
          p << gy_healer->GetGUID();
          p.rpos(0);
          session->HandleSpiritHealerActivateOpcode(p);
        } else {
          session->SendSpiritResurrect();
        }
      }
      BotSessionRegistry::Instance().WithSession(bot->GetGUID().GetRawValue(),
                                                 [](BotSession& s) {
                                                   s.corpse_run_step.reset();
                                                   s.corpse_run_progress = {};
                                                 });
      if (!bot->IsAlive()) {
        return {.error = "could not resurrect at Spirit Healer now"};
      }
      return {.used = true, .info = "resurrected at Spirit Healer"};
    }
  }
  return {.error = "invalid resurrection action"};
}

ActionOutcome ExecuteLootRollAction(Player* bot, const LootRollAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  if (action.vote > 3) {  // Pass, Need, Greed, Disenchant.
    return {.error = "invalid loot roll"};
  }
  WorldPacket p(CMSG_LOOT_ROLL, 8 + 4 + 1);
  p << ParseSnapshotGuid(action.item_guid) << action.slot << action.vote;
  p.rpos(0);
  session->HandleLootRoll(p);
  return {.used = true, .info = action.label};
}

ActionOutcome ExecuteReadyCheckAction(Player* bot,
                                      const ReadyCheckAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  if (!bot->GetGroup()) {
    BotManager::ClearPendingReadyCheck(bot);
    return {.error = "you are not in a group"};
  }
  if (action.start) {
    Group* g = bot->GetGroup();
    if (!g->IsLeader(bot->GetGUID()) && !g->IsAssistant(bot->GetGUID())) {
      return {.error = "only the leader starts a ready check"};
    }
    bool group_has_human = false;
    for (GroupReference* itr = g->GetFirstMember(); itr != nullptr;
         itr = itr->next()) {
      if (Player* m = itr->GetSource(); m && IsHumanPlayer(m)) {
        group_has_human = true;
        break;
      }
    }
    if (!group_has_human) {
      const uint64_t gguid = g->GetGUID().GetRawValue();
      group_has_human = BotSessionRegistry::Instance().InspectSession(
          bot->GetGUID().GetRawValue(), [gguid](const BotSession* s) {
            return s && s->human_group_guid == gguid;
          });
    }
    const BotLeaderApprovalState approvals =
        BotSessionRegistry::Instance().InspectSession(
            bot->GetGUID().GetRawValue(), [](const BotSession* s) {
              return s ? s->leader_approvals : BotLeaderApprovalState{};
            });
    if (!CanBotLeaderPerformAction(BotLeaderActionKind::kReadyCheck,
                                   group_has_human, approvals,
                                   base::TimeTicks::Now())) {
      return {.error =
                  "cannot start a ready check without a human group member "
                  "asking"};
    }
    WorldPacket request(MSG_RAID_READY_CHECK, 0);
    session->HandleRaidReadyCheckOpcode(request);
    BotSessionRegistry::Instance().WithSession(
        bot->GetGUID().GetRawValue(), [](BotSession& s) {
          ConsumeBotLeaderApproval(s.leader_approvals,
                                   BotLeaderActionKind::kReadyCheck);
        });
    return {.used = true, .info = "started a ready check"};
  }
  const uint8_t state = action.ready ? 1 : 0;
  WorldPacket p(MSG_RAID_READY_CHECK, 1);
  p << state;
  p.rpos(0);
  session->HandleRaidReadyCheckOpcode(p);
  BotManager::ClearPendingReadyCheck(bot);
  return {.used = true,
          .info = state ? "confirmed ready check" : "declined ready check"};
}

ActionOutcome ExecuteSharedQuestAction(Player* bot,
                                       const SharedQuestAction& action) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {.error = "not a bot"};
  }
  const std::optional<PendingSharedQuest> pending =
      BotManager::TakePendingSharedQuest(bot, action.quest_id);
  if (!action.accept) {
    if (pending) {
      if (Player* sharer = ObjectAccessor::FindConnectedPlayer(
              ObjectGuid(pending->sharer_guid))) {
        sharer->SendPushToPartyResponse(bot, QUEST_PARTY_MSG_DECLINE_QUEST);
      }
    }
    bot->SetDivider(ObjectGuid::Empty);
    return {.used = true, .info = "declined shared quest"};
  }
  WorldSession* session = bot->GetSession();
  const Quest* quest = sObjectMgr->GetQuestTemplate(action.quest_id);
  if (!session || !quest || !pending || pending->sharer_guid == 0) {
    return {.error = "shared quest is no longer available"};
  }
  const ObjectGuid divider(pending->sharer_guid);
  bot->SetDivider(divider);
  if (pending->is_confirm || quest->HasFlag(QUEST_FLAGS_PARTY_ACCEPT)) {
    WorldPacket raw(CMSG_QUEST_CONFIRM_ACCEPT, 4);
    raw << action.quest_id;
    WorldPackets::Quest::QuestConfirmAcceptClient pkt(std::move(raw));
    pkt.Read();
    session->HandleQuestConfirmAccept(pkt);
  } else {
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST, 8 + 4 + 4);
    p << divider << action.quest_id << uint32_t(0);
    p.rpos(0);
    session->HandleQuestgiverAcceptQuestOpcode(p);
  }
  bot->SetDivider(ObjectGuid::Empty);
  if (bot->GetQuestStatus(action.quest_id) == QUEST_STATUS_NONE) {
    return {.error = "cannot accept shared quest"};
  }
  ai->RecordQuestAccepted(action.quest_id);
  return {.used = true,
          .info = (pending->is_confirm ? "accepted escort quest "
                                       : "accepted shared quest ") +
                  quest->GetTitle()};
}

ActionOutcome ExecuteShareQuestAction(Player* bot,
                                      const ShareQuestAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  if (!bot->CanShareQuest(action.quest_id)) {
    return {.error = "you can't share that quest"};
  }
  WorldPacket raw(CMSG_PUSHQUESTTOPARTY, 4);
  raw << action.quest_id;
  WorldPackets::Quest::PushQuestToParty packet(std::move(raw));
  packet.Read();
  session->HandlePushQuestToParty(packet);
  const Quest* quest = sObjectMgr->GetQuestTemplate(action.quest_id);
  return {.used = true,
          .info = "shared quest " + (quest ? quest->GetTitle()
                                           : std::to_string(action.quest_id))};
}

ActionOutcome ExecuteLfgAction(Player* bot, const LfgAction& action) {
  switch (action.op) {
    case LfgAction::Op::kQueue: {
      if (Group* g = bot->GetGroup()) {
        if (!g->IsLeader(bot->GetGUID())) {
          return {.error =
                      "only the group leader can queue for Dungeon Finder"};
        }
        bool group_has_human = false;
        for (GroupReference* itr = g->GetFirstMember(); itr != nullptr;
             itr = itr->next()) {
          if (Player* m = itr->GetSource(); m && IsHumanPlayer(m)) {
            group_has_human = true;
            break;
          }
        }
        if (!group_has_human) {
          const uint64_t gguid = g->GetGUID().GetRawValue();
          group_has_human = BotSessionRegistry::Instance().InspectSession(
              bot->GetGUID().GetRawValue(), [gguid](const BotSession* s) {
                return s && s->human_group_guid == gguid;
              });
        }
        const BotLeaderApprovalState approvals =
            BotSessionRegistry::Instance().InspectSession(
                bot->GetGUID().GetRawValue(), [](const BotSession* s) {
                  return s ? s->leader_approvals : BotLeaderApprovalState{};
                });
        if (!CanBotLeaderPerformAction(BotLeaderActionKind::kLfgQueue,
                                       group_has_human, approvals,
                                       base::TimeTicks::Now())) {
          return {.error =
                      "cannot queue Random Dungeon without a human group "
                      "member asking"};
        }
      }
      if (action.value == 0) {
        return {.error = "invalid LFG role"};
      }
      if (std::string why = PartyLfgBlockReason(bot); !why.empty()) {
        return {.error = "cannot queue for Dungeon Finder: " + std::move(why)};
      }
      const uint32_t did = FindRandomLfgDungeonId(bot);
      if (did == 0) {
        return {.error = "no random dungeon available for your level"};
      }
      lfg::LfgDungeonSet dungeons = {did};
      sLFGMgr->JoinLfg(bot, static_cast<uint8_t>(action.value), dungeons, "");
      if (sLFGMgr->GetState(bot->GetGUID()) == lfg::LFG_STATE_NONE) {
        return {.error = "could not join Dungeon Finder queue"};
      }
      BotSessionRegistry::Instance().WithSession(
          bot->GetGUID().GetRawValue(), [](BotSession& s) {
            ConsumeBotLeaderApproval(s.leader_approvals,
                                     BotLeaderActionKind::kLfgQueue);
          });
      return {.used = true, .info = "queued for Random Dungeon"};
    }
    case LfgAction::Op::kLeave:
      sLFGMgr->LeaveLfg(bot->GetGUID());
      BotManager::ClearPendingLfgProposal(bot);
      return {.used = true, .info = "left Dungeon Finder queue"};
    case LfgAction::Op::kRole:
      if (action.value == 0) {
        return {.error = "invalid LFG role"};
      }
      if (Group* g = bot->GetGroup()) {
        sLFGMgr->UpdateRoleCheck(g->GetGUID(), bot->GetGUID(),
                                 static_cast<uint8_t>(action.value));
        return {.used = true, .info = "confirmed Dungeon Finder role"};
      }
      return {.error = "you are not in a group"};
    case LfgAction::Op::kTeleIn:
      sLFGMgr->TeleportPlayer(bot, false);
      return {.used = true, .info = "teleported to LFG dungeon"};
    case LfgAction::Op::kTeleOut:
      sLFGMgr->TeleportPlayer(bot, true);
      return {.used = true, .info = "teleported out of LFG dungeon"};
    case LfgAction::Op::kAcceptProposal:
    case LfgAction::Op::kDeclineProposal: {
      const bool accept = action.op == LfgAction::Op::kAcceptProposal;
      sLFGMgr->UpdateProposal(action.value, bot->GetGUID(), accept);
      BotManager::ClearPendingLfgProposal(bot);
      return {.used = true,
              .info = accept ? "accepted Dungeon Finder proposal"
                             : "declined Dungeon Finder proposal"};
    }
  }
  return {.error = "unknown LFG action"};
}

ActionOutcome ExecuteGroupPolicyAction(Player* bot,
                                       const GroupPolicyAction& action) {
  if (!bot->GetGroup()) {
    return {.error = "you are not in a group"};
  }
  BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
  std::string info;
  switch (action.op) {
    case GroupPolicyAction::Op::kNoTank:
      policy.not_tank = true;
      info = "stopped tanking";
      break;
    case GroupPolicyAction::Op::kTank:
      policy.not_tank = false;
      info = "tanking again";
      break;
    case GroupPolicyAction::Op::kStay: {
      bool group_in_combat = bot->IsInCombat();
      if (!group_in_combat) {
        for (GroupReference* itr = bot->GetGroup()->GetFirstMember();
             itr != nullptr; itr = itr->next()) {
          Player* m = itr->GetSource();
          if (m && m != bot && m->IsInWorld() &&
              m->GetMapId() == bot->GetMapId() &&
              m->GetInstanceId() == bot->GetInstanceId() && m->IsInCombat()) {
            group_in_combat = true;
            break;
          }
        }
      }
      policy.stay = true;
      policy.stay_in_combat = group_in_combat;
      if (CanClearBotMovement(bot)) {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
      }
      info = "waiting here";
      break;
    }
    case GroupPolicyAction::Op::kFollow:
      policy.stay = false;
      policy.stay_in_combat = false;
      BotManager::ClearPingStay(bot);
      info = "following the leader again";
      break;
    case GroupPolicyAction::Op::kAutoPopups:
    case GroupPolicyAction::Op::kManualPopups: {
      const bool on = action.op == GroupPolicyAction::Op::kAutoPopups;
      BotSessionRegistry::Instance().WithSession(
          bot->GetGUID().GetRawValue(),
          [on](BotSession& s) { s.auto_popups = on; });
      return {.used = true,
              .info = on ? "popups are answered automatically now"
                         : "you answer popups yourself again"};
    }
  }
  BotManager::SetGroupPolicy(bot, policy);
  return {.used = true, .info = std::move(info)};
}

ActionOutcome ExecuteGroupLeaderAction(Player* bot,
                                       const GroupLeaderAction& action) {
  Group* group = bot ? bot->GetGroup() : nullptr;
  if (!group) {
    return {.error = "you are not in a group"};
  }
  if (!group->IsLeader(bot->GetGUID())) {
    return {.error = "only the party leader can do that"};
  }
  if (bot->IsInCombat()) {
    return {.error = "cannot change group settings in combat"};
  }
  switch (action.op) {
    case GroupLeaderAction::Op::kPromote: {
      const ObjectGuid target_guid = ParseSnapshotGuid(action.target_guid);
      Player* target = ObjectAccessor::FindConnectedPlayer(target_guid);
      if (!target || !target->IsInWorld() || target->GetGroup() != group ||
          target_guid == bot->GetGUID()) {
        return {.error = "that player is not an online party member"};
      }
      group->ChangeLeader(target_guid);
      group->SendUpdate();
      return {.used = true,
              .info = "promoted " + target->GetName() + " to party leader"};
    }
    case GroupLeaderAction::Op::kSetDifficulty: {
      if (group->isRaidGroup() || group->isLFGGroup(true)) {
        return {.error = "cannot change dungeon difficulty in this group"};
      }
      if (action.value >= MAX_DUNGEON_DIFFICULTY) {
        return {.error = "invalid dungeon difficulty"};
      }
      const Difficulty diff = static_cast<Difficulty>(action.value);
      if (group->GetDungeonDifficulty() == diff) {
        return {.used = true,
                .info = diff == DUNGEON_DIFFICULTY_HEROIC
                            ? "dungeon difficulty is already Heroic"
                            : "dungeon difficulty is already Normal"};
      }
      for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
           itr = itr->next()) {
        Player* m = itr->GetSource();
        if (!m || !m->IsInWorld()) {
          return {.error =
                      "everyone must be online to change dungeon difficulty"};
        }
        const bool in_dng =
            m->GetGUID() == bot->GetGUID()
                ? (m->GetMap() && m->GetMap()->IsDungeon())
                : (m->GetMap() && m->GetMap()->IsNonRaidDungeon());
        if (in_dng) {
          return {.error =
                      "everyone must be outside the dungeon to change "
                      "difficulty"};
        }
      }
      group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false, bot);
      group->SetDungeonDifficulty(diff);
      return {.used = true,
              .info = diff == DUNGEON_DIFFICULTY_HEROIC
                          ? "set dungeon difficulty to Heroic"
                          : "set dungeon difficulty to Normal"};
    }
    case GroupLeaderAction::Op::kSetLootMethod: {
      if (group->isLFGGroup(true)) {
        return {.error = "cannot change loot method in a Dungeon Finder group"};
      }
      if (action.value > NEED_BEFORE_GREED || action.value == MASTER_LOOT) {
        return {.error = "invalid loot method"};
      }
      const LootMethod method = static_cast<LootMethod>(action.value);
      group->SetLootMethod(method);
      group->SendUpdate();
      const char* name = "Group Loot";
      if (method == FREE_FOR_ALL) {
        name = "Free-for-All";
      } else if (method == ROUND_ROBIN) {
        name = "Round Robin";
      } else if (method == NEED_BEFORE_GREED) {
        name = "Need Before Greed";
      }
      return {.used = true, .info = std::string("set loot method to ") + name};
    }
    case GroupLeaderAction::Op::kResetInstances: {
      if (group->isLFGGroup(true)) {
        return {.error = "cannot reset a Dungeon Finder instance"};
      }
      for (GroupReference* itr = group->GetFirstMember(); itr != nullptr;
           itr = itr->next()) {
        Player* m = itr->GetSource();
        if (m && m->IsInWorld() && m->GetMap() && m->GetMap()->IsDungeon()) {
          return {.error =
                      "everyone must be outside the dungeon to reset "
                      "instances"};
        }
      }
      group->ResetInstances(INSTANCE_RESET_ALL, false, bot);
      return {.used = true, .info = "reset all instances"};
    }
  }
  return {.error = "unknown group leader action"};
}

ActionOutcome ExecuteFollowLeaderAction(Player* bot,
                                        const FollowLeaderAction& action) {
  {
    BotManager::GroupPolicy policy = BotManager::GetGroupPolicy(bot);
    policy.stay = false;
    policy.stay_in_combat = false;
    BotManager::SetGroupPolicy(bot, policy);
    BotManager::ClearPingStay(bot);
  }
  Player* leader = ObjectAccessor::FindConnectedPlayer(
      ParseSnapshotGuid(action.leader_guid));
  if (!leader || !leader->IsInWorld() || !leader->IsAlive() ||
      leader->HasPlayerFlag(PLAYER_FLAGS_GHOST) ||
      leader->GetMapId() != bot->GetMapId() ||
      leader->GetInstanceId() != bot->GetInstanceId()) {
    return {.error = "party leader is not nearby"};
  }
  RecordLastMovement(bot, leader->GetPositionX(), leader->GetPositionY(),
                     leader->GetPositionZ());
  bot->GetMotionMaster()->MoveFollow(leader, 4.0f, std::numbers::pi_v<float>);
  return {.used = true, .info = "following " + leader->GetName()};
}

ActionOutcome ExecuteMarkTargetAction(Player* bot,
                                      const MarkTargetAction& action) {
  Unit* u =
      ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(action.target_guid));
  if (action.icon == 7) {
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (u && group) {
      if (!IsUnitInCombatWithGroup(bot, group, u)) {
        return {.error = "target is not in active combat"};
      }
      if (u->HasBreakableByDamageCrowdControlAura()) {
        return {.error = "target is crowd-controlled"};
      }
      if (HasAnyGroupTargetIcon(group, u->GetGUID())) {
        return {.error = "target already has a raid mark"};
      }
    }
  }
  return MarkUnitWithIcon(bot, u, action.icon);
}

ItemActionOutcome ExecuteMountAction(Player* bot, const MountAction& action) {
  const MountState mount_state{
      .alive = bot->IsAlive(),
      .in_combat = bot->IsInCombat(),
      .mounted = bot->IsMounted(),
      .in_flight = bot->IsInFlight(),
      .in_vehicle = bot->GetVehicle() != nullptr,
      .in_dungeon = !bot->GetMap() || bot->GetMap()->IsDungeon(),
      .outdoors = bot->IsOutdoors(),
      .swimming = bot->IsInWater(),
  };
  if (std::string err =
          ValidateMountState(mount_state, bot->HasSpell(action.spell_id));
      !err.empty()) {
    return {.error = std::move(err)};
  }
  if (bot->isMoving()) {
    bot->StopMoving();
  }
  bot->CastSpell(bot, action.spell_id, false);
  const bool casting = IsSpellCastPending(bot, action.spell_id);
  if (!casting && !bot->IsMounted()) {
    return {.error = "cannot mount now"};
  }
  return {.used = !casting,
          .info = casting ? "" : ("mounted " + action.label),
          .pending_spell_id = casting ? action.spell_id : 0};
}

ActionOutcome ExecuteDismountAction(Player* bot) {
  const bool incapacitated = bot->HasUnitState(
      UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING |
      UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED);
  if (std::string err = ValidateDismountState(bot->IsMounted(),
                                              bot->IsInFlight(), incapacitated);
      !err.empty()) {
    return {.error = std::move(err)};
  }
  bot->Dismount();
  bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
  if (bot->IsMounted()) {
    return {.error = "could not dismount"};
  }
  return {.used = true, .info = "dismounted"};
}

ActionOutcome ExecuteVehicleSpellAction(Player* bot,
                                        const VehicleSpellAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  Vehicle* veh = bot->GetVehicle();
  Unit* base = veh ? veh->GetBase() : nullptr;
  Creature* creature = base ? base->ToCreature() : nullptr;
  const bool incapacitated = bot->HasUnitState(
      UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING |
      UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED);

  bool has_spell = false;
  if (base && action.spell_id != 0) {
    if (const CharmInfo* charm = base->GetCharmInfo()) {
      for (uint8_t i = ACTION_BAR_INDEX_START; i < ACTION_BAR_INDEX_END; ++i) {
        const UnitActionBarEntry* entry = charm->GetActionBarEntry(i);
        if (entry && entry->IsActionBarForSpell() &&
            entry->GetAction() == action.spell_id) {
          has_spell = true;
          break;
        }
      }
    }
    if (!has_spell && creature && creature->HasSpell(action.spell_id)) {
      has_spell = true;
    }
  }
  const bool on_cooldown =
      creature ? creature->HasSpellCooldown(action.spell_id) : false;
  if (std::string err = ValidateVehicleSpellState(
          veh != nullptr && base != nullptr, base && base->IsAlive(),
          incapacitated, has_spell, on_cooldown);
      !err.empty()) {
    return {.error = std::move(err)};
  }
  const SpellInfo* spell = sSpellMgr->GetSpellInfo(action.spell_id);
  if (!spell) {
    return {.error = "unknown vehicle ability"};
  }
  Unit* target = bot->GetSelectedUnit();
  if (!target) {
    target = bot->GetVictim();
  }
  if (!target) {
    target = base->GetVictim();
  }
  if (!target ||
      (spell->IsPositive() && target != base && !base->IsFriendlyTo(target))) {
    target = base;
  }
  WorldPacket p(CMSG_CAST_SPELL);
  p << static_cast<uint8_t>(0) << spell->Id << static_cast<uint8_t>(0);
  SpellCastTargets targets;
  if ((spell->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION) != 0) {
    targets.SetDst(*target);
  } else {
    targets.SetUnitTarget(target);
  }
  targets.Write(p);
  p.rpos(0);
  session->HandleCastSpellOpcode(p);
  return {.used = true, .info = action.label};
}

ActionOutcome ExecuteLeaveVehicleAction(Player* bot) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  Vehicle* veh = bot->GetVehicle();
  const VehicleSeatEntry* seat = veh ? veh->GetSeatForPassenger(bot) : nullptr;
  const bool incapacitated = bot->HasUnitState(
      UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING |
      UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED);
  if (std::string err = ValidateVehicleExitState(
          veh != nullptr, seat && seat->CanEnterOrExit(), incapacitated);
      !err.empty()) {
    return {.error = std::move(err)};
  }
  WorldPacket p(CMSG_REQUEST_VEHICLE_EXIT, 0);
  session->HandleRequestVehicleExit(p);
  if (bot->GetVehicle()) {
    return {.error = "could not leave vehicle"};
  }
  return {.used = true, .info = "left vehicle"};
}

ActStartResult ExecuteCrowdControlAction(Player* bot,
                                         const CrowdControlAction& action,
                                         base::TimeTicks now,
                                         ActiveAct* out_active) {
  const SpellInfo* spell = sSpellMgr->GetSpellInfo(action.spell_id);
  Unit* target =
      ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(action.target_guid));
  if (!spell || !bot->HasSpell(action.spell_id)) {
    return {.error = "you don't know that spell"};
  }
  if (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target)) {
    return {.error = "no target to crowd-control"};
  }
  if (bot->HasSpellCooldown(spell->Id)) {
    return {.error = action.label + " is on cooldown"};
  }
  // The same physics as `act: use`: stealth first for a stealth-only spell,
  // walk into range and sight, hold still while casting (CMSG_CAST_SPELL on
  // the target, like a player's click).
  return StartSpellUseOn(bot, /*command_id=*/0, spell, target, now, out_active);
}

ActStartResult ExecuteTauntAction(Player* bot,
                                  const TauntAction& action,
                                  base::TimeTicks now,
                                  ActiveAct* out_active) {
  const SpellInfo* spell = sSpellMgr->GetSpellInfo(action.spell_id);
  Unit* target =
      ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(action.target_guid));
  if (!spell || !bot->HasSpell(action.spell_id)) {
    return {.error = "you don't know that spell"};
  }
  if (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target)) {
    return {.error = "no target to taunt"};
  }
  if (bot->HasSpellCooldown(spell->Id)) {
    return {.error = action.label + " is on cooldown"};
  }
  ActStartResult res =
      StartSpellUseOn(bot, /*command_id=*/0, spell, target, now, out_active);
  if (res.error.empty()) {
    BotManager::RecordBrainTaunt(bot, target);
  }
  return res;
}

ActStartResult ExecuteBattleResAction(Player* bot,
                                      const BattleResAction& action,
                                      base::TimeTicks now,
                                      ActiveAct* out_active) {
  Player* target = ObjectAccessor::FindConnectedPlayer(
      ParseSnapshotGuid(action.target_guid));
  if (!target || !target->IsInWorld() || !target->IsInMap(bot)) {
    return {.error = "battle res target is not nearby"};
  }
  if (action.spell_id != 0) {
    const SpellInfo* spell = sSpellMgr->GetSpellInfo(action.spell_id);
    if (!spell || !bot->HasSpell(action.spell_id)) {
      return {.error = "you don't know that battle res spell"};
    }
    if (target->IsAlive() || target->HasPlayerFlag(PLAYER_FLAGS_GHOST)) {
      return {.error = "target is not a dead body"};
    }
    if (bot->HasSpellCooldown(spell->Id)) {
      return {.error = action.label + " is on cooldown"};
    }
    return StartSpellUseOn(bot, /*command_id=*/0, spell, target, now,
                           out_active);
  }
  if (action.item_entry != 0) {
    if (!target->IsAlive()) {
      return {.error = "soulstone target must be alive"};
    }
    Item* item = bot->GetItemByEntry(action.item_entry);
    if (!item) {
      return {.error = "no soulstone in bags"};
    }
    const ItemTemplate* proto = item->GetTemplate();
    if (!proto || proto->Spells[0].SpellId == 0) {
      return {.error = "invalid soulstone item"};
    }
    if (bot->HasSpellCooldown(proto->Spells[0].SpellId)) {
      return {.error = action.label + " is on cooldown"};
    }
    SpellCastTargets targets;
    targets.SetUnitTarget(target);
    bot->CastItemUseSpell(item, targets, 0, 0);
    return {.done = true};
  }
  return {.error = "invalid battle res action"};
}

ActionOutcome ExecuteTradeAction(Player* bot, const TradeAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  TradeData* mine = bot->GetTradeData();
  if (action.op == TradeAction::Op::kOpen) {
    if (mine) {
      return {.error = "you are already trading"};
    }
    Player* other = ObjectAccessor::FindConnectedPlayer(
        ParseSnapshotGuid(action.target_guid));
    if (!other || !other->IsInWorld()) {
      return {.error = "they are not online"};
    }
    WorldPacket p(CMSG_INITIATE_TRADE, 8);
    p << other->GetGUID();
    p.rpos(0);
    session->HandleInitiateTradeOpcode(p);
    if (!bot->GetTradeData()) {
      return {.error = "can't trade with " + other->GetName() + " now"};
    }
    return {.used = true, .info = "asked " + other->GetName() + " to trade"};
  }
  if (!mine || !mine->GetTrader()) {
    return {.error = "no trade window is open"};
  }
  const std::string with = mine->GetTrader()->GetName();
  switch (action.op) {
    case TradeAction::Op::kAccept: {
      WorldPacket p(CMSG_ACCEPT_TRADE, 4);
      p << uint32_t(1);
      p.rpos(0);
      session->HandleAcceptTradeOpcode(p);
      if (!bot->GetTradeData()) {
        return {.used = true, .info = "traded with " + with};
      }
      if (!bot->GetTradeData()->IsAccepted()) {
        return {.error = "the trade with " + with + " can't go through"};
      }
      return {.used = true, .info = "accepted the trade; waiting for " + with};
    }
    case TradeAction::Op::kCancel: {
      WorldPacket p(CMSG_CANCEL_TRADE, 0);
      session->HandleCancelTradeOpcode(p);
      return {.used = true, .info = "cancelled the trade with " + with};
    }
    case TradeAction::Op::kPutItem: {
      Item* item = bot->GetItemByPos(action.bag, action.slot);
      if (!item || item->GetEntry() != action.entry) {
        return {.error = "that item is no longer there"};
      }
      int free_slot = -1;
      for (uint8_t slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot) {
        if (!mine->GetItem(static_cast<TradeSlots>(slot))) {
          free_slot = slot;
          break;
        }
      }
      if (free_slot < 0) {
        return {.error = "the trade window is full"};
      }
      WorldPacket p(CMSG_SET_TRADE_ITEM, 3);
      p << uint8_t(free_slot) << action.bag << action.slot;
      p.rpos(0);
      session->HandleSetTradeItemOpcode(p);
      TradeData* now = bot->GetTradeData();
      if (!now || !now->HasItem(item->GetGUID())) {
        return {.error = "can't trade " + item->GetTemplate()->Name1};
      }
      return {.used = true,
              .info = "put " + item->GetTemplate()->Name1 + " in the trade"};
    }
    case TradeAction::Op::kOpen:
      break;
  }
  return {.error = "invalid trade action"};
}

ActionOutcome ExecuteTradeGold(Player* bot, uint32_t copper) {
  WorldSession* session = bot ? bot->GetSession() : nullptr;
  if (!session) {
    return {.error = "no session"};
  }
  TradeData* mine = bot->GetTradeData();
  const bool has_trade = mine && mine->GetTrader() != nullptr;
  if (std::string err = ValidateTradeGold(has_trade, copper, bot->GetMoney());
      !err.empty()) {
    return {.error = std::move(err)};
  }
  WorldPacket p(CMSG_SET_TRADE_GOLD, 4);
  p << copper;
  p.rpos(0);
  session->HandleSetTradeGoldOpcode(p);
  TradeData* now = bot->GetTradeData();
  if (!now || now->GetMoney() != copper) {
    return {.error = "can't set trade gold"};
  }
  return {.used = true, .info = "put money in the trade"};
}

ActionOutcome ExecutePopupAnswerAction(Player* bot,
                                       const PopupAnswerAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  const uint64_t key = bot->GetGUID().GetRawValue();
  if (action.kind == PopupAnswerAction::Kind::kSummon) {
    if (bot->GetSummonExpireTimer() <= GameTime::GetGameTime().count()) {
      return {.error = "no summon to answer"};
    }
    bot->SummonIfPossible(action.yes, ObjectGuid::Empty);
    BotSessionRegistry::Instance().WithSession(
        key, [](BotSession& s) { s.pending_summoner.clear(); });
    return {.used = true,
            .info = action.yes ? "accepted the summon" : "declined the summon"};
  }
  const bool pending =
      BotSessionRegistry::Instance().WithSession(key, [](BotSession& s) {
        const bool had =
            !s.pending_boot_victim.empty() &&
            (s.pending_boot_until_ms == 0 ||
             static_cast<int32_t>(s.pending_boot_until_ms - getMSTime()) > 0);
        s.pending_boot_victim.clear();
        s.pending_boot_reason.clear();
        s.pending_boot_until_ms = 0;
        return had;
      });
  if (!pending) {
    return {.error = "no kick vote to answer"};
  }
  WorldPacket p(CMSG_LFG_SET_BOOT_VOTE, 1);
  p << uint8_t(action.yes ? 1 : 0);
  p.rpos(0);
  session->HandleLfgSetBootVoteOpcode(p);
  return {.used = true,
          .info = action.yes ? "voted to kick" : "voted not to kick"};
}

ActionOutcome ExecuteMeetingStoneSummonAction(
    Player* bot,
    const MeetingStoneSummonAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  Group* group = bot->GetGroup();
  if (!group) {
    return {.error = "you are not in a group"};
  }
  GameObject* go = ObjectAccessor::GetGameObject(
      *bot, ParseSnapshotGuid(action.object_guid));
  if (!go || !go->IsInWorld() || !go->isSpawned() ||
      go->GetGoType() != GAMEOBJECT_TYPE_MEETINGSTONE) {
    return {.error = "meeting stone is gone"};
  }
  Player* target = ObjectAccessor::FindConnectedPlayer(
      ParseSnapshotGuid(action.target_guid));
  if (!target || target == bot || !target->IsInWorld() ||
      !target->IsInSameRaidWith(bot)) {
    return {.error = "group member is not available to summon"};
  }
  const GameObjectTemplate* info = go->GetGOInfo();
  const uint8_t min_level =
      info ? static_cast<uint8_t>(info->meetingstone.minLevel) : 0;
  if (bot->GetLevel() < min_level) {
    return {.error = "your level is too low for " + go->GetName()};
  }
  if (target->GetLevel() < min_level) {
    return {.error =
                target->GetName() + "'s level is too low for " + go->GetName()};
  }
  if (!IsInUseRange(bot, go)) {
    if (!WalkNextTo(bot, go)) {
      return {.error = "no path to " + go->GetName()};
    }
    return {};
  }
  if (bot->isMoving()) {
    bot->StopMoving();
  }
  ClearLastMovement(bot);
  bot->SetSelection(target->GetGUID());
  WorldPacket pkt(CMSG_GAMEOBJ_USE, 8);
  pkt << go->GetGUID();
  pkt.rpos(0);
  session->HandleGameObjectUseOpcode(pkt);
  if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
    ai->ResetActionDuration();
  }
  return {.used = true, .info = "summoning " + target->GetName()};
}

namespace {

// The brain's loot policy: defer briefly while a human in the roll hasn't
// voted yet, then Need a usable spec-appropriate upgrade of the bot's own
// armor type that doesn't bind on equip (never against a human who already
// Needed), else Disenchant (when eligible), else Greed, else Pass.
std::optional<LootRollChoice> PolicyRollVote(Player* bot,
                                             const Roll& roll,
                                             uint64_t roll_elapsed_ms) {
  const ItemTemplate* proto = sObjectMgr->GetItemTemplate(roll.itemid);
  LootRollPolicyInput in;
  in.roll_vote_mask = roll.rollVoteMask;
  in.roll_elapsed_ms = roll_elapsed_ms;
  if (proto) {
    in.bind_on_equip = proto->Bonding == BIND_WHEN_EQUIPPED;
    in.usable = bot->CanUseItem(proto) == EQUIP_ERR_OK;
    in.armor_type_matches = proto->Class != ITEM_CLASS_ARMOR ||
                            proto->InventoryType == INVTYPE_CLOAK ||
                            !ArmorTypeName(proto->SubClass) ||
                            proto->SubClass == BestArmorSubclass(bot);
    const LootStatProfile profile = BotLootStatProfile(bot);
    in.shield_disallowed_for_spec = proto->InventoryType == INVTYPE_SHIELD &&
                                    IsShieldDisallowedForProfile(profile);
    in.stat_fit =
        EvaluateStatFit(profile, bot->getClass(), ExtractItemStatFlags(proto));
    const std::vector<uint8_t> slots = SlotsFor(bot, proto->InventoryType);
    for (const uint8_t slot : slots) {
      const Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
      const ItemTemplate* wp = worn ? worn->GetTemplate() : nullptr;
      if (!wp || proto->ItemLevel > wp->ItemLevel) {
        in.has_upgrade_slot = true;
        break;
      }
    }
  }
  // Respect humans in the roll: defer while a connected human hasn't voted
  // yet, and never Need against a human who already Needed.
  for (const auto& [guid, vote] : roll.playerVote) {
    if (guid == bot->GetGUID()) {
      continue;
    }
    Player* p = ObjectAccessor::FindConnectedPlayer(guid);
    if (!p || !IsHumanPlayer(p)) {
      continue;
    }
    if (vote == NEED) {
      in.human_already_needed = true;
    } else if (vote == NOT_EMITED_YET) {
      in.human_pending_vote = true;
    }
  }
  return DecidePolicyRollVote(in);
}

}  // namespace

std::vector<std::string> AnswerPopupsByPolicy(Player* bot) {
  std::vector<std::string> answered;
  if (!bot || !bot->IsInWorld()) {
    return answered;
  }
  const bool on = BotSessionRegistry::Instance().InspectSession(
      bot->GetGUID().GetRawValue(),
      [](const BotSession* s) { return s && s->auto_popups; });
  if (!on) {
    return answered;
  }
  const auto note = [&](const ActionOutcome& out, std::string what) {
    if (out.error.empty()) {
      answered.push_back(std::move(what));
    }
  };
  if (!bot->IsAlive()) {
    if (bot->isResurrectRequested()) {
      note(ExecuteResurrectAction(
               bot, ResurrectAction{ResurrectAction::Mode::kAccept}),
           "accepted a resurrection");
    }
  } else {
    if (BotManager::HasPendingReadyCheck(bot)) {
      note(ExecuteReadyCheckAction(bot, ReadyCheckAction{true}),
           "answered the ready check: ready");
    }
    if (bot->GetSummonExpireTimer() > GameTime::GetGameTime().count()) {
      note(ExecutePopupAnswerAction(
               bot, PopupAnswerAction{PopupAnswerAction::Kind::kSummon, true}),
           "accepted a summon");
    }
    for (const PendingSharedQuest& sq :
         BotManager::GetPendingSharedQuests(bot)) {
      const Quest* q = sObjectMgr->GetQuestTemplate(sq.quest_id);
      const char* kind =
          sq.is_confirm ? "accepted escort quest " : "accepted shared quest ";
      note(ExecuteSharedQuestAction(bot, SharedQuestAction{sq.quest_id, true}),
           std::string(kind) +
               (q ? q->GetTitle() : std::to_string(sq.quest_id)));
    }
    const lfg::LfgState st = sLFGMgr->GetState(bot->GetGUID());
    if (st == lfg::LFG_STATE_ROLECHECK) {
      const std::vector<LfgRoleChoice> roles = SpecLfgRoles(bot);
      if (!roles.empty()) {
        const LfgRoleChoice* pick = &roles.front();
        if (BotManager::GetGroupPolicy(bot).not_tank &&
            pick->mask == lfg::PLAYER_ROLE_TANK) {
          for (const LfgRoleChoice& rc : roles) {
            if (rc.mask != lfg::PLAYER_ROLE_TANK) {
              pick = &rc;
              break;
            }
          }
        }
        note(ExecuteLfgAction(bot, LfgAction{LfgAction::Op::kRole, pick->mask}),
             std::string("confirmed the Dungeon Finder role: ") + pick->name);
      }
    } else if (st == lfg::LFG_STATE_PROPOSAL) {
      if (const uint32_t pid = BotManager::GetPendingLfgProposal(bot)) {
        note(ExecuteLfgAction(bot,
                              LfgAction{LfgAction::Op::kAcceptProposal, pid}),
             "accepted the Dungeon Finder proposal");
      }
    }
  }
  const uint64_t bot_key = bot->GetGUID().GetRawValue();
  if (Group* group = bot->GetGroup()) {
    const uint64_t now_ms = static_cast<uint64_t>(
        (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds());
    std::vector<RollKey> active_keys;
    std::vector<const Roll*> open_rolls;
    for (const Roll* roll : group->GetRolls()) {
      if (!roll) {
        continue;
      }
      auto it = roll->playerVote.find(bot->GetGUID());
      if (it == roll->playerVote.end() || it->second != NOT_EMITED_YET) {
        continue;
      }
      active_keys.push_back(
          RollKey{roll->itemGUID.GetRawValue(), roll->itemSlot});
      open_rolls.push_back(roll);
    }
    std::vector<uint64_t> elapsed_ms;
    elapsed_ms.reserve(open_rolls.size());
    BotSessionRegistry::Instance().WithSession(bot_key, [&](BotSession& s) {
      PrunePendingRolls(&s.pending_rolls_seen, active_keys);
      for (const RollKey& key : active_keys) {
        elapsed_ms.push_back(
            RecordAndGetRollElapsedMs(&s.pending_rolls_seen, key, now_ms));
      }
    });
    std::vector<LootRollAction> votes;
    for (size_t idx = 0; idx < open_rolls.size(); ++idx) {
      const Roll* roll = open_rolls[idx];
      const std::optional<LootRollChoice> choice =
          PolicyRollVote(bot, *roll, elapsed_ms[idx]);
      if (!choice.has_value()) {
        continue;  // Deferred while a human in the roll hasn't voted yet.
      }
      LootRollAction a;
      a.item_guid = base::StringPrintf(
          "0x%016llx",
          static_cast<unsigned long long>(roll->itemGUID.GetRawValue()));
      a.slot = roll->itemSlot;
      a.vote = static_cast<uint8_t>(*choice);
      const ItemTemplate* proto = sObjectMgr->GetItemTemplate(roll->itemid);
      a.label = std::string(LootRollChoiceVerb(*choice)) + " " +
                (proto ? proto->Name1 : "an item");
      votes.push_back(std::move(a));
    }
    for (const LootRollAction& a : votes) {
      note(ExecuteLootRollAction(bot, a), "rolled: " + a.label);
    }
  } else {
    BotSessionRegistry::Instance().WithSession(
        bot_key, [](BotSession& s) { s.pending_rolls_seen.clear(); });
  }
  return answered;
}

}  // namespace llm_brain
