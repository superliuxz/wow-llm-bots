// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/world_util.h"

#include <cstdint>
#include <string>
#include <vector>

#include "Bag.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "StringFormat.h"
#include "UnitDefines.h"
#include "World.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

std::string AreaName(uint32_t area_id) {
  const AreaTableEntry* area = sAreaTableStore.LookupEntry(area_id);
  if (!area) {
    return "";
  }
  const char* name = area->area_name[sWorld->GetDefaultDbcLocale()];
  return name ? name : "";
}

std::string GuidString(uint64_t raw_guid) {
  return Acore::StringFormat("0x{:016X}", raw_guid);
}

bool IsGeneralBag(const Bag* bag) {
  const ItemTemplate* p = bag ? bag->GetTemplate() : nullptr;
  return p && p->Class == ITEM_CLASS_CONTAINER &&
         p->SubClass == ITEM_SUBCLASS_CONTAINER;
}

std::vector<BagSlotItem> BagItems(Player* bot) {
  std::vector<BagSlotItem> out;
  for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END;
       ++slot) {
    if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) {
      out.push_back({item, INVENTORY_SLOT_BAG_0, slot, true});
    }
  }
  for (uint8_t bag_pos = INVENTORY_SLOT_BAG_START;
       bag_pos < INVENTORY_SLOT_BAG_END; ++bag_pos) {
    Bag* bag = bot->GetBagByPos(bag_pos);
    if (!bag) {
      continue;
    }
    const bool general = IsGeneralBag(bag);
    for (uint32_t slot = 0; slot < bag->GetBagSize(); ++slot) {
      if (Item* item = bot->GetItemByPos(bag_pos, static_cast<uint8_t>(slot))) {
        out.push_back({item, bag_pos, static_cast<uint8_t>(slot), general});
      }
    }
  }
  return out;
}

wlb::snapshot::Self::Control ReadControlState(const Player* bot) {
  wlb::snapshot::Self::Control control;
  control.stunned = bot->HasUnitState(UNIT_STATE_STUNNED) ||
                    bot->HasUnitFlag(UNIT_FLAG_STUNNED);
  control.feared =
      bot->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) ||
      bot->HasUnitFlag(UNIT_FLAG_FLEEING | UNIT_FLAG_CONFUSED);
  control.charmed =
      bot->HasUnitState(UNIT_STATE_CHARMED | UNIT_STATE_POSSESSED) ||
      !bot->GetCharmerGUID().IsEmpty();
  control.silenced = bot->HasUnitFlag(UNIT_FLAG_SILENCED);
  control.rooted = bot->HasUnitState(UNIT_STATE_ROOT);
  control.pacified = bot->HasUnitFlag(UNIT_FLAG_PACIFIED);
  control.disarmed = bot->HasUnitFlag(UNIT_FLAG_DISARMED);
  return control;
}

uint8_t ControlMask(const Player* bot) {
  const wlb::snapshot::Self::Control c = ReadControlState(bot);
  return static_cast<uint8_t>(
      (c.stunned ? 1u << 0 : 0u) | (c.feared ? 1u << 1 : 0u) |
      (c.charmed ? 1u << 2 : 0u) | (c.silenced ? 1u << 3 : 0u) |
      (c.rooted ? 1u << 4 : 0u) | (c.pacified ? 1u << 5 : 0u) |
      (c.disarmed ? 1u << 6 : 0u));
}

}  // namespace llm_brain
