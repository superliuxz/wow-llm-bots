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
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "snapshot/perception.h"
#include "snapshot/snapshot.h"
#include "snapshot/spell_text.h"
#include "snapshot/world_util.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr uint32_t kHearthstoneItem = 6948;
constexpr uint32_t kHearthstoneSpell = 8690;

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
  static constexpr pb::Item::Quality kQualities[] = {
      pb::Item::POOR,         pb::Item::COMMON,  pb::Item::UNCOMMON,
      pb::Item::RARE_QUALITY, pb::Item::EPIC,    pb::Item::LEGENDARY,
      pb::Item::ARTIFACT,     pb::Item::HEIRLOOM};
  return quality < std::size(kQualities) ? kQualities[quality]
                                         : pb::Item::QUALITY_UNSPECIFIED;
}

pb::Item::Kind ToKind(const ItemTemplate* p) {
  if (p->Quality == ITEM_QUALITY_POOR) {
    return pb::Item::JUNK;
  }
  struct Entry {
    uint32_t item_class;
    pb::Item::Kind kind;
  };
  static constexpr Entry kKinds[] = {
      {ITEM_CLASS_WEAPON, pb::Item::EQUIPMENT},
      {ITEM_CLASS_ARMOR, pb::Item::EQUIPMENT},
      {ITEM_CLASS_CONSUMABLE, pb::Item::CONSUMABLE},
      {ITEM_CLASS_CONTAINER, pb::Item::CONTAINER},
      {ITEM_CLASS_QUIVER, pb::Item::CONTAINER},
      {ITEM_CLASS_GEM, pb::Item::GEM},
      {ITEM_CLASS_REAGENT, pb::Item::REAGENT},
      {ITEM_CLASS_PROJECTILE, pb::Item::PROJECTILE},
      {ITEM_CLASS_TRADE_GOODS, pb::Item::TRADE_GOOD},
      {ITEM_CLASS_RECIPE, pb::Item::RECIPE},
      {ITEM_CLASS_QUEST, pb::Item::QUEST},
      {ITEM_CLASS_KEY, pb::Item::KEY},
      {ITEM_CLASS_GLYPH, pb::Item::GLYPH},
  };
  for (const Entry& e : kKinds) {
    if (e.item_class == p->Class) {
      return e.kind;
    }
  }
  return pb::Item::OTHER;
}

struct InvTypeInfo {
  uint32_t inv_type;
  pb::EquipmentPart::Slot slot;
  const char* word;
};

constexpr InvTypeInfo kInvTypes[] = {
    {INVTYPE_HEAD, pb::EquipmentPart::HEAD, "Head"},
    {INVTYPE_NECK, pb::EquipmentPart::NECK, "Neck"},
    {INVTYPE_SHOULDERS, pb::EquipmentPart::SHOULDER, "Shoulder"},
    {INVTYPE_BODY, pb::EquipmentPart::SHIRT, "Shirt"},
    {INVTYPE_CHEST, pb::EquipmentPart::CHEST, "Chest"},
    {INVTYPE_ROBE, pb::EquipmentPart::CHEST, "Chest"},
    {INVTYPE_WAIST, pb::EquipmentPart::WAIST, "Waist"},
    {INVTYPE_LEGS, pb::EquipmentPart::LEGS, "Legs"},
    {INVTYPE_FEET, pb::EquipmentPart::FEET, "Feet"},
    {INVTYPE_WRISTS, pb::EquipmentPart::WRIST, "Wrist"},
    {INVTYPE_HANDS, pb::EquipmentPart::HANDS, "Hands"},
    {INVTYPE_FINGER, pb::EquipmentPart::FINGER1, "Finger"},
    {INVTYPE_TRINKET, pb::EquipmentPart::TRINKET1, "Trinket"},
    {INVTYPE_CLOAK, pb::EquipmentPart::BACK, "Back"},
    {INVTYPE_WEAPON, pb::EquipmentPart::MAIN_HAND, "One-Hand"},
    {INVTYPE_2HWEAPON, pb::EquipmentPart::MAIN_HAND, "Two-Hand"},
    {INVTYPE_WEAPONMAINHAND, pb::EquipmentPart::MAIN_HAND, "Main Hand"},
    {INVTYPE_WEAPONOFFHAND, pb::EquipmentPart::OFF_HAND, "Off Hand"},
    {INVTYPE_SHIELD, pb::EquipmentPart::OFF_HAND, "Off Hand"},
    {INVTYPE_HOLDABLE, pb::EquipmentPart::OFF_HAND, "Held In Off-hand"},
    {INVTYPE_RANGED, pb::EquipmentPart::RANGED, "Ranged"},
    {INVTYPE_RANGEDRIGHT, pb::EquipmentPart::RANGED, "Ranged"},
    {INVTYPE_THROWN, pb::EquipmentPart::RANGED, "Thrown"},
    {INVTYPE_RELIC, pb::EquipmentPart::RANGED, "Relic"},
    {INVTYPE_TABARD, pb::EquipmentPart::TABARD, "Tabard"},
    {INVTYPE_BAG, pb::EquipmentPart::BAG, "Bag"},
    {INVTYPE_QUIVER, pb::EquipmentPart::BAG, ""},
};

