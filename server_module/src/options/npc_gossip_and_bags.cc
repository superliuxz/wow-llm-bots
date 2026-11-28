// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/npc_gossip_and_bags.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "options/riding_mounts.h"

namespace llm_brain {
namespace {

uint64_t TotalStackCopper(uint32_t sell_price, uint32_t stack_count) {
  const uint64_t count = stack_count == 0 ? 1u : stack_count;
  return static_cast<uint64_t>(sell_price) * count;
}

}  // namespace

const char* ItemQualityShortName(uint32_t quality) {
  switch (quality) {
    case kItemQualityPoor:
      return "grey";
    case kItemQualityNormal:
      return "white";
    case kItemQualityUncommon:
      return "green";
    case kItemQualityRare:
      return "blue";
    case kItemQualityEpic:
      return "epic";
    case 5:
      return "legendary";
    case 6:
      return "artifact";
    case 7:
      return "heirloom";
    default:
      return "item";
  }
}

std::string SanitizeGossipText(std::string_view raw, size_t max_chars) {
  std::string cleaned;
  cleaned.reserve(raw.size());
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] == '|' && i + 1 < raw.size()) {
      const char next = raw[i + 1];
      if (next == 'c' || next == 'C') {
        // |cAARRGGBB color prefix (10 chars total).
        i += std::min<size_t>(10, raw.size() - i);
        continue;
      }
      if (next == 'r' || next == 'R' || next == 'h') {
        i += 2;
        continue;
      }
      if (next == 'H') {
        // |H...|h link payload; keep visible text after |h.
        const size_t end = raw.find("|h", i + 2);
        i = (end == std::string_view::npos) ? raw.size() : end + 2;
        continue;
      }
      if (next == 'T') {
        // |T...|t inline texture icon.
        const size_t end = raw.find("|t", i + 2);
        i = (end == std::string_view::npos) ? raw.size() : end + 2;
        continue;
      }
      if (next == '|') {
        cleaned.push_back('|');
        i += 2;
        continue;
      }
    }
    const unsigned char ch = static_cast<unsigned char>(raw[i]);
    if (ch == '\r' || ch == '\n' || ch == '\t' || ch < 0x20) {
      if (!cleaned.empty() && cleaned.back() != ' ') {
        cleaned.push_back(' ');
      }
    } else if (ch == ' ') {
      if (!cleaned.empty() && cleaned.back() != ' ') {
        cleaned.push_back(' ');
      }
    } else if (ch == '"') {
      cleaned.push_back('\'');
    } else {
      cleaned.push_back(static_cast<char>(ch));
    }
    ++i;
  }
  while (!cleaned.empty() && cleaned.back() == ' ') {
    cleaned.pop_back();
  }
  if (max_chars > 3 && cleaned.size() > max_chars) {
    cleaned.resize(max_chars - 3);
    while (!cleaned.empty() && cleaned.back() == ' ') {
      cleaned.pop_back();
    }
    cleaned += "...";
  }
  return cleaned;
}

bool IsSelectableGossipOption(const GossipOptionCandidate& c,
                              uint32_t bot_money_copper) {
  if (c.is_coded) {
    return false;
  }
  if (c.box_money > bot_money_copper) {
    return false;
  }
  if (c.sender == 0 && c.option_type != kGossipOptionGossip) {
    return false;
  }
  return !SanitizeGossipText(c.message).empty();
}

std::vector<GossipOptionChoice> SelectGossipOptions(
    uint32_t menu_id,
    const std::vector<GossipOptionCandidate>& candidates,
    uint32_t bot_money_copper,
    std::string_view npc_name,
    size_t max_options) {
  std::vector<const GossipOptionCandidate*> eligible;
  for (const GossipOptionCandidate& c : candidates) {
    if (IsSelectableGossipOption(c, bot_money_copper)) {
      eligible.push_back(&c);
    }
  }
  std::stable_sort(
      eligible.begin(), eligible.end(),
      [](const GossipOptionCandidate* a, const GossipOptionCandidate* b) {
        return a->list_id < b->list_id;
      });

  std::vector<GossipOptionChoice> out;
  for (const GossipOptionCandidate* c : eligible) {
    if (out.size() >= max_options) {
      break;
    }
    std::string text = SanitizeGossipText(c->message);
    if (text.empty()) {
      continue;
    }
    std::string label =
        "Say to " + std::string(npc_name) + ": \"" + text + "\"";
    if (c->box_money > 0) {
      label += " (" + FormatCopperCost(c->box_money) + ")";
    }
    out.push_back(GossipOptionChoice{
        .menu_id = menu_id,
        .list_id = c->list_id,
        .label = std::move(label),
        .sanitized_text = std::move(text),
    });
  }
  return out;
}

