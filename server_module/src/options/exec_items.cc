// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/exec_items.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "AiObjectContext.h"
#include "ByteBuffer.h"
#include "Creature.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Define.h"
#include "Event.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "base/strings/stringprintf.h"
#include "options/action_types.h"
#include "options/exec_shared.h"
#include "options/position_facts.h"
#include "snapshot/snapshot.h"
#include "snapshot/world_util.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr uint32_t kSpellCategoryFood = 11;
constexpr uint32_t kSpellCategoryDrink = 59;
constexpr uint32_t kSpellGraccusFruitcake = 25990;
constexpr uint32_t kSitHoldMs = 1500;

bool HasCategoryAura(Player* bot, uint32_t category) {
  for (const auto& [spell_id, app] : bot->GetAppliedAuras()) {
    (void)spell_id;
    if (!app || !app->GetBase()) {
      continue;
    }
    if (const SpellInfo* info = app->GetBase()->GetSpellInfo();
        info && info->GetCategory() == category) {
      return true;
    }
  }
  return false;
}

bool TryEatAndDrink(Player* bot, PlayerbotAI* ai) {
  bool has_food = false;
  bool has_drink = false;
  CheckBagFoodAndDrink(bot, &has_food, &has_drink);
  bool used = false;
  if (has_food && !bot->IsFullHealth() &&
      !HasCategoryAura(bot, kSpellCategoryFood) &&
      !bot->HasAura(kSpellGraccusFruitcake)) {
    used |= ai->DoSpecificAction("eat", Event(), /*silent=*/true);
  }
  if (has_drink && bot->GetMaxPower(POWER_MANA) > 0 &&
      bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA) &&
      !HasCategoryAura(bot, kSpellCategoryDrink)) {
    used |= ai->DoSpecificAction("drink", Event(), /*silent=*/true);
  }
  return used;
}

const char* TalentTabName(uint32_t tab_id) {
  switch (tab_id) {
    case TALENT_TREE_WARRIOR_ARMS:
      return "Arms";
    case TALENT_TREE_WARRIOR_FURY:
      return "Fury";
    case TALENT_TREE_WARRIOR_PROTECTION:
      return "Protection";
    case TALENT_TREE_PALADIN_HOLY:
      return "Holy";
    case TALENT_TREE_PALADIN_PROTECTION:
      return "Protection";
    case TALENT_TREE_PALADIN_RETRIBUTION:
      return "Retribution";
    case TALENT_TREE_HUNTER_BEAST_MASTERY:
      return "Beast Mastery";
    case TALENT_TREE_HUNTER_MARKSMANSHIP:
      return "Marksmanship";
    case TALENT_TREE_HUNTER_SURVIVAL:
      return "Survival";
    case TALENT_TREE_ROGUE_ASSASSINATION:
      return "Assassination";
    case TALENT_TREE_ROGUE_COMBAT:
      return "Combat";
    case TALENT_TREE_ROGUE_SUBTLETY:
      return "Subtlety";
    case TALENT_TREE_PRIEST_DISCIPLINE:
      return "Discipline";
    case TALENT_TREE_PRIEST_HOLY:
      return "Holy";
    case TALENT_TREE_PRIEST_SHADOW:
      return "Shadow";
    case TALENT_TREE_DEATH_KNIGHT_BLOOD:
      return "Blood";
    case TALENT_TREE_DEATH_KNIGHT_FROST:
      return "Frost";
    case TALENT_TREE_DEATH_KNIGHT_UNHOLY:
      return "Unholy";
    case TALENT_TREE_SHAMAN_ELEMENTAL:
      return "Elemental";
    case TALENT_TREE_SHAMAN_ENHANCEMENT:
      return "Enhancement";
    case TALENT_TREE_SHAMAN_RESTORATION:
      return "Restoration";
    case TALENT_TREE_MAGE_ARCANE:
      return "Arcane";
    case TALENT_TREE_MAGE_FIRE:
      return "Fire";
    case TALENT_TREE_MAGE_FROST:
      return "Frost";
    case TALENT_TREE_WARLOCK_AFFLICTION:
      return "Affliction";
    case TALENT_TREE_WARLOCK_DEMONOLOGY:
      return "Demonology";
    case TALENT_TREE_WARLOCK_DESTRUCTION:
      return "Destruction";
    case TALENT_TREE_DRUID_BALANCE:
      return "Balance";
    case TALENT_TREE_DRUID_FERAL_COMBAT:
      return "Feral Combat";
    case TALENT_TREE_DRUID_RESTORATION:
      return "Restoration";
    default:
      return nullptr;
  }
}

const TalentEntry* FindClassTalent(uint32_t class_mask,
                                   uint32_t tab_page,
                                   uint32_t row,
                                   uint32_t col) {
  for (uint32_t i = 0; i < sTalentStore.GetNumRows(); ++i) {
    const TalentEntry* t = sTalentStore.LookupEntry(i);
    if (!t || t->Row != row || t->Col != col) {
      continue;
    }
    const TalentTabEntry* tab = sTalentTabStore.LookupEntry(t->TalentTab);
    if (tab && (tab->ClassMask & class_mask) != 0 && tab->tabpage == tab_page) {
      return t;
    }
  }
  return nullptr;
}

