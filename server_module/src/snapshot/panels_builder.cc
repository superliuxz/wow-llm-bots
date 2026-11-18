// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/panels_builder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Bag.h"
#include "Config.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "G3D/Quat.h"  // IWYU pragma: keep (GameObjectData.h needs it complete)
#include "GameObjectData.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "Unit.h"
#include "UpdateFields.h"
#include "World.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "proto/snapshot.h"
#include "snapshot/spell_text.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr uint32_t kHearthstoneItem = 6948;
constexpr uint32_t kHearthstoneSpell = 8690;

std::string AreaName(uint32_t area_id) {
  const AreaTableEntry* area = sAreaTableStore.LookupEntry(area_id);
  if (!area) {
    return "";
  }
  const char* name = area->area_name[sWorld->GetDefaultDbcLocale()];
  return name ? name : "";
}

// "1g 2s 3c", as the client's money text.
std::string MoneyText(uint32_t copper) {
  std::string out;
  if (copper >= 10000) {
    out += base::StringPrintf("%ug ", copper / 10000);
  }
  if (copper % 10000 >= 100) {
    out += base::StringPrintf("%us ", copper % 10000 / 100);
  }
  if (copper % 100 != 0 || out.empty()) {
    out += base::StringPrintf("%uc ", copper % 100);
  }
  out.pop_back();
  return out;
}

pb::Item::Quality ToQuality(uint32_t quality) {
  switch (quality) {
    case ITEM_QUALITY_POOR:
      return pb::Item::POOR;
    case ITEM_QUALITY_NORMAL:
      return pb::Item::COMMON;
    case ITEM_QUALITY_UNCOMMON:
      return pb::Item::UNCOMMON;
    case ITEM_QUALITY_RARE:
      return pb::Item::RARE_QUALITY;
    case ITEM_QUALITY_EPIC:
      return pb::Item::EPIC;
    case ITEM_QUALITY_LEGENDARY:
      return pb::Item::LEGENDARY;
    case ITEM_QUALITY_ARTIFACT:
      return pb::Item::ARTIFACT;
    case ITEM_QUALITY_HEIRLOOM:
      return pb::Item::HEIRLOOM;
    default:
      return pb::Item::QUALITY_UNSPECIFIED;
  }
}

pb::Item::Kind ToKind(const ItemTemplate* p) {
  if (p->Quality == ITEM_QUALITY_POOR) {
    return pb::Item::JUNK;
  }
  switch (p->Class) {
    case ITEM_CLASS_WEAPON:
    case ITEM_CLASS_ARMOR:
      return pb::Item::EQUIPMENT;
    case ITEM_CLASS_CONSUMABLE:
      return pb::Item::CONSUMABLE;
    case ITEM_CLASS_CONTAINER:
    case ITEM_CLASS_QUIVER:
      return pb::Item::CONTAINER;
    case ITEM_CLASS_GEM:
      return pb::Item::GEM;
    case ITEM_CLASS_REAGENT:
      return pb::Item::REAGENT;
    case ITEM_CLASS_PROJECTILE:
      return pb::Item::PROJECTILE;
    case ITEM_CLASS_TRADE_GOODS:
      return pb::Item::TRADE_GOOD;
    case ITEM_CLASS_RECIPE:
      return pb::Item::RECIPE;
    case ITEM_CLASS_QUEST:
      return pb::Item::QUEST;
    case ITEM_CLASS_KEY:
      return pb::Item::KEY;
    case ITEM_CLASS_GLYPH:
      return pb::Item::GLYPH;
    default:
      return pb::Item::OTHER;
  }
}