std::string ValidateGossipSelect(bool in_combat,
                                 bool npc_in_range,
                                 bool menu_matches_npc,
                                 bool menu_id_matches,
                                 bool item_exists,
                                 bool is_coded,
                                 uint32_t box_money,
                                 uint32_t bot_money) {
  if (in_combat) {
    return "you are in combat";
  }
  if (!npc_in_range) {
    return "that NPC is not in talk range";
  }
  if (!menu_matches_npc || !menu_id_matches || !item_exists) {
    return "that dialogue option is no longer open";
  }
  if (is_coded) {
    return "that dialogue option requires a text code";
  }
  if (box_money > bot_money) {
    return "not enough money";
  }
  return {};
}

std::string DestroySafetyBlockReason(const DestroyItemCandidate& c) {
  if (c.entry == 0 || c.name.empty()) {
    return "invalid item";
  }
  if (c.is_equipped_or_bag_slot) {
    return "cannot destroy equipped item or bag";
  }
  if (!c.in_general_bag) {
    return "item is not in a general bag";
  }
  if (c.entry == kHearthstoneEntry) {
    return "cannot destroy hearthstone";
  }
  if ((c.flags & kItemFlagNoUserDestroy) != 0) {
    return "item cannot be destroyed";
  }
  if (c.item_class == kItemClassQuest || c.bonding == kItemBondQuest ||
      c.bonding == kItemBondQuest1 || c.start_quest != 0 ||
      c.required_for_active_quest) {
    return "cannot destroy quest item";
  }
  if (c.item_class == kItemClassContainer || c.item_class == kItemClassQuiver ||
      c.item_class == kItemClassKey || c.item_class == kItemClassProjectile ||
      c.item_class == kItemClassReagent) {
    return "cannot destroy protected item type";
  }
  if (c.quality > kItemQualityRare) {
    return "cannot destroy epic or higher quality item";
  }
  return {};
}

std::vector<DestroyItemOption> SelectDestroyItemOptions(
    uint32_t free_bag_slots,
    bool alive,
    bool in_combat,
    bool in_trade,
    const std::vector<DestroyItemCandidate>& candidates,
    size_t max_options) {
  if (free_bag_slots > kDestroyOfferMaxFreeSlots || !alive || in_combat ||
      in_trade || max_options == 0) {
    return {};
  }
  std::vector<const DestroyItemCandidate*> safe;
  for (const DestroyItemCandidate& c : candidates) {
    if (DestroySafetyBlockReason(c).empty()) {
      safe.push_back(&c);
    }
  }
  std::stable_sort(
      safe.begin(), safe.end(),
      [](const DestroyItemCandidate* a, const DestroyItemCandidate* b) {
        if (a->quality != b->quality) {
          return a->quality < b->quality;
        }
        const uint64_t val_a = TotalStackCopper(a->sell_price, a->stack_count);
        const uint64_t val_b = TotalStackCopper(b->sell_price, b->stack_count);
        if (val_a != val_b) {
          return val_a < val_b;
        }
        if (a->entry != b->entry) {
          return a->entry < b->entry;
        }
        if (a->bag != b->bag) {
          return a->bag < b->bag;
        }
        return a->slot < b->slot;
      });

  std::vector<DestroyItemOption> out;
  std::vector<uint32_t> seen_entries;
  for (const DestroyItemCandidate* c : safe) {
    if (out.size() >= max_options) {
      break;
    }
    if (std::find(seen_entries.begin(), seen_entries.end(), c->entry) !=
        seen_entries.end()) {
      continue;
    }
    seen_entries.push_back(c->entry);
    const uint64_t total_copper =
        TotalStackCopper(c->sell_price, c->stack_count);
    const std::string value_text =
        total_copper == 0
            ? "no value"
            : FormatCopperCost(static_cast<uint32_t>(std::min<uint64_t>(
                  total_copper, std::numeric_limits<uint32_t>::max())));
    std::string label = "Destroy " + c->name;
    if (c->stack_count > 1) {
      label += " x" + std::to_string(c->stack_count);
    }
    label += " (";
    label += ItemQualityShortName(c->quality);
    label += ", " + value_text + ")";

    std::string guid = "destroy:" + std::to_string(c->bag) + ":" +
                       std::to_string(c->slot) + ":" + std::to_string(c->entry);
    out.push_back(DestroyItemOption{
        .bag = c->bag,
        .slot = c->slot,
        .entry = c->entry,
        .guid = std::move(guid),
        .label = std::move(label),
    });
  }
  return out;
}