const std::vector<std::vector<uint32>>* PremadeBuildForTree(uint8_t cls,
                                                            uint32_t tab_page) {
  if (cls >= MAX_CLASSES) {
    return nullptr;
  }
  for (uint32_t spec = 0; spec < MAX_SPECNO; ++spec) {
    if (sPlayerbotAIConfig.PremadeSpecName[cls][spec].empty()) {
      continue;
    }
    for (int level = MAX_LEVEL - 1; level >= 0; --level) {
      const std::vector<std::vector<uint32>>& order =
          sPlayerbotAIConfig.ParsedSpecLinkOrder[cls][spec][level];
      if (order.empty()) {
        continue;
      }
      if (!order.front().empty() && order.front()[0] == tab_page) {
        return &order;
      }
      break;
    }
  }
  return nullptr;
}

std::vector<uint8_t> TakeableLootSlots(Player* bot, Creature* creature) {
  PermissionTypes permission = NONE_PERMISSION;
  if (Group* group = creature->GetLootRecipientGroup()) {
    if (bot->GetGroup() == group) {
      switch (group->GetLootMethod()) {
        case MASTER_LOOT:
          permission = group->GetMasterLooterGuid() == bot->GetGUID()
                           ? MASTER_PERMISSION
                           : RESTRICTED_PERMISSION;
          break;
        case FREE_FOR_ALL:
          permission = ALL_PERMISSION;
          break;
        case ROUND_ROBIN:
          permission = ROUND_ROBIN_PERMISSION;
          break;
        default:
          permission = GROUP_PERMISSION;
          break;
      }
    }
  } else if (creature->GetLootRecipient() == bot) {
    permission = OWNER_PERMISSION;
  }
  ByteBuffer view;
  view << LootView(creature->loot, bot, permission);
  std::vector<uint8_t> slots;
  view.read_skip<uint32_t>();
  const uint8_t count = view.read<uint8_t>();
  for (uint8_t i = 0; i < count; ++i) {
    const auto slot = view.read<uint8_t>();
    view.read_skip(5 * sizeof(uint32_t));
    const auto type = view.read<uint8_t>();
    if (type == LOOT_SLOT_TYPE_ALLOW_LOOT || type == LOOT_SLOT_TYPE_OWNER) {
      slots.push_back(slot);
    }
  }
  return slots;
}

}  // namespace

bool HasFoodOrDrinkAura(Player* bot) {
  return bot->HasAura(kSpellGraccusFruitcake) ||
         HasCategoryAura(bot, kSpellCategoryFood) ||
         HasCategoryAura(bot, kSpellCategoryDrink);
}

void CheckBagFoodAndDrink(Player* bot, bool* has_food, bool* has_drink) {
  *has_food = false;
  *has_drink = false;
  for (const BagSlotItem& entry : BagItems(bot)) {
    const ItemTemplate* proto = entry.item->GetTemplate();
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE ||
        (proto->SubClass != ITEM_SUBCLASS_FOOD &&
         proto->SubClass != ITEM_SUBCLASS_CONSUMABLE) ||
        bot->CanUseItem(proto) != EQUIP_ERR_OK) {
      continue;
    }
    const uint32_t cat = proto->Spells[0].SpellCategory;
    if (cat == kSpellCategoryFood) {
      *has_food = true;
    } else if (cat == kSpellCategoryDrink) {
      *has_drink = true;
    }
  }
}

void ContinueRest(Player* bot) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai || bot->IsNonMeleeSpellCast(false) || bot->IsInCombat()) {
    return;
  }
  TryEatAndDrink(bot, ai);
  if (bot->getStandState() == UNIT_STAND_STATE_SIT &&
      !HasFoodOrDrinkAura(bot)) {
    ai->SetNextCheckDelay(kSitHoldMs);
  }
}

bool IsRestInProgress(Player* bot, bool* had_food_drink_aura) {
  if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() ||
      bot->IsInWater()) {
    return false;
  }
  const bool full_hp = bot->IsFullHealth();
  const bool full_mana =
      bot->GetMaxPower(POWER_MANA) == 0 ||
      bot->GetPower(POWER_MANA) >= bot->GetMaxPower(POWER_MANA);
  if (full_hp && full_mana) {
    if (bot->getStandState() == UNIT_STAND_STATE_SIT) {
      bot->SetStandState(UNIT_STAND_STATE_STAND);
    }
    return false;
  }
  const bool has_aura = HasFoodOrDrinkAura(bot);
  if (has_aura) {
    if (had_food_drink_aura) {
      *had_food_drink_aura = true;
    }
    return true;
  }
  if (had_food_drink_aura && *had_food_drink_aura) {
    if (bot->getStandState() == UNIT_STAND_STATE_SIT) {
      bot->SetStandState(UNIT_STAND_STATE_STAND);
    }
    return false;
  }
  return bot->getStandState() == UNIT_STAND_STATE_SIT;
}