// The equipment slot an item of `inventory_type` goes in.
pb::EquipmentPart::Slot SlotOfType(uint32_t inventory_type) {
  switch (inventory_type) {
    case INVTYPE_HEAD:
      return pb::EquipmentPart::HEAD;
    case INVTYPE_NECK:
      return pb::EquipmentPart::NECK;
    case INVTYPE_SHOULDERS:
      return pb::EquipmentPart::SHOULDER;
    case INVTYPE_BODY:
      return pb::EquipmentPart::SHIRT;
    case INVTYPE_CHEST:
    case INVTYPE_ROBE:
      return pb::EquipmentPart::CHEST;
    case INVTYPE_WAIST:
      return pb::EquipmentPart::WAIST;
    case INVTYPE_LEGS:
      return pb::EquipmentPart::LEGS;
    case INVTYPE_FEET:
      return pb::EquipmentPart::FEET;
    case INVTYPE_WRISTS:
      return pb::EquipmentPart::WRIST;
    case INVTYPE_HANDS:
      return pb::EquipmentPart::HANDS;
    case INVTYPE_FINGER:
      return pb::EquipmentPart::FINGER1;
    case INVTYPE_TRINKET:
      return pb::EquipmentPart::TRINKET1;
    case INVTYPE_CLOAK:
      return pb::EquipmentPart::BACK;
    case INVTYPE_WEAPON:
    case INVTYPE_2HWEAPON:
    case INVTYPE_WEAPONMAINHAND:
      return pb::EquipmentPart::MAIN_HAND;
    case INVTYPE_SHIELD:
    case INVTYPE_WEAPONOFFHAND:
    case INVTYPE_HOLDABLE:
      return pb::EquipmentPart::OFF_HAND;
    case INVTYPE_RANGED:
    case INVTYPE_THROWN:
    case INVTYPE_RANGEDRIGHT:
    case INVTYPE_RELIC:
      return pb::EquipmentPart::RANGED;
    case INVTYPE_TABARD:
      return pb::EquipmentPart::TABARD;
    case INVTYPE_BAG:
    case INVTYPE_QUIVER:
      return pb::EquipmentPart::BAG;
    default:
      return pb::EquipmentPart::SLOT_UNSPECIFIED;
  }
}

pb::EquipmentPart::Slot SlotOfEquipmentPos(uint8_t slot) {
  static constexpr pb::EquipmentPart::Slot kSlots[] = {
      pb::EquipmentPart::HEAD,     pb::EquipmentPart::NECK,
      pb::EquipmentPart::SHOULDER, pb::EquipmentPart::SHIRT,
      pb::EquipmentPart::CHEST,    pb::EquipmentPart::WAIST,
      pb::EquipmentPart::LEGS,     pb::EquipmentPart::FEET,
      pb::EquipmentPart::WRIST,    pb::EquipmentPart::HANDS,
      pb::EquipmentPart::FINGER1,  pb::EquipmentPart::FINGER2,
      pb::EquipmentPart::TRINKET1, pb::EquipmentPart::TRINKET2,
      pb::EquipmentPart::BACK,     pb::EquipmentPart::MAIN_HAND,
      pb::EquipmentPart::OFF_HAND, pb::EquipmentPart::RANGED,
      pb::EquipmentPart::TABARD};
  return slot < std::size(kSlots) ? kSlots[slot]
                                  : pb::EquipmentPart::SLOT_UNSPECIFIED;
}

// The client's slot word ("Chest") for an inventory type.
const char* SlotWord(uint32_t inventory_type) {
  switch (inventory_type) {
    case INVTYPE_HEAD:
      return "Head";
    case INVTYPE_NECK:
      return "Neck";
    case INVTYPE_SHOULDERS:
      return "Shoulder";
    case INVTYPE_BODY:
      return "Shirt";
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
      return "Wrist";
    case INVTYPE_HANDS:
      return "Hands";
    case INVTYPE_FINGER:
      return "Finger";
    case INVTYPE_TRINKET:
      return "Trinket";
    case INVTYPE_CLOAK:
      return "Back";
    case INVTYPE_WEAPON:
      return "One-Hand";
    case INVTYPE_2HWEAPON:
      return "Two-Hand";
    case INVTYPE_WEAPONMAINHAND:
      return "Main Hand";
    case INVTYPE_WEAPONOFFHAND:
      return "Off Hand";
    case INVTYPE_SHIELD:
      return "Off Hand";
    case INVTYPE_HOLDABLE:
      return "Held In Off-hand";
    case INVTYPE_RANGED:
    case INVTYPE_RANGEDRIGHT:
      return "Ranged";
    case INVTYPE_THROWN:
      return "Thrown";
    case INVTYPE_RELIC:
      return "Relic";
    case INVTYPE_TABARD:
      return "Tabard";
    case INVTYPE_BAG:
      return "Bag";
    default:
      return "";
  }
}

// "Cloth", "Mace", ...: the item's subclass as the tooltip names it.
std::string TypeWord(const ItemTemplate* p) {
  if (p->Class == ITEM_CLASS_ARMOR) {
    static constexpr const char* kArmor[] = {"", "Cloth", "Leather", "Mail",
                                             "", "Plate", "",        "Shield",
                                             "", "",      "",        "Relic"};
    return p->SubClass < std::size(kArmor) ? kArmor[p->SubClass] : "";
  }
  if (p->Class == ITEM_CLASS_WEAPON) {
    static constexpr const char* kWeapon[] = {
        "Axe",      "Axe",         "Bow",           "Gun",    "Mace",   "Mace",
        "Polearm",  "Sword",       "Sword",         "",       "Staff",  "",
        "",         "Fist Weapon", "Miscellaneous", "Dagger", "Thrown", "",
        "Crossbow", "Wand",        "Fishing Pole"};
    return p->SubClass < std::size(kWeapon) ? kWeapon[p->SubClass] : "";
  }
  return "";
}

