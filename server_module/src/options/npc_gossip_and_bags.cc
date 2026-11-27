// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/npc_gossip_and_bags.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "options/riding_mounts.h"

namespace llm_brain {
namespace {

uint64_t TotalStackCopper(const DestroyItemCandidate& c) {
  const uint64_t count = c.stack_count == 0 ? 1u : c.stack_count;
  return static_cast<uint64_t>(c.sell_price) * count;
}

}  // namespace

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
      c.item_class == kItemClassKey || c.item_class == kItemClassProjectile) {
    return "cannot destroy protected item type";
  }
  if (c.quality > kItemQualityNormal) {
    return "cannot destroy uncommon or higher quality item";
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
  if (free_bag_slots > 0 || !alive || in_combat || in_trade ||
      max_options == 0) {
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
        const uint64_t val_a = TotalStackCopper(*a);
        const uint64_t val_b = TotalStackCopper(*b);
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
    const uint64_t total_copper = TotalStackCopper(*c);
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
    label += (c->quality == kItemQualityPoor ? "grey" : "white");
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

}  // namespace llm_brain