void CollectTalentTrees(Player* bot,
                        std::vector<pb::OptionFacts_TalentTree>* out) {
  out->clear();
  if (!bot || !bot->IsAlive() || bot->IsInCombat()) {
    return;
  }
  const uint32_t free_points = bot->GetFreeTalentPoints();
  if (free_points == 0) {
    return;
  }
  const uint32_t class_mask = bot->getClassMask();
  struct TabInfo {
    uint32_t tab_id = 0;
    uint32_t tab_page = 0;
    std::string name;
    uint32_t spent = 0;
  };
  std::vector<TabInfo> tabs;
  for (uint32_t i = 0; i < sTalentTabStore.GetNumRows(); ++i) {
    const TalentTabEntry* entry = sTalentTabStore.LookupEntry(i);
    if (!entry || (entry->ClassMask & class_mask) == 0) {
      continue;
    }
    const char* name = TalentTabName(entry->TalentTabID);
    if (!name || !*name) {
      continue;
    }
    tabs.push_back({entry->TalentTabID, entry->tabpage, name, 0});
  }
  std::sort(tabs.begin(), tabs.end(), [](const TabInfo& a, const TabInfo& b) {
    return a.tab_page < b.tab_page;
  });
  const uint8_t active_spec = bot->GetActiveSpec();
  const PlayerTalentMap& talent_map = bot->GetTalentMap();
  for (uint32_t i = 0; i < sTalentStore.GetNumRows(); ++i) {
    const TalentEntry* talent = sTalentStore.LookupEntry(i);
    if (!talent) {
      continue;
    }
    TabInfo* owner = nullptr;
    for (TabInfo& t : tabs) {
      if (t.tab_id == talent->TalentTab) {
        owner = &t;
        break;
      }
    }
    if (!owner) {
      continue;
    }
    for (int rank = MAX_TALENT_RANK - 1; rank >= 0; --rank) {
      const uint32_t spell_id = talent->RankID[rank];
      if (!spell_id) {
        continue;
      }
      auto it = talent_map.find(spell_id);
      if (it != talent_map.end() && it->second &&
          it->second->State != PLAYERSPELL_REMOVED &&
          it->second->IsInSpec(active_spec)) {
        owner->spent += static_cast<uint32_t>(rank + 1);
        break;
      }
    }
  }
  for (const TabInfo& t : tabs) {
    out->push_back({t.tab_page, t.name, t.spent, free_points});
  }
}