std::optional<pb::Stat> ToStat(uint32_t mod) {
  switch (mod) {
    case ITEM_MOD_MANA:
      return pb::Stat::MANA;
    case ITEM_MOD_HEALTH:
      return pb::Stat::HEALTH;
    case ITEM_MOD_AGILITY:
      return pb::Stat::AGILITY;
    case ITEM_MOD_STRENGTH:
      return pb::Stat::STRENGTH;
    case ITEM_MOD_INTELLECT:
      return pb::Stat::INTELLECT;
    case ITEM_MOD_SPIRIT:
      return pb::Stat::SPIRIT;
    case ITEM_MOD_STAMINA:
      return pb::Stat::STAMINA;
    case ITEM_MOD_DEFENSE_SKILL_RATING:
      return pb::Stat::DEFENSE_RATING;
    case ITEM_MOD_DODGE_RATING:
      return pb::Stat::DODGE_RATING;
    case ITEM_MOD_PARRY_RATING:
      return pb::Stat::PARRY_RATING;
    case ITEM_MOD_BLOCK_RATING:
      return pb::Stat::BLOCK_RATING;
    case ITEM_MOD_HIT_RATING:
      return pb::Stat::HIT_RATING;
    case ITEM_MOD_CRIT_RATING:
      return pb::Stat::CRIT_RATING;
    case ITEM_MOD_RESILIENCE_RATING:
      return pb::Stat::RESILIENCE_RATING;
    case ITEM_MOD_HASTE_RATING:
      return pb::Stat::HASTE_RATING;
    case ITEM_MOD_EXPERTISE_RATING:
      return pb::Stat::EXPERTISE_RATING;
    case ITEM_MOD_ATTACK_POWER:
      return pb::Stat::ATTACK_POWER;
    case ITEM_MOD_RANGED_ATTACK_POWER:
      return pb::Stat::RANGED_ATTACK_POWER;
    case ITEM_MOD_MANA_REGENERATION:
      return pb::Stat::MP5;
    case ITEM_MOD_ARMOR_PENETRATION_RATING:
      return pb::Stat::ARMOR_PENETRATION_RATING;
    case ITEM_MOD_SPELL_POWER:
      return pb::Stat::SPELL_POWER;
    case ITEM_MOD_SPELL_PENETRATION:
      return pb::Stat::SPELL_PENETRATION;
    case ITEM_MOD_BLOCK_VALUE:
      return pb::Stat::BLOCK_VALUE;
    default:
      return std::nullopt;
  }
}

// "+2 Stamina", "Improves hit rating by 10.", as the tooltip writes them.
std::string StatLine(pb::Stat stat, int32_t value) {
  switch (stat) {
    case pb::Stat::STRENGTH:
      return base::StringPrintf("%+d Strength", value);
    case pb::Stat::AGILITY:
      return base::StringPrintf("%+d Agility", value);
    case pb::Stat::STAMINA:
      return base::StringPrintf("%+d Stamina", value);
    case pb::Stat::INTELLECT:
      return base::StringPrintf("%+d Intellect", value);
    case pb::Stat::SPIRIT:
      return base::StringPrintf("%+d Spirit", value);
    case pb::Stat::HEALTH:
      return base::StringPrintf("%+d Health", value);
    case pb::Stat::MANA:
      return base::StringPrintf("%+d Mana", value);
    case pb::Stat::ATTACK_POWER:
      return base::StringPrintf("Equip: Increases attack power by %d.", value);
    case pb::Stat::RANGED_ATTACK_POWER:
      return base::StringPrintf("Equip: Increases ranged attack power by %d.",
                                value);
    case pb::Stat::SPELL_POWER:
      return base::StringPrintf("Equip: Increases spell power by %d.", value);
    case pb::Stat::MP5:
      return base::StringPrintf("Equip: Restores %d mana per 5 sec.", value);
    case pb::Stat::HIT_RATING:
      return base::StringPrintf("Equip: Improves hit rating by %d.", value);
    case pb::Stat::CRIT_RATING:
      return base::StringPrintf("Equip: Improves critical strike rating by %d.",
                                value);
    case pb::Stat::HASTE_RATING:
      return base::StringPrintf("Equip: Improves haste rating by %d.", value);
    default:
      return base::StringPrintf("Equip: +%d %s", value,
                                std::string(pb::EnumName(stat)).c_str());
  }
}