std::string VendorSellSafetyBlockReason(const VendorSellCandidate& c) {
  if (c.entry == 0 || c.name.empty()) {
    return "invalid item";
  }
  if (c.is_equipped_or_bag_slot) {
    return "cannot sell equipped item or bag";
  }
  if (c.is_non_empty_bag) {
    return "cannot sell non-empty bag";
  }
  if (c.is_refundable) {
    return "cannot sell refundable item";
  }
  if (c.sell_price == 0) {
    return "vendor will not buy that item";
  }
  if (c.entry == kHearthstoneEntry) {
    return "cannot sell hearthstone";
  }
  if (c.item_class == kItemClassQuest || c.bonding == kItemBondQuest ||
      c.bonding == kItemBondQuest1 || c.start_quest != 0 ||
      c.required_for_active_quest) {
    return "cannot sell quest item";
  }
  if (c.item_class == kItemClassKey || c.item_class == kItemClassReagent ||
      c.item_class == kItemClassProjectile ||
      c.item_class == kItemClassQuiver) {
    return "cannot sell protected item type";
  }
  return {};
}

std::vector<VendorSellOption> SelectVendorSellOptions(
    std::string_view npc_guid,
    std::string_view npc_name,
    const std::vector<VendorSellCandidate>& candidates,
    size_t max_options) {
  if (npc_guid.empty() || npc_name.empty() || max_options == 0) {
    return {};
  }
  std::vector<const VendorSellCandidate*> eligible;
  for (const VendorSellCandidate& c : candidates) {
    if (c.quality > kItemQualityPoor &&
        VendorSellSafetyBlockReason(c).empty()) {
      eligible.push_back(&c);
    }
  }
  std::stable_sort(
      eligible.begin(), eligible.end(),
      [](const VendorSellCandidate* a, const VendorSellCandidate* b) {
        if (a->quality != b->quality) {
          return a->quality < b->quality;
        }
        const uint64_t val_a = TotalStackCopper(a->sell_price, a->stack_count);
        const uint64_t val_b = TotalStackCopper(b->sell_price, b->stack_count);
        if (val_a != val_b) {
          return val_a < val_b;
        }
        if (a->entry != b->entry) {
          return a->entry < b->entry;
        }
        if (a->bag != b->bag) {
          return a->bag < b->bag;
        }
        return a->slot < b->slot;
      });

  std::vector<VendorSellOption> out;
  std::vector<uint32_t> seen_entries;
  for (const VendorSellCandidate* c : eligible) {
    if (out.size() >= max_options) {
      break;
    }
    if (std::find(seen_entries.begin(), seen_entries.end(), c->entry) !=
        seen_entries.end()) {
      continue;
    }
    seen_entries.push_back(c->entry);
    const uint64_t total_copper =
        TotalStackCopper(c->sell_price, c->stack_count);
    const std::string value_text =
        FormatCopperCost(static_cast<uint32_t>(std::min<uint64_t>(
            total_copper, std::numeric_limits<uint32_t>::max())));
    std::string label = "Sell " + c->name;
    if (c->stack_count > 1) {
      label += " x" + std::to_string(c->stack_count);
    }
    label += " to " + std::string(npc_name) + " (";
    label += ItemQualityShortName(c->quality);
    if ((c->item_class == kItemClassArmor ||
         c->item_class == kItemClassWeapon) &&
        c->item_level > 0) {
      label += ", ilvl " + std::to_string(c->item_level);
    }
    label += ", " + value_text + ")";

    std::string action_guid =
        "npc_sell:" + std::string(npc_guid) + ":" + std::to_string(c->bag) +
        ":" + std::to_string(c->slot) + ":" + std::to_string(c->entry);
    out.push_back(VendorSellOption{
        .bag = c->bag,
        .slot = c->slot,
        .entry = c->entry,
        .action_guid = std::move(action_guid),
        .label = std::move(label),
    });
  }
  return out;
}