void CollectUsableBagItems(Player* bot,
                           std::vector<pb::OptionFacts_Item>* out) {
  out->clear();
  if (!bot || bot->m_mover != bot) {
    return;
  }
  static constexpr size_t kMaxUsableBagItems = 32;
  for (const BagSlotItem& entry : BagItems(bot)) {
    if (out->size() >= kMaxUsableBagItems) {
      break;
    }
    const ItemTemplate* proto =
        entry.item ? entry.item->GetTemplate() : nullptr;
    if (!proto || proto->Name1.empty() ||
        proto->InventoryType != INVTYPE_NON_EQUIP ||
        bot->CanUseItem(entry.item) != EQUIP_ERR_OK) {
      continue;
    }
    uint32_t spell_id = 0;
    uint32_t spell_cat = 0;
    bool has_charges = true;
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
      if (proto->Spells[i].SpellId > 0 &&
          proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE) {
        if (proto->Spells[i].SpellCharges != 0 &&
            entry.item->GetSpellCharges(i) == 0) {
          has_charges = false;
          continue;
        }
        spell_id = static_cast<uint32_t>(proto->Spells[i].SpellId);
        spell_cat = proto->Spells[i].SpellCategory;
        has_charges = true;
        break;
      }
    }
    if (spell_id == 0 || !has_charges) {
      continue;
    }
    if (spell_cat == kSpellCategoryFood || spell_cat == kSpellCategoryDrink) {
      continue;
    }
    if (bot->HasSpellCooldown(spell_id)) {
      continue;
    }
    const SpellInfo* info = sSpellMgr->GetSpellInfo(spell_id);
    if (!info) {
      continue;
    }
    bool requires_item_target = false;
    for (uint8_t eff = 0; eff < MAX_SPELL_EFFECTS; ++eff) {
      const uint32_t e = info->Effects[eff].Effect;
      if (e == SPELL_EFFECT_ENCHANT_ITEM ||
          e == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY ||
          e == SPELL_EFFECT_ENCHANT_HELD_ITEM) {
        requires_item_target = true;
        break;
      }
    }
    if (requires_item_target) {
      continue;
    }
    if (bot->GetLastPotionId() &&
        (entry.item->IsPotion() || info->IsCooldownStartedOnEvent())) {
      continue;
    }
    bool out_of_combat_only = false;
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
      if (proto->Spells[i].SpellId > 0) {
        if (const SpellInfo* si = sSpellMgr->GetSpellInfo(
                static_cast<uint32_t>(proto->Spells[i].SpellId));
            si && si->HasAttribute(SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL)) {
          out_of_combat_only = true;
          break;
        }
      }
    }
    bool dup = false;
    for (const pb::OptionFacts_Item& existing : *out) {
      if (existing.entry == proto->ItemId) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    pb::OptionFacts_Item item;
    item.entry = proto->ItemId;
    item.name = proto->Name1;
    item.out_of_combat_only = out_of_combat_only;
    for (uint8_t i = 0; i < MAX_SPELL_EFFECTS; ++i) {
      const SpellEffectInfo& eff = info->Effects[i];
      if (eff.Effect == SPELL_EFFECT_TELEPORT_UNITS &&
          (eff.TargetA.GetTarget() == TARGET_DEST_HOME ||
           eff.TargetB.GetTarget() == TARGET_DEST_HOME)) {
        const std::string home = AreaName(bot->m_homebindAreaId);
        item.detail =
            home.empty() ? "teleports home" : "teleports home to " + home;
        break;
      }
    }
    out->push_back(std::move(item));
  }

  if (!bot->IsInCombat()) {
    static constexpr size_t kMaxEquipCandidates = 8;
    const auto quality_name = [](uint32_t q) -> const char* {
      switch (q) {
        case ITEM_QUALITY_NORMAL:
          return "common";
        case ITEM_QUALITY_UNCOMMON:
          return "uncommon";
        case ITEM_QUALITY_RARE:
          return "rare";
        case ITEM_QUALITY_EPIC:
          return "epic";
        case ITEM_QUALITY_LEGENDARY:
          return "legendary";
        case ITEM_QUALITY_HEIRLOOM:
          return "heirloom";
        default:
          return "";
      }
    };
    const auto armor_subclass_name = [](uint32_t sub) -> const char* {
      switch (sub) {
        case ITEM_SUBCLASS_ARMOR_CLOTH:
          return "Cloth";
        case ITEM_SUBCLASS_ARMOR_LEATHER:
          return "Leather";
        case ITEM_SUBCLASS_ARMOR_MAIL:
          return "Mail";
        case ITEM_SUBCLASS_ARMOR_PLATE:
          return "Plate";
        case ITEM_SUBCLASS_ARMOR_SHIELD:
          return "Shield";
        default:
          return "";
      }
    };
    const auto stat_short_name = [](uint32_t st) -> const char* {
      switch (st) {
        case ITEM_MOD_AGILITY:
          return "Agi";
        case ITEM_MOD_STRENGTH:
          return "Str";
        case ITEM_MOD_INTELLECT:
          return "Int";
        case ITEM_MOD_SPIRIT:
          return "Spi";
        case ITEM_MOD_STAMINA:
          return "Sta";
        case ITEM_MOD_SPELL_POWER:
          return "SP";
        case ITEM_MOD_ATTACK_POWER:
          return "AP";
        case ITEM_MOD_CRIT_RATING:
          return "Crit";
        case ITEM_MOD_HIT_RATING:
          return "Hit";
        case ITEM_MOD_HASTE_RATING:
          return "Haste";
        default:
          return "";
      }
    };
    const auto slot_name = [](uint32_t inv) -> const char* {
      switch (inv) {
        case INVTYPE_HEAD:
          return "Head";
        case INVTYPE_NECK:
          return "Neck";
        case INVTYPE_SHOULDERS:
          return "Shoulders";
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
          return "Chest";
        case INVTYPE_WAIST:
          return "Waist";
        case INVTYPE_LEGS:
          return "Legs";
        case INVTYPE_FEET:
          return "Feet";
        case INVTYPE_WRISTS:
          return "Wrists";
        case INVTYPE_HANDS:
          return "Hands";
        case INVTYPE_FINGER:
          return "Finger";
        case INVTYPE_TRINKET:
          return "Trinket";
        case INVTYPE_CLOAK:
          return "Back";
        case INVTYPE_WEAPON:
          return "1H";
        case INVTYPE_SHIELD:
          return "Off Hand";
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:
          return "Ranged";
        case INVTYPE_2HWEAPON:
          return "2H";
        case INVTYPE_WEAPONMAINHAND:
          return "Main Hand";
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_HOLDABLE:
          return "Off Hand";
        case INVTYPE_RELIC:
          return "Relic";
        default:
          return "";
      }
    };
    const auto weapon_subclass_name = [](uint32_t sub) -> const char* {
      switch (sub) {
        case ITEM_SUBCLASS_WEAPON_AXE:
        case ITEM_SUBCLASS_WEAPON_AXE2:
          return "Axe";
        case ITEM_SUBCLASS_WEAPON_BOW:
          return "Bow";
        case ITEM_SUBCLASS_WEAPON_GUN:
          return "Gun";
        case ITEM_SUBCLASS_WEAPON_MACE:
        case ITEM_SUBCLASS_WEAPON_MACE2:
          return "Mace";
        case ITEM_SUBCLASS_WEAPON_POLEARM:
          return "Polearm";
        case ITEM_SUBCLASS_WEAPON_SWORD:
        case ITEM_SUBCLASS_WEAPON_SWORD2:
          return "Sword";
        case ITEM_SUBCLASS_WEAPON_STAFF:
          return "Staff";
        case ITEM_SUBCLASS_WEAPON_FIST:
          return "Fist";
        case ITEM_SUBCLASS_WEAPON_DAGGER:
          return "Dagger";
        case ITEM_SUBCLASS_WEAPON_THROWN:
          return "Thrown";
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
          return "Crossbow";
        case ITEM_SUBCLASS_WEAPON_WAND:
          return "Wand";
        default:
          return "Weapon";
      }
    };
    struct RankedEquip {
      int32_t ilvl_delta = 0;
      pb::OptionFacts_Item item;
    };
    std::vector<RankedEquip> candidates;
    for (const BagSlotItem& entry : BagItems(bot)) {
      Item* bag_item = entry.item;
      const ItemTemplate* proto = bag_item ? bag_item->GetTemplate() : nullptr;
      if (!proto || proto->Name1.empty() ||
          proto->Quality <= ITEM_QUALITY_POOR ||
          proto->InventoryType == INVTYPE_NON_EQUIP ||
          (proto->Class != ITEM_CLASS_ARMOR &&
           proto->Class != ITEM_CLASS_WEAPON)) {
        continue;
      }
      uint16_t dest = 0;
      if (!PickEquipSlot(bot, bag_item, proto, &dest)) {
        continue;
      }
      const uint8_t equip_slot = static_cast<uint8_t>(dest & 0xFF);
      const Item* equipped =
          bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equip_slot);
      const ItemTemplate* equipped_proto =
          equipped ? equipped->GetTemplate() : nullptr;
      const uint32_t equipped_ilvl =
          equipped_proto ? equipped_proto->ItemLevel : 0;
      bool dup = false;
      for (const RankedEquip& existing : candidates) {
        if (existing.item.entry == proto->ItemId) {
          dup = true;
          break;
        }
      }
      if (dup) {
        continue;
      }
      std::string detail;
      if (const char* sl = slot_name(proto->InventoryType); *sl) {
        detail += sl;
      }
      if (const char* qn = quality_name(proto->Quality); *qn) {
        if (!detail.empty())
          detail += " ";
        detail += qn;
      }
      if (proto->Class == ITEM_CLASS_ARMOR) {
        if (const char* sn = armor_subclass_name(proto->SubClass); *sn) {
          if (!detail.empty())
            detail += " ";
          detail += sn;
        }
      } else if (proto->Class == ITEM_CLASS_WEAPON) {
        if (!detail.empty())
          detail += " ";
        detail += weapon_subclass_name(proto->SubClass);
      }
      if (!detail.empty())
        detail += ", ";
      if (equipped_proto) {
        detail += "ilvl " + std::to_string(proto->ItemLevel) + " vs ";
        if (!equipped_proto->Name1.empty()) {
          detail += equipped_proto->Name1 + " ";
        }
        detail += "ilvl " + std::to_string(equipped_ilvl);
      } else {
        detail += "ilvl " + std::to_string(proto->ItemLevel) + ", empty slot";
      }
      if (proto->Class == ITEM_CLASS_WEAPON && proto->Delay > 0) {
        float avg_dmg = 0.0f;
        for (int i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i) {
          avg_dmg +=
              (proto->Damage[i].DamageMin + proto->Damage[i].DamageMax) * 0.5f;
        }
        const float dps =
            avg_dmg / (static_cast<float>(proto->Delay) / 1000.0f);
        if (dps >= 0.5f) {
          detail += ", " + std::to_string(static_cast<int>(std::lround(dps))) +
                    " DPS";
        }
      } else if (proto->Armor > 0) {
        detail += ", " + std::to_string(proto->Armor) + " Armor";
      }
      size_t stats_added = 0;
      for (uint32_t s = 0; s < proto->StatsCount && s < MAX_ITEM_PROTO_STATS;
           ++s) {
        if (proto->ItemStat[s].ItemStatValue <= 0) {
          continue;
        }
        const char* sname = stat_short_name(proto->ItemStat[s].ItemStatType);
        if (!*sname) {
          continue;
        }
        detail += ", +" + std::to_string(proto->ItemStat[s].ItemStatValue) +
                  " " + sname;
        if (++stats_added >= 3) {
          break;
        }
      }
      if (proto->Bonding == BIND_WHEN_EQUIPPED && !bag_item->IsSoulBound()) {
        detail += ", BoE";
      }
      pb::OptionFacts_Item item;
      item.entry = proto->ItemId;
      item.name = proto->Name1;
      item.out_of_combat_only = true;
      item.equippable = true;
      item.detail = std::move(detail);
      candidates.push_back({static_cast<int32_t>(proto->ItemLevel) -
                                static_cast<int32_t>(equipped_ilvl),
                            std::move(item)});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const RankedEquip& a, const RankedEquip& b) {
                       return a.ilvl_delta > b.ilvl_delta;
                     });
    size_t equip_added = 0;
    for (RankedEquip& cand : candidates) {
      if (equip_added >= kMaxEquipCandidates ||
          out->size() >= kMaxUsableBagItems) {
        break;
      }
      out->push_back(std::move(cand.item));
      ++equip_added;
    }
  }
}