const InvTypeInfo* FindInvType(uint32_t inventory_type) {
  for (const InvTypeInfo& info : kInvTypes) {
    if (info.inv_type == inventory_type) {
      return &info;
    }
  }
  return nullptr;
}

pb::EquipmentPart::Slot SlotOfType(uint32_t inventory_type) {
  const InvTypeInfo* info = FindInvType(inventory_type);
  return info ? info->slot : pb::EquipmentPart::SLOT_UNSPECIFIED;
}

const char* SlotWord(uint32_t inventory_type) {
  const InvTypeInfo* info = FindInvType(inventory_type);
  return info ? info->word : "";
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

// "Cloth", "Mace", ...: the item's subclass as the tooltip names it.
std::string TypeWord(const ItemTemplate* p) {
  if (p->Class == ITEM_CLASS_ARMOR) {
    // Matches upstream enum ItemSubclassArmor (ItemTemplate.h).
    static constexpr const char* kArmor[] = {
        "",       "Cloth",  "Leather", "Mail",  "Plate", "Buckler",
        "Shield", "Libram", "Idol",    "Totem", "Sigil"};
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

struct StatInfo {
  uint32_t mod;
  pb::Stat stat;
  const char* prefix;  // Empty: signed primary stat ("%+d <suffix>").
  const char* suffix;
};

constexpr StatInfo kStats[] = {
    {ITEM_MOD_STRENGTH, pb::Stat::STRENGTH, "", " Strength"},
    {ITEM_MOD_AGILITY, pb::Stat::AGILITY, "", " Agility"},
    {ITEM_MOD_STAMINA, pb::Stat::STAMINA, "", " Stamina"},
    {ITEM_MOD_INTELLECT, pb::Stat::INTELLECT, "", " Intellect"},
    {ITEM_MOD_SPIRIT, pb::Stat::SPIRIT, "", " Spirit"},
    {ITEM_MOD_HEALTH, pb::Stat::HEALTH, "", " Health"},
    {ITEM_MOD_MANA, pb::Stat::MANA, "", " Mana"},
    {ITEM_MOD_ATTACK_POWER, pb::Stat::ATTACK_POWER,
     "Equip: Increases attack power by ", "."},
    {ITEM_MOD_RANGED_ATTACK_POWER, pb::Stat::RANGED_ATTACK_POWER,
     "Equip: Increases ranged attack power by ", "."},
    {ITEM_MOD_SPELL_POWER, pb::Stat::SPELL_POWER,
     "Equip: Increases spell power by ", "."},
    {ITEM_MOD_MANA_REGENERATION, pb::Stat::MP5, "Equip: Restores ",
     " mana per 5 sec."},
    {ITEM_MOD_HIT_RATING, pb::Stat::HIT_RATING,
     "Equip: Improves hit rating by ", "."},
    {ITEM_MOD_CRIT_RATING, pb::Stat::CRIT_RATING,
     "Equip: Improves critical strike rating by ", "."},
    {ITEM_MOD_HASTE_RATING, pb::Stat::HASTE_RATING,
     "Equip: Improves haste rating by ", "."},
    {ITEM_MOD_DEFENSE_SKILL_RATING, pb::Stat::DEFENSE_RATING, nullptr, nullptr},
    {ITEM_MOD_DODGE_RATING, pb::Stat::DODGE_RATING, nullptr, nullptr},
    {ITEM_MOD_PARRY_RATING, pb::Stat::PARRY_RATING, nullptr, nullptr},
    {ITEM_MOD_BLOCK_RATING, pb::Stat::BLOCK_RATING, nullptr, nullptr},
    {ITEM_MOD_RESILIENCE_RATING, pb::Stat::RESILIENCE_RATING, nullptr, nullptr},
    {ITEM_MOD_EXPERTISE_RATING, pb::Stat::EXPERTISE_RATING, nullptr, nullptr},
    {ITEM_MOD_ARMOR_PENETRATION_RATING, pb::Stat::ARMOR_PENETRATION_RATING,
     nullptr, nullptr},
    {ITEM_MOD_SPELL_PENETRATION, pb::Stat::SPELL_PENETRATION, nullptr, nullptr},
    {ITEM_MOD_BLOCK_VALUE, pb::Stat::BLOCK_VALUE, nullptr, nullptr},
};

const StatInfo* FindStat(uint32_t mod) {
  for (const StatInfo& info : kStats) {
    if (info.mod == mod) {
      return &info;
    }
  }
  return nullptr;
}

// "+2 Stamina", "Equip: Improves hit rating by 10.", as the tooltip writes.
std::string StatLine(const StatInfo& info, int32_t value) {
  if (!info.prefix) {
    return base::StrCat({"Equip: +", base::NumberToString(value), " ",
                         pb::EnumName(info.stat)});
  }
  if (!*info.prefix) {
    return base::StringPrintf("%+d%s", value, info.suffix);
  }
  return base::StrCat({info.prefix, base::NumberToString(value), info.suffix});
}

struct BindingInfo {
  uint32_t bonding;
  pb::Item::Binding binding;
  const char* line;
};

constexpr BindingInfo kBindings[] = {
    {NO_BIND, pb::Item::NOT_BOUND, ""},
    {BIND_WHEN_PICKED_UP, pb::Item::BIND_ON_PICKUP, "Binds when picked up"},
    {BIND_WHEN_EQUIPPED, pb::Item::BIND_ON_EQUIP, "Binds when equipped"},
    {BIND_WHEN_USE, pb::Item::BIND_ON_USE, "Binds when used"},
    {BIND_QUEST_ITEM, pb::Item::BIND_ON_PICKUP, "Quest Item"},
    {BIND_QUEST_ITEM1, pb::Item::BIND_ON_PICKUP, "Quest Item"},
};

const char* BindingLine(uint32_t bonding) {
  for (const BindingInfo& info : kBindings) {
    if (info.bonding == bonding) {
      return info.line;
    }
  }
  return "";
}

pb::Item::Binding ToBinding(uint32_t bonding) {
  for (const BindingInfo& info : kBindings) {
    if (info.bonding == bonding) {
      return info.binding;
    }
  }
  return pb::Item::BINDING_UNSPECIFIED;
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

struct UseSpellMatch {
  const _Spell* spell = nullptr;
  uint8_t index = 0;
};

std::optional<UseSpellMatch> UseSpell(const ItemTemplate* p) {
  for (uint8_t i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i) {
    const _Spell& spell = p->Spells[i];
    if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE) {
      return UseSpellMatch{&spell, i};
    }
  }
  return std::nullopt;
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
  if (const std::optional<UseSpellMatch> use = UseSpell(p)) {
    out.usable = bot->CanUseItem(item) == EQUIP_ERR_OK;
    out.cooldown_s =
        static_cast<float>(bot->GetSpellCooldownDelay(use->spell->SpellId)) /
        1000.0f;
    if (use->spell->SpellCharges != 0) {
      out.charges =
          static_cast<uint32_t>(std::abs(item->GetSpellCharges(use->index)));
    }
  }
  return out;
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
    const auto [lo, hi] = Range(*info, e);
    switch (code) {
      case 's':
        return lo == hi ? base::NumberToString(lo)
                        : base::StringPrintf("%d to %d", lo, hi);
      case 'm':
        return base::NumberToString(lo);
      case 'o': {
        // Over the whole duration, at one value per period; a regen aura
        // (food, drink: no period of its own) counts per 5 s, as the client
        // does. Floating point, then cut: 17 per 5 s for 18 s is 61, not 51
        // (review of f40b0b6, finding 16).
        const int32_t ms = info->GetDuration();
        const double period = e.Amplitude > 0 ? e.Amplitude : 5000.0;
        if (ms <= 0) {
          return std::nullopt;
        }
        const auto total = [&](int v) {
          return static_cast<int>(v * (ms / period));
        };
        return lo == hi ? base::NumberToString(total(lo))
                        : base::StringPrintf("%d to %d", total(lo), total(hi));
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
  // The effect's value range as the tooltip shows it, never a roll: the
  // base points with the level bonus SpellEffectInfo::CalcValue adds, plus
  // 1..die sides (a potion's "70 to 90"). Talent modifiers are left out, as
  // the client's own tooltip leaves them out.
  std::pair<int, int> Range(const SpellInfo& info,
                            const SpellEffectInfo& e) const {
    int base = e.BasePoints;
    if (e.RealPointsPerLevel != 0.0f) {
      int level = static_cast<int>(bot_->GetLevel());
      if (info.MaxLevel > 0 && level > static_cast<int>(info.MaxLevel)) {
        level = static_cast<int>(info.MaxLevel);
      } else if (level < static_cast<int>(info.BaseLevel)) {
        level = static_cast<int>(info.BaseLevel);
      }
      level -= static_cast<int>(std::max(info.BaseLevel, info.SpellLevel));
      base += static_cast<int>(level * e.RealPointsPerLevel);
    }
    const int dice = e.DieSides;
    int lo = base + (dice == 0 ? 0 : std::min(dice, 1));
    int hi = base + (dice == 0 ? 0 : std::max(dice, 1));
    lo = std::abs(lo);
    hi = std::abs(hi);
    return {std::min(lo, hi), std::max(lo, hi)};
  }

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
    if (const StatInfo* info = FindStat(p->ItemStat[i].ItemStatType);
        info && p->ItemStat[i].ItemStatValue != 0) {
      stats.push_back(
          {info->stat, static_cast<float>(p->ItemStat[i].ItemStatValue)});
      lines.push_back(StatLine(*info, p->ItemStat[i].ItemStatValue));
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
  // Read without Player::GetReqKillOrCastCurrentCount: its m_QuestStatus[id]
  // inserts an empty entry for a quest that has none (a rewarded one), which
  // then reads as NONE: offered again and never completable (review of
  // f40b0b6, finding 3).
  const QuestStatusMap& statuses = bot->getQuestStatusMap();
  const auto status = statuses.find(q->GetQuestId());
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
            status != statuses.end() ? status->second.CreatureOrGOCount[i] : 0,
            need),
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

std::string QuestObjectivesText(Player* bot, const Quest* q) {
  std::string race;
  if (const ChrRacesEntry* r = sChrRacesStore.LookupEntry(bot->getRace())) {
    if (const char* rn = r->name[sWorld->GetDefaultDbcLocale()]) {
      race = rn;
    }
  }
  return FormatQuestText(q->GetObjectives(), bot->GetName(),
                         ClassName(bot->getClass()), race,
                         bot->getGender() == GENDER_FEMALE);
}

void AddQuestTooltip(Player* bot, const Quest* q, pb::Tooltipped* out) {
  std::vector<std::string>& lines = out->tooltip.emplace().lines;
  lines.push_back(q->GetTitle());
  if (q->GetQuestLevel() > 0) {
    lines.push_back(base::StringPrintf("Level %d", q->GetQuestLevel()));
  }
  if (!q->GetObjectives().empty()) {
    lines.push_back(QuestObjectivesText(bot, q));
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
  for (uint8_t bag_pos = INVENTORY_SLOT_BAG_START;
       bag_pos < INVENTORY_SLOT_BAG_END; ++bag_pos) {
    if (const Bag* bag = bot->GetBagByPos(bag_pos); IsGeneralBag(bag)) {
      total += bag->GetBagSize();
    }
  }
  uint32_t used = 0;
  for (const BagSlotItem& entry : BagItems(bot)) {
    used += entry.general_bag ? 1 : 0;
    bags->items.push_back(BagItem(bot, entry.item, entry.bag, entry.slot));
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
    entry.description = QuestObjectivesText(bot, q);
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
    // In the quest log only: a rewarded quest isn't NONE, but it has no slot
    // (and no counters) any more.
    if (!q || bot->FindQuestSlot(id) >= MAX_QUEST_LOG_SIZE) {
      return std::nullopt;  // Not in the bot's quest log: nothing to point at.
    }
    AddQuestTooltip(bot, q, &out);
    return out;
  }
  return std::nullopt;
}

}  // namespace llm_brain