std::optional<BagEquipTarget> PickBagEquipSlot(
    uint32_t new_bag_slots,
    uint8_t source_bag,
    bool can_relocate_from_source_bag,
    const std::vector<EquippedBagSlotInfo>& equipped_slots) {
  if (new_bag_slots == 0) {
    return std::nullopt;
  }
  // 1. Prefer the first empty bag slot.
  for (const EquippedBagSlotInfo& s : equipped_slots) {
    if (!s.occupied) {
      return BagEquipTarget{
          .dest_slot = s.slot,
          .replaces_existing = false,
          .replaced_slots = 0,
          .replaced_name = "",
          .needs_relocate = false,
      };
    }
  }
  // 2. Otherwise pick the smallest occupied general bag strictly smaller than
  //    `new_bag_slots` that is not the bag currently holding this item.
  const EquippedBagSlotInfo* best = nullptr;
  for (const EquippedBagSlotInfo& s : equipped_slots) {
    if (!s.occupied || !s.is_general_bag || s.slot == source_bag ||
        s.container_slots >= new_bag_slots || s.used_slots > new_bag_slots) {
      continue;
    }
    if (!best || s.container_slots < best->container_slots ||
        (s.container_slots == best->container_slots && s.slot < best->slot)) {
      best = &s;
    }
  }
  if (best) {
    return BagEquipTarget{
        .dest_slot = best->slot,
        .replaces_existing = true,
        .replaced_slots = best->container_slots,
        .replaced_name = best->name,
        .needs_relocate = false,
    };
  }
  // 3. Fall back to replacing `source_bag` itself only if the item can be
  //    relocated out of `source_bag` before swapping.
  if (can_relocate_from_source_bag) {
    for (const EquippedBagSlotInfo& s : equipped_slots) {
      if (s.occupied && s.is_general_bag && s.slot == source_bag &&
          s.container_slots < new_bag_slots && s.used_slots <= new_bag_slots) {
        return BagEquipTarget{
            .dest_slot = s.slot,
            .replaces_existing = true,
            .replaced_slots = s.container_slots,
            .replaced_name = s.name,
            .needs_relocate = true,
        };
      }
    }
  }
  return std::nullopt;
}

std::string FormatBagEquipDetail(uint32_t new_bag_slots,
                                 const BagEquipTarget& target) {
  std::string detail = std::to_string(new_bag_slots) + "-slot bag, ";
  if (!target.replaces_existing) {
    detail += "empty bag slot";
  } else {
    detail += "replaces " + std::to_string(target.replaced_slots) + "-slot ";
    detail += target.replaced_name.empty() ? "bag" : target.replaced_name;
  }
  return detail;
}

std::vector<BagPurchaseOption> SelectBagPurchases(
    const std::vector<BagVendorCandidate>& candidates,
    uint32_t bot_money_copper,
    uint32_t free_bag_slots,
    const std::vector<EquippedBagSlotInfo>& equipped_slots,
    std::string_view npc_name,
    size_t max_options) {
  if (npc_name.empty() || max_options == 0 || equipped_slots.empty()) {
    return {};
  }
  bool has_empty_slot = false;
  uint32_t min_replaceable = std::numeric_limits<uint32_t>::max();
  for (const EquippedBagSlotInfo& s : equipped_slots) {
    if (!s.occupied) {
      has_empty_slot = true;
      min_replaceable = 0;
      break;
    }
    if (s.is_general_bag && s.container_slots < min_replaceable) {
      min_replaceable = s.container_slots;
    }
  }
  if (min_replaceable == std::numeric_limits<uint32_t>::max()) {
    return {};
  }
  // If all bag slots are occupied and inventory has no free slot, a purchased
  // bag cannot be stored in inventory (whereas an empty bag slot can receive a
  // bought bag directly).
  if (!has_empty_slot && free_bag_slots == 0) {
    return {};
  }

  std::vector<const BagVendorCandidate*> eligible;
  for (const BagVendorCandidate& c : candidates) {
    if (c.item_id == 0 || c.name.empty() || !c.meets_requirements ||
        c.container_slots == 0 || c.container_slots <= min_replaceable ||
        c.price_copper > bot_money_copper) {
      continue;
    }
    eligible.push_back(&c);
  }
  std::stable_sort(
      eligible.begin(), eligible.end(),
      [](const BagVendorCandidate* a, const BagVendorCandidate* b) {
        if (a->container_slots != b->container_slots) {
          return a->container_slots > b->container_slots;
        }
        if (a->price_copper != b->price_copper) {
          return a->price_copper < b->price_copper;
        }
        return a->item_id < b->item_id;
      });

  std::vector<BagPurchaseOption> out;
  std::vector<uint32_t> seen_sizes;
  for (const BagVendorCandidate* c : eligible) {
    if (out.size() >= max_options) {
      break;
    }
    if (std::find(seen_sizes.begin(), seen_sizes.end(), c->container_slots) !=
        seen_sizes.end()) {
      continue;
    }
    seen_sizes.push_back(c->container_slots);
    std::string label = "Buy " + c->name + " (" +
                        std::to_string(c->container_slots) + "-slot bag";
    if (!has_empty_slot) {
      label += " vs " + std::to_string(min_replaceable) + "-slot";
    }
    if (c->price_copper > 0) {
      label += ", " + FormatCopperCost(c->price_copper);
    }
    label += ") from " + std::string(npc_name);
    out.push_back(BagPurchaseOption{
        .item_id = c->item_id,
        .container_slots = c->container_slots,
        .price_copper = c->price_copper,
        .label = std::move(label),
    });
  }
  return out;
}

}  // namespace llm_brain