bool HasTakeableLoot(Player* bot, Creature* creature) {
  if (!bot || !creature || !creature->IsInWorld()) {
    return false;
  }
  const std::vector<uint8_t> slots = TakeableLootSlots(bot, creature);
  size_t storable_items = 0;
  for (uint8_t slot : slots) {
    const LootItem* item = creature->loot.LootItemInSlot(slot, bot);
    if (!item || item->is_looted) {
      continue;
    }
    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item->itemid,
                             item->count) == EQUIP_ERR_OK) {
      ++storable_items;
    }
  }
  return HasCorpseTakeableLoot(
      creature->isDead(), creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE),
      bot->isAllowedToLoot(creature), creature->loot.gold, storable_items);
}

std::string LootCorpseNow(Player* bot, Creature* creature) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return "not a bot";
  }
  if (!creature || !creature->IsInWorld() || !creature->isDead()) {
    return "nothing to loot";
  }
  if (!bot->isAllowedToLoot(creature) ||
      !creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)) {
    return "nothing to loot on " + creature->GetName();
  }
  if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE)) {
    return base::StringPrintf("%s is %.0f yd away", creature->GetName().c_str(),
                              bot->GetDistance(creature));
  }
  ClearLastMovement(bot);
  if (bot->isMoving()) {
    bot->StopMoving();
  }
  if (AiObjectContext* ctx = ai->GetAiObjectContext()) {
    if (auto* avail = ctx->GetValue<LootObjectStack*>("available loot");
        avail && avail->Get()) {
      avail->Get()->Remove(creature->GetGUID());
    }
    if (auto* lt = ctx->GetValue<LootObject>("loot target");
        lt && lt->Get().guid == creature->GetGUID()) {
      lt->Set(LootObject());
    }
  }
  const ObjectGuid guid = creature->GetGUID();
  if (const ObjectGuid prev = bot->GetLootGUID(); prev && prev != guid) {
    bot->GetSession()->DoLootRelease(prev);
  }
  WorldPacket open_pkt(CMSG_LOOT, 8);
  open_pkt << guid;
  open_pkt.rpos(0);
  bot->GetSession()->HandleLootOpcode(open_pkt);
  if (bot->GetLootGUID() != guid) {
    return "nothing to loot on " + creature->GetName();
  }
  if (creature->loot.gold > 0) {
    WorldPacket money_pkt(CMSG_LOOT_MONEY, 0);
    bot->GetSession()->HandleLootMoneyOpcode(money_pkt);
  }
  for (const uint8_t slot : TakeableLootSlots(bot, creature)) {
    WorldPacket item_pkt(CMSG_AUTOSTORE_LOOT_ITEM, 1);
    item_pkt << static_cast<uint8_t>(slot);
    item_pkt.rpos(0);
    bot->GetSession()->HandleAutostoreLootItemOpcode(item_pkt);
  }
  std::string no_room;
  std::string refused;
  for (const uint8_t slot : TakeableLootSlots(bot, creature)) {
    const LootItem* left = creature->loot.LootItemInSlot(slot, bot);
    if (left == nullptr) {
      continue;
    }
    const ItemTemplate* proto = sObjectMgr->GetItemTemplate(left->itemid);
    const std::string item_name = proto ? proto->Name1 : "an item";
    ItemPosCountVec dest;
    switch (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, left->itemid,
                                 left->count)) {
      case EQUIP_ERR_OK:
        refused = item_name;
        break;
      case EQUIP_ERR_BAG_FULL:
      case EQUIP_ERR_INVENTORY_FULL:
        no_room = item_name;
        break;
      default:
        refused = item_name;
        break;
    }
  }
  WorldPacket release_pkt(CMSG_LOOT_RELEASE, 8);
  release_pkt << guid;
  release_pkt.rpos(0);
  bot->GetSession()->HandleLootReleaseOpcode(release_pkt);
  ai->ResetActionDuration();
  if (!no_room.empty()) {
    return "Inventory is full";
  }
  if (!refused.empty()) {
    return "You can't carry " + refused;
  }
  return "";
}