const char* BindingLine(uint32_t bonding) {
  switch (bonding) {
    case BIND_WHEN_PICKED_UP:
      return "Binds when picked up";
    case BIND_WHEN_EQUIPPED:
      return "Binds when equipped";
    case BIND_WHEN_USE:
      return "Binds when used";
    case BIND_QUEST_ITEM:
    case BIND_QUEST_ITEM1:
      return "Quest Item";
    default:
      return "";
  }
}

pb::Item::Binding ToBinding(uint32_t bonding) {
  switch (bonding) {
    case NO_BIND:
      return pb::Item::NOT_BOUND;
    case BIND_WHEN_PICKED_UP:
    case BIND_QUEST_ITEM:
    case BIND_QUEST_ITEM1:
      return pb::Item::BIND_ON_PICKUP;
    case BIND_WHEN_EQUIPPED:
      return pb::Item::BIND_ON_EQUIP;
    case BIND_WHEN_USE:
      return pb::Item::BIND_ON_USE;
    default:
      return pb::Item::BINDING_UNSPECIFIED;
  }
}

// What a glance at a bag slot shows: the icon, its count, its border color.
pb::Item BasicItem(const ItemTemplate* p, uint32_t count) {
  pb::Item item;
  item.id = p->ItemId;
  item.name = p->Name1;
  item.quality = ToQuality(p->Quality);
  item.count = count;
  item.kind = ToKind(p);
  item.starts_quest = p->StartQuest != 0;
  return item;
}

const _Spell* UseSpell(const ItemTemplate* p) {
  for (const _Spell& spell : p->Spells) {
    if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE) {
      return &spell;
    }
  }
  return nullptr;
}

pb::Item BagItem(Player* bot, Item* item, uint8_t bag, uint8_t slot) {
  const ItemTemplate* p = item->GetTemplate();
  pb::Item out = BasicItem(p, item->GetCount());
  out.bag_slot =
      bag == INVENTORY_SLOT_BAG_0
          ? base::StringPrintf("backpack %u",
                               slot - INVENTORY_SLOT_ITEM_START + 1)
          : base::StringPrintf("bag %u slot %u",
                               bag - INVENTORY_SLOT_BAG_START + 1, slot + 1);
  if (const _Spell* use = UseSpell(p)) {
    out.usable = bot->CanUseItem(item) == EQUIP_ERR_OK;
    out.cooldown_s =
        static_cast<float>(bot->GetSpellCooldownDelay(use->SpellId)) / 1000.0f;
    if (use->SpellCharges != 0) {
      out.charges = static_cast<uint32_t>(std::abs(item->GetSpellCharges()));
    }
  }
  return out;
}

bool IsGeneralBag(const Bag* bag) {
  const ItemTemplate* p = bag->GetTemplate();
  return p && p->Class == ITEM_CLASS_CONTAINER &&
         p->SubClass == ITEM_SUBCLASS_CONTAINER;
}

// The spell values a description's tokens name (spell_text.h).
class SpellValues : public SpellValueSource {
 public:
  SpellValues(Player* bot, uint32_t spell_id)
      : bot_(bot), spell_id_(spell_id) {}

  std::optional<std::string> Value(uint32_t spell_id,
                                   char code,
                                   int index) const override {
    if (code == 'z') {
      const std::string home = AreaName(bot_->m_homebindAreaId);
      return home.empty() ? std::nullopt : std::optional(home);
    }
    const SpellInfo* info =
        sSpellMgr->GetSpellInfo(spell_id != 0 ? spell_id : spell_id_);
    if (!info) {
      return std::nullopt;
    }
    const int effect = index > 0 ? index - 1 : 0;
    const SpellEffectInfo& e = info->Effects[effect];
    switch (code) {
      case 's':
      case 'm':
        return base::NumberToString(std::abs(e.CalcValue(bot_)));
      case 'o': {
        if (e.Amplitude <= 0 || info->GetDuration() <= 0) {
          return std::nullopt;
        }
        const int32_t ticks = info->GetDuration() / e.Amplitude;
        return base::NumberToString(std::abs(e.CalcValue(bot_)) * ticks);
      }
      case 't':
        if (e.Amplitude <= 0) {
          return std::nullopt;
        }
        return base::StringPrintf("%g", e.Amplitude / 1000.0);
      case 'd': {
        const int32_t ms = info->GetDuration();
        if (ms <= 0) {
          return std::nullopt;
        }
        const int32_t s = ms / 1000;
        if (s < 60) {
          return base::StringPrintf("%d sec", s);
        }
        if (s < 3600) {
          return base::StringPrintf("%d min", s / 60);
        }
        return base::StringPrintf("%d hrs", s / 3600);
      }
      case 'a':
        return base::StringPrintf("%g", e.CalcRadius());
      case 'h':
        return base::NumberToString(info->ProcChance);
      case 'n':
        return base::NumberToString(info->ProcCharges);
      case 'u':
        return base::NumberToString(info->StackAmount);
      case 'x':
        return base::NumberToString(e.ChainTarget);
      default:
        return std::nullopt;
    }
  }