ActionOutcome ExecuteLootAction(Player* bot, const LootAction& action) {
  Creature* creature =
      ObjectAccessor::GetCreature(*bot, ParseSnapshotGuid(action.guid));
  if (!creature || !creature->IsInWorld()) {
    return {.error = action.name + " is gone"};
  }
  if (!creature->isDead()) {
    return {.error = action.name + " isn't dead"};
  }
  LootObject loot(bot, creature->GetGUID());
  if (loot.IsEmpty() || !bot->isAllowedToLoot(creature) ||
      !creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)) {
    return {.error = "nothing to loot on " + action.name};
  }
  if (IsInLootRange(bot, creature)) {
    std::string error = LootCorpseNow(bot, creature);
    const bool used = error.empty();
    return {.error = std::move(error), .used = used};
  }
  if (!WalkNextTo(bot, creature)) {
    return {.error = "no path to " + action.name};
  }
  return {};
}

ActionOutcome ExecuteObjectAction(Player* bot,
                                  const std::string& guid,
                                  const std::string& name) {
  GameObject* go = ObjectAccessor::GetGameObject(*bot, ParseSnapshotGuid(guid));
  if (!go || !go->IsInWorld() || !go->isSpawned()) {
    return {.error = name + " is gone"};
  }
  if (IsInUseRange(bot, go)) {
    std::string error = UseObjectNow(bot, go);
    const bool used = error.empty();
    return {.error = std::move(error), .used = used};
  }
  if (!WalkNextTo(bot, go)) {
    return {.error = "no path to " + name};
  }
  return {};
}

ActionOutcome ExecuteRestAction(Player* bot) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {.error = "not a bot"};
  }
  if (bot->IsInCombat()) {
    return {.error = "you are in combat"};
  }
  if (bot->IsMounted()) {
    return {.error = "you are mounted"};
  }
  if (bot->IsInWater()) {
    return {.error = "you are swimming"};
  }
  ClearLastMovement(bot);
  if (bot->isMoving()) {
    bot->StopMoving();
  }
  const bool used_consumable = TryEatAndDrink(bot, ai);
  if (!used_consumable) {
    bot->SetStandState(UNIT_STAND_STATE_SIT);
    ai->SetNextCheckDelay(kSitHoldMs);
  }
  return {};
}

ItemActionOutcome ExecuteUseBagItemAction(Player* bot,
                                          const UseBagItemAction& action) {
  WorldSession* session = bot->GetSession();
  if (!session) {
    return {.error = "no session"};
  }
  if (action.entry == 0) {
    return {.error = "invalid item id"};
  }
  const BagSlotItem* found = nullptr;
  uint32_t item_spell_id = 0;
  int spell_slot = -1;
  std::vector<BagSlotItem> items = BagItems(bot);
  for (const BagSlotItem& slot_item : items) {
    if (!slot_item.item || slot_item.item->GetEntry() != action.entry) {
      continue;
    }
    const ItemTemplate* p = slot_item.item->GetTemplate();
    if (!p || p->InventoryType != INVTYPE_NON_EQUIP) {
      continue;
    }
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
      if (p->Spells[i].SpellId > 0 &&
          p->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE) {
        if (p->Spells[i].SpellCharges != 0 &&
            slot_item.item->GetSpellCharges(i) == 0) {
          continue;
        }
        found = &slot_item;
        item_spell_id = static_cast<uint32_t>(p->Spells[i].SpellId);
        spell_slot = i;
        break;
      }
    }
    if (found) {
      break;
    }
  }
  if (!found || item_spell_id == 0) {
    return {.error = "can't use " + action.name + " now"};
  }
  Item* item = found->item;
  const ItemTemplate* proto = item->GetTemplate();
  if (!proto || bot->CanUseItem(item) != EQUIP_ERR_OK) {
    return {.error = "can't use " + action.name + " now"};
  }
  if (bot->HasSpellCooldown(item_spell_id)) {
    return {.error = action.name + " is on cooldown"};
  }
  if (bot->m_mover != bot) {
    return {.error = "can't use " + action.name + " now"};
  }
  const SpellInfo* item_spell = sSpellMgr->GetSpellInfo(item_spell_id);
  if (!item_spell) {
    return {.error = "can't use " + action.name + " now"};
  }
  if (bot->IsInCombat()) {
    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
      if (proto->Spells[i].SpellId > 0) {
        if (const SpellInfo* si = sSpellMgr->GetSpellInfo(
                static_cast<uint32_t>(proto->Spells[i].SpellId));
            si && si->HasAttribute(SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL)) {
          return {.error = "can't use " + action.name + " in combat"};
        }
      }
    }
  }
  if (item_spell->CalcCastTime() > 0) {
    bot->StopMoving();
  }
  Unit* item_target = bot->GetSelectedUnit();
  if (item_spell->IsPositive() && item_target && item_target != bot &&
      !bot->IsFriendlyTo(item_target)) {
    item_target = bot;
  }
  const ObjectGuid item_guid = item->GetGUID();
  const uint32_t count_before = bot->GetItemCount(proto->ItemId);
  const int32_t charges_before =
      spell_slot >= 0 ? item->GetSpellCharges(spell_slot) : 0;
  Unit* aura_unit = item_target ? item_target : bot;
  const ObjectGuid aura_unit_guid = aura_unit->GetGUID();
  bool had_aura_before = false;
  int32_t aura_dur_before = -1;
  if (const Aura* aura_before = aura_unit->GetAura(item_spell_id)) {
    had_aura_before = true;
    aura_dur_before = aura_before->GetDuration();
  }
  const uint32_t last_potion_before = bot->GetLastPotionId();
  const bool gcd_before =
      bot->GetGlobalCooldownMgr().HasGlobalCooldown(item_spell);
  const size_t queue_before = QueuedSpellCount(bot, item_spell_id);
  const void* vehicle_before = bot->GetVehicle();
  WorldPacket p(CMSG_USE_ITEM);
  p << found->bag << found->slot << static_cast<uint8_t>(0) << item_spell_id
    << item_guid << static_cast<uint32_t>(0) << static_cast<uint8_t>(0);
  SpellCastTargets targets;
  if ((item_spell->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION) != 0) {
    targets.SetDst(*(item_target ? item_target : bot));
  } else if (item_target) {
    targets.SetUnitTarget(item_target);
  }
  targets.Write(p);
  p.rpos(0);
  session->HandleUseItemOpcode(p);
  const bool count_dropped = bot->GetItemCount(proto->ItemId) < count_before;
  bool charges_dropped = false;
  if (!count_dropped && spell_slot >= 0 &&
      proto->Spells[spell_slot].SpellCharges != 0) {
    if (Item* post_item = bot->GetItemByPos(found->bag, found->slot);
        post_item && post_item->GetGUID() == item_guid) {
      charges_dropped =
          post_item->GetSpellCharges(spell_slot) != charges_before;
    }
  }
  const bool cast_started = IsPreparingOrChannelingSpell(bot, item_spell_id);
  const bool queued_now = QueuedSpellCount(bot, item_spell_id) > queue_before;
  const bool cd_started = bot->HasSpellCooldown(item_spell_id);
  const bool gcd_started =
      !gcd_before && bot->GetGlobalCooldownMgr().HasGlobalCooldown(item_spell);
  Unit* post_aura_unit = (aura_unit_guid == bot->GetGUID())
                             ? static_cast<Unit*>(bot)
                             : ObjectAccessor::GetUnit(*bot, aura_unit_guid);
  const Aura* aura_after =
      post_aura_unit ? post_aura_unit->GetAura(item_spell_id) : nullptr;
  const bool aura_applied =
      aura_after != nullptr &&
      (!had_aura_before || aura_after->GetDuration() > aura_dur_before);
  const bool potion_recorded = last_potion_before != proto->ItemId &&
                               bot->GetLastPotionId() == proto->ItemId;
  const bool vehicle_changed = bot->GetVehicle() != vehicle_before;
  if (!count_dropped && !charges_dropped && !cast_started && !queued_now &&
      !cd_started && !gcd_started && !aura_applied && !potion_recorded &&
      !vehicle_changed) {
    return {.error = "can't use " + action.name + " now"};
  }
  const bool still_pending = cast_started || queued_now;
  return {.used = !still_pending,
          .info = still_pending ? "" : "used " + action.name,
          .pending_spell_id = still_pending ? item_spell_id : 0};
}