 private:
  Player* bot_;
  uint32_t spell_id_;
};

std::string SpellLine(Player* bot,
                      const _Spell& spell,
                      const ItemSpellTexts* texts) {
  const char* prefix = "";
  switch (spell.SpellTrigger) {
    case ITEM_SPELLTRIGGER_ON_USE:
      prefix = "Use: ";
      break;
    case ITEM_SPELLTRIGGER_ON_EQUIP:
      prefix = "Equip: ";
      break;
    case ITEM_SPELLTRIGGER_CHANCE_ON_HIT:
      prefix = "Chance on hit: ";
      break;
    default:
      return "";
  }
  const uint32_t id = static_cast<uint32_t>(spell.SpellId);
  if (const std::string* text = texts ? texts->Find(id) : nullptr) {
    return prefix + FormatSpellText(*text, SpellValues(bot, id));
  }
  const SpellInfo* info = sSpellMgr->GetSpellInfo(id);
  const char* name =
      info ? info->SpellName[sWorld->GetDefaultDbcLocale()] : nullptr;
  return name ? std::string(prefix) + name : "";
}

void AddItemTooltip(Player* bot,
                    const ItemTemplate* p,
                    const ItemSpellTexts* texts,
                    pb::Tooltipped* out) {
  Item* have = bot->GetItemByEntry(p->ItemId);
  pb::Item& item = out->item.emplace(BasicItem(p, have ? have->GetCount() : 1));
  item.item_level = p->ItemLevel;
  item.required_level = p->RequiredLevel;
  item.binding = ToBinding(p->Bonding);
  item.unique = p->MaxCount == 1;
  item.vendor_value = p->SellPrice;

  std::vector<std::string>& lines = out->tooltip.emplace().lines;
  lines.push_back(p->Name1);
  if (have && have->IsSoulBound()) {
    lines.push_back("Soulbound");
  } else if (const char* bind = BindingLine(p->Bonding); *bind) {
    lines.push_back(bind);
  }
  if (item.unique) {
    lines.push_back("Unique");
  }
  const pb::EquipmentPart::Slot slot = SlotOfType(p->InventoryType);
  if (const char* word = SlotWord(p->InventoryType); *word) {
    const std::string type = TypeWord(p);
    lines.push_back(type.empty() ? word : std::string(word) + "  " + type);
  }
  if (p->Damage[0].DamageMax > 0 && p->Delay > 0) {
    const float speed = static_cast<float>(p->Delay) / 1000.0f;
    lines.push_back(base::StringPrintf("%.0f - %.0f Damage  Speed %.2f",
                                       p->Damage[0].DamageMin,
                                       p->Damage[0].DamageMax, speed));
    lines.push_back(base::StringPrintf(
        "(%.1f damage per second)",
        (p->Damage[0].DamageMin + p->Damage[0].DamageMax) / 2.0f / speed));
  }
  if (p->Armor > 0) {
    lines.push_back(base::StringPrintf("%u Armor", p->Armor));
  }
  std::vector<pb::StatValue> stats;
  for (uint32_t i = 0; i < p->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i) {
    if (const std::optional<pb::Stat> stat =
            ToStat(p->ItemStat[i].ItemStatType);
        stat && p->ItemStat[i].ItemStatValue != 0) {
      stats.push_back(
          {*stat, static_cast<float>(p->ItemStat[i].ItemStatValue)});
      lines.push_back(StatLine(*stat, p->ItemStat[i].ItemStatValue));
    }
  }
  if (p->MaxDurability > 0) {
    const uint32_t current =
        have ? have->GetUInt32Value(ITEM_FIELD_DURABILITY) : p->MaxDurability;
    lines.push_back(
        base::StringPrintf("Durability %u / %u", current, p->MaxDurability));
  }
  if (p->RequiredLevel > 1) {
    lines.push_back(base::StringPrintf("Requires Level %u", p->RequiredLevel));
  }
  for (const _Spell& spell : p->Spells) {
    if (spell.SpellId > 0) {
      if (std::string line = SpellLine(bot, spell, texts); !line.empty()) {
        lines.push_back(std::move(line));
      }
    }
  }
  if (!p->Description.empty()) {
    lines.push_back("\"" + p->Description + "\"");
  }
  lines.push_back(p->SellPrice > 0 ? "Sell Price: " + MoneyText(p->SellPrice)
                                   : "No sell price");

  if (slot == pb::EquipmentPart::SLOT_UNSPECIFIED ||
      slot == pb::EquipmentPart::BAG) {
    return;
  }
  pb::EquipmentPart& equipment = item.equipment.emplace();
  equipment.slot = slot;
  equipment.stats = stats;
  equipment.type = TypeWord(p);
  // What the bot wears there, to compare (the client shows it beside).
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  const uint8_t eq_slot = ai ? ai->FindEquipSlot(p, NULL_SLOT, true)
                             : static_cast<uint8_t>(NULL_SLOT);
  Item* worn = eq_slot < EQUIPMENT_SLOT_END
                   ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, eq_slot)
                   : nullptr;
  StatsWeightCalculator calc(bot);
  const float score = calc.CalculateItem(p->ItemId, 0, eq_slot);
  if (worn && worn->GetTemplate()) {
    out->compare.push_back(BasicItem(worn->GetTemplate(), 1));
    equipment.upgrade =
        score > calc.CalculateItem(worn->GetEntry(), 0, eq_slot);
  } else {
    equipment.upgrade = score > 0 && bot->CanUseItem(p) == EQUIP_ERR_OK;
  }
}

std::string ProgressLine(Player* bot, const Quest* q) {
  std::vector<std::string> parts;
  for (uint32_t i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
    const int32_t entry = q->RequiredNpcOrGo[i];
    const uint32_t need = q->RequiredNpcOrGoCount[i];
    if (entry == 0 || need == 0) {
      continue;
    }
    std::string name;
    if (entry > 0) {
      if (const CreatureTemplate* c = sObjectMgr->GetCreatureTemplate(entry)) {
        name = c->Name;
      }
    } else if (const GameObjectTemplate* g =
                   sObjectMgr->GetGameObjectTemplate(-entry)) {
      name = g->name;
    }
    parts.push_back(base::StringPrintf(
        "%s: %u/%u", name.c_str(),
        std::min<uint32_t>(
            bot->GetReqKillOrCastCurrentCount(q->GetQuestId(), entry), need),
        need));
  }
  for (uint32_t i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i) {
    const uint32_t item = q->RequiredItemId[i];
    const uint32_t need = q->RequiredItemCount[i];
    const ItemTemplate* p = item ? sObjectMgr->GetItemTemplate(item) : nullptr;
    if (!p || need == 0) {
      continue;
    }
    parts.push_back(base::StringPrintf("%s: %u/%u", p->Name1.c_str(),
                                       std::min(bot->GetItemCount(item), need),
                                       need));
  }
  return base::JoinString(parts, ", ");
}

std::vector<pb::Item> RewardItems(const uint32_t* ids,
                                  const uint32_t* counts,
                                  uint32_t n) {
  std::vector<pb::Item> out;
  for (uint32_t i = 0; i < n; ++i) {
    if (const ItemTemplate* p =
            ids[i] ? sObjectMgr->GetItemTemplate(ids[i]) : nullptr) {
      out.push_back(BasicItem(p, std::max<uint32_t>(counts[i], 1)));
    }
  }
  return out;
}

std::string ItemList(const std::vector<pb::Item>& items) {
  std::vector<std::string> names;
  for (const pb::Item& item : items) {
    names.push_back(
        item.count > 1
            ? base::StringPrintf("%s x%u", item.name.c_str(), item.count)
            : item.name);
  }
  return base::JoinString(names, ", ");
}