ActionOutcome ExecuteEquipItemAction(Player* bot,
                                     const EquipItemAction& action) {
  if (bot->IsInCombat()) {
    return {.error = "you are in combat"};
  }
  if (action.entry == 0) {
    return {.error = "invalid item id"};
  }
  for (const BagSlotItem& slot_item : BagItems(bot)) {
    if (!slot_item.item || slot_item.item->GetEntry() != action.entry) {
      continue;
    }
    return EquipBagItem(bot, slot_item.item, slot_item.bag, slot_item.slot,
                        action.name);
  }
  return {.error = action.name + " is not in your bags"};
}

ActionOutcome ExecuteSpendTalentsAction(Player* bot,
                                        const SpendTalentsAction& action) {
  if (bot->IsInCombat()) {
    return {.error = "cannot spend talents in combat"};
  }
  const uint32_t before = bot->GetFreeTalentPoints();
  if (before == 0) {
    return {.error = "no unspent talent points"};
  }
  if (action.tab_page > 2) {
    return {.error = "invalid talent tree"};
  }
  const uint32_t class_mask = bot->getClassMask();
  uint32_t target_tab_id = 0;
  for (uint32_t i = 0; i < sTalentTabStore.GetNumRows(); ++i) {
    const TalentTabEntry* entry = sTalentTabStore.LookupEntry(i);
    if (entry && (entry->ClassMask & class_mask) != 0 &&
        entry->tabpage == action.tab_page) {
      target_tab_id = entry->TalentTabID;
      break;
    }
  }
  if (target_tab_id == 0) {
    return {.error = "invalid talent tree"};
  }
  std::vector<const TalentEntry*> tab_talents;
  for (uint32_t i = 0; i < sTalentStore.GetNumRows(); ++i) {
    const TalentEntry* t = sTalentStore.LookupEntry(i);
    if (t && t->TalentTab == target_tab_id) {
      tab_talents.push_back(t);
    }
  }
  std::sort(tab_talents.begin(), tab_talents.end(),
            [](const TalentEntry* a, const TalentEntry* b) {
              if (a->Row != b->Row)
                return a->Row < b->Row;
              if (a->Col != b->Col)
                return a->Col < b->Col;
              return a->TalentID < b->TalentID;
            });
  const uint8_t active_spec = bot->GetActiveSpec();
  auto learned_rank = [&](const TalentEntry* t) -> int {
    const PlayerTalentMap& tmap = bot->GetTalentMap();
    for (int r = MAX_TALENT_RANK - 1; r >= 0; --r) {
      const uint32_t sid = t->RankID[r];
      if (!sid)
        continue;
      auto it = tmap.find(sid);
      if (it != tmap.end() && it->second &&
          it->second->State != PLAYERSPELL_REMOVED &&
          it->second->IsInSpec(active_spec)) {
        return r;
      }
    }
    return -1;
  };
  uint32_t spent_in_tab = 0;
  for (const TalentEntry* t : tab_talents) {
    if (const int r = learned_rank(t); r >= 0) {
      spent_in_tab += static_cast<uint32_t>(r + 1);
    }
  }
  if (const std::vector<std::vector<uint32>>* build =
          PremadeBuildForTree(bot->getClass(), action.tab_page)) {
    for (const std::vector<uint32>& step : *build) {
      if (bot->GetFreeTalentPoints() == 0)
        break;
      if (step.size() < 4)
        continue;
      const TalentEntry* t =
          FindClassTalent(class_mask, step[0], step[1], step[2]);
      if (!t)
        continue;
      for (int next = learned_rank(t) + 1;
           next < static_cast<int>(step[3]) && next < MAX_TALENT_RANK &&
           t->RankID[next] != 0 && bot->GetFreeTalentPoints() > 0;
           ++next) {
        const uint32_t free_before = bot->GetFreeTalentPoints();
        bot->LearnTalent(t->TalentID, static_cast<uint32_t>(next));
        if (bot->GetFreeTalentPoints() >= free_before)
          break;
      }
    }
    spent_in_tab = 0;
    for (const TalentEntry* t : tab_talents) {
      if (const int r = learned_rank(t); r >= 0) {
        spent_in_tab += static_cast<uint32_t>(r + 1);
      }
    }
  }
  while (bot->GetFreeTalentPoints() > 0) {
    bool progressed = false;
    for (const TalentEntry* t : tab_talents) {
      if (t->Row * 5 > spent_in_tab)
        continue;
      if (t->DependsOn != 0) {
        const TalentEntry* prereq = sTalentStore.LookupEntry(t->DependsOn);
        if (!prereq ||
            learned_rank(prereq) < static_cast<int>(t->DependsOnRank)) {
          continue;
        }
      }
      const int next = learned_rank(t) + 1;
      if (next >= MAX_TALENT_RANK || t->RankID[next] == 0)
        continue;
      const uint32_t free_before = bot->GetFreeTalentPoints();
      bot->LearnTalent(t->TalentID, static_cast<uint32_t>(next));
      if (bot->GetFreeTalentPoints() < free_before) {
        ++spent_in_tab;
        progressed = true;
        break;
      }
    }
    if (!progressed)
      break;
  }
  const uint32_t after = bot->GetFreeTalentPoints();
  if (after >= before) {
    return {.error = "no learnable talents in " + action.tree_name};
  }
  bot->SendTalentsInfoData(false);
  return {.used = true,
          .info = "spent " + std::to_string(before - after) +
                  " talent point(s) in " + action.tree_name};
}

}  // namespace llm_brain