void AddQuestTooltip(Player* bot, const Quest* q, pb::Tooltipped* out) {
  std::vector<std::string>& lines = out->tooltip.emplace().lines;
  lines.push_back(q->GetTitle());
  if (q->GetQuestLevel() > 0) {
    lines.push_back(base::StringPrintf("Level %d", q->GetQuestLevel()));
  }
  if (!q->GetObjectives().empty()) {
    lines.push_back(q->GetObjectives());
  }
  if (std::string progress = ProgressLine(bot, q); !progress.empty()) {
    lines.push_back(std::move(progress));
  }
  if (bot->GetQuestStatus(q->GetQuestId()) == QUEST_STATUS_COMPLETE) {
    lines.push_back("Complete: turn it in");
  }
  const std::vector<pb::Item> rewards =
      RewardItems(q->RewardItemId, q->RewardItemIdCount, QUEST_REWARDS_COUNT);
  if (!rewards.empty()) {
    lines.push_back("You will receive: " + ItemList(rewards));
  }
  const std::vector<pb::Item> choices =
      RewardItems(q->RewardChoiceItemId, q->RewardChoiceItemCount,
                  QUEST_REWARD_CHOICES_COUNT);
  if (!choices.empty()) {
    lines.push_back("Choose one of: " + ItemList(choices));
  }
  if (const int32_t money = q->GetRewOrReqMoney(bot->GetLevel()); money > 0) {
    lines.push_back("Money: " + MoneyText(static_cast<uint32_t>(money)));
  }
}

}  // namespace

ItemSpellTexts::ItemSpellTexts(std::unordered_map<uint32_t, std::string> texts)
    : texts_(std::move(texts)) {}

ItemSpellTexts::~ItemSpellTexts() = default;

std::unique_ptr<ItemSpellTexts> ItemSpellTexts::Load() {
  std::string path = sConfigMgr->GetOption<std::string>("DataDir", "./");
  if (!path.empty() && path.back() != '/') {
    path.push_back('/');
  }
  path += "dbc/Spell.dbc";
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return nullptr;
  }
  std::string bytes;
  char buf[1 << 16];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    bytes.append(buf, n);
  }
  std::fclose(f);
  std::vector<uint32_t> wanted;
  for (const auto& [id, p] : *sObjectMgr->GetItemTemplateStore()) {
    for (const _Spell& spell : p.Spells) {
      if (spell.SpellId > 0) {
        wanted.push_back(static_cast<uint32_t>(spell.SpellId));
      }
    }
  }
  wanted.push_back(kHearthstoneSpell);
  std::unordered_map<uint32_t, std::string> texts = ParseSpellDescriptions(
      bytes, wanted, static_cast<uint32_t>(sWorld->GetDefaultDbcLocale()));
  if (texts.empty()) {
    return nullptr;
  }
  return std::unique_ptr<ItemSpellTexts>(new ItemSpellTexts(std::move(texts)));
}

const std::string* ItemSpellTexts::Find(uint32_t spell_id) const {
  const auto it = texts_.find(spell_id);
  return it == texts_.end() ? nullptr : &it->second;
}

void FillBags(Player* bot, pb::Bags* bags) {
  uint32_t total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
  uint32_t used = 0;
  for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END;
       ++slot) {
    if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) {
      ++used;
      bags->items.push_back(BagItem(bot, item, INVENTORY_SLOT_BAG_0, slot));
    }
  }
  for (uint8_t bag_pos = INVENTORY_SLOT_BAG_START;
       bag_pos < INVENTORY_SLOT_BAG_END; ++bag_pos) {
    Bag* bag = bot->GetBagByPos(bag_pos);
    if (!bag) {
      continue;
    }
    const bool general = IsGeneralBag(bag);
    if (general) {
      total += bag->GetBagSize();
    }
    for (uint32_t slot = 0; slot < bag->GetBagSize(); ++slot) {
      if (Item* item = bot->GetItemByPos(bag_pos, static_cast<uint8_t>(slot))) {
        used += general ? 1 : 0;
        bags->items.push_back(
            BagItem(bot, item, bag_pos, static_cast<uint8_t>(slot)));
      }
    }
  }
  bags->total_slots = total;
  bags->free_slots = total - std::min(used, total);
  if (bot->HasItemCount(kHearthstoneItem, 1)) {
    pb::Bags::Hearthstone& stone = bags->hearthstone.emplace();
    stone.ready = !bot->HasSpellCooldown(kHearthstoneSpell);
    stone.bind = AreaName(bot->m_homebindAreaId);
  }
}

void FillCharacter(Player* bot, pb::Character* character) {
  for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot) {
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item || !item->GetTemplate()) {
      continue;
    }
    pb::Item& out =
        character->equipped.emplace_back(BasicItem(item->GetTemplate(), 1));
    pb::EquipmentPart& eq = out.equipment.emplace();
    eq.slot = SlotOfEquipmentPos(slot);
    const uint32_t max = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    if (max > 0) {
      eq.durability_pct =
          100.0f * item->GetUInt32Value(ITEM_FIELD_DURABILITY) / max;
    }
  }
  const auto add = [character](pb::Stat stat, float value) {
    character->stats.push_back({stat, value});
  };
  add(pb::Stat::STRENGTH, bot->GetStat(STAT_STRENGTH));
  add(pb::Stat::AGILITY, bot->GetStat(STAT_AGILITY));
  add(pb::Stat::STAMINA, bot->GetStat(STAT_STAMINA));
  add(pb::Stat::INTELLECT, bot->GetStat(STAT_INTELLECT));
  add(pb::Stat::SPIRIT, bot->GetStat(STAT_SPIRIT));
  add(pb::Stat::ARMOR, static_cast<float>(bot->GetArmor()));
  add(pb::Stat::ATTACK_POWER, bot->GetTotalAttackPowerValue(BASE_ATTACK));
  add(pb::Stat::SPELL_POWER, static_cast<float>(bot->SpellBaseDamageBonusDone(
                                 SPELL_SCHOOL_MASK_MAGIC)));
  add(pb::Stat::MELEE_CRIT_PCT, bot->GetFloatValue(PLAYER_CRIT_PERCENTAGE));
  add(pb::Stat::SPELL_CRIT_PCT,
      bot->GetFloatValue(static_cast<uint16_t>(PLAYER_SPELL_CRIT_PERCENTAGE1) +
                         static_cast<uint16_t>(SPELL_SCHOOL_HOLY)));
  add(pb::Stat::DODGE_PCT, bot->GetFloatValue(PLAYER_DODGE_PERCENTAGE));
}

void FillQuestLog(Player* bot, pb::QuestLog* log) {
  for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot) {
    const uint32_t quest_id = bot->GetQuestSlotQuestId(slot);
    const Quest* q =
        quest_id ? sObjectMgr->GetQuestTemplate(quest_id) : nullptr;
    if (!q) {
      continue;
    }
    pb::QuestLog::Entry& entry = log->quests.emplace_back();
    pb::Objective& obj = entry.objective.emplace();
    obj.title = q->GetTitle();
    pb::QuestPart& part = obj.quest.emplace();
    part.quest_id = quest_id;
    part.quest = q->GetTitle();
    const QuestStatus status = bot->GetQuestStatus(quest_id);
    part.complete = status == QUEST_STATUS_COMPLETE;
    part.failed = status == QUEST_STATUS_FAILED;
    part.daily = q->IsDaily();
    if (q->GetQuestLevel() > 0) {
      part.level = static_cast<uint32_t>(q->GetQuestLevel());
    }
    obj.done = part.complete;
    part.text = ProgressLine(bot, q);
    entry.description = q->GetObjectives();
    entry.rewards =
        RewardItems(q->RewardItemId, q->RewardItemIdCount, QUEST_REWARDS_COUNT);
    entry.choices = RewardItems(q->RewardChoiceItemId, q->RewardChoiceItemCount,
                                QUEST_REWARD_CHOICES_COUNT);
    entry.money = static_cast<uint32_t>(
        std::max<int32_t>(0, q->GetRewOrReqMoney(bot->GetLevel())));
    entry.shareable = q->HasFlag(QUEST_FLAGS_SHARABLE);
  }
}

std::optional<pb::Tooltipped> BuildTooltip(Player* bot,
                                           const pb::HoverRef& ref,
                                           const ItemSpellTexts* texts) {
  uint32_t id = 0;
  if (!base::StringToUint(ref.id, &id) || id == 0) {
    return std::nullopt;
  }
  pb::Tooltipped out;
  out.ref = ref;
  if (ref.kind == pb::HoverRef::ITEM) {
    const ItemTemplate* p = sObjectMgr->GetItemTemplate(id);
    if (!p) {
      return std::nullopt;
    }
    AddItemTooltip(bot, p, texts, &out);
    return out;
  }
  if (ref.kind == pb::HoverRef::QUEST) {
    const Quest* q = sObjectMgr->GetQuestTemplate(id);
    if (!q || bot->GetQuestStatus(id) == QUEST_STATUS_NONE) {
      return std::nullopt;  // Not in the bot's quest log: nothing to point at.
    }
    AddQuestTooltip(bot, q, &out);
    return out;
  }
  return std::nullopt;
}

}  // namespace llm_brain
