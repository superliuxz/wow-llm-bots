// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure NPC gossip dialogue filtering/formatting and full-bag safe item
// destruction helpers (no AzerothCore headers, unit-tested directly).

#ifndef MOD_LLM_BRAIN_OPTIONS_NPC_GOSSIP_AND_BAGS_H_
#define MOD_LLM_BRAIN_OPTIONS_NPC_GOSSIP_AND_BAGS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llm_brain {

// Mirrors AzerothCore's Gossip_Option::GOSSIP_OPTION_GOSSIP (GossipDef.h).
inline constexpr uint32_t kGossipOptionGossip = 1;

// Mirrors AzerothCore's ItemQuality, ItemClass, ItemBondingType, and ItemFlags
// (ItemTemplate.h / SharedDefines.h). Static-asserted in exec_items.cc.
inline constexpr uint32_t kItemQualityPoor = 0;
inline constexpr uint32_t kItemQualityNormal = 1;
inline constexpr uint32_t kItemClassContainer = 1;
inline constexpr uint32_t kItemClassProjectile = 6;
inline constexpr uint32_t kItemClassQuiver = 11;
inline constexpr uint32_t kItemClassQuest = 12;
inline constexpr uint32_t kItemClassKey = 13;
inline constexpr uint32_t kItemBondQuest = 4;
inline constexpr uint32_t kItemBondQuest1 = 5;
inline constexpr uint32_t kItemFlagNoUserDestroy = 0x00000020u;
inline constexpr uint32_t kHearthstoneEntry = 6948u;

// Strips WoW UI escape codes (`|cXXXXXXXX`, `|r`, `|T...|t`, `|H...|h`, `|h`),
// collapses control characters and whitespace into single spaces, trims, and
// truncates to `max_chars` (with "..." when truncated).
std::string SanitizeGossipText(std::string_view raw, size_t max_chars = 80);

struct GossipOptionCandidate {
  uint32_t list_id = 0;
  uint32_t sender = 0;
  uint32_t option_type = 0;
  std::string message;
  uint32_t box_money = 0;
  bool is_coded = false;
};

struct GossipOptionChoice {
  uint32_t menu_id = 0;
  uint32_t list_id = 0;
  std::string label;
  std::string sanitized_text;
};

// True when `c` is a mechanically selectable dialogue option for the brain:
// non-empty after sanitization, not password-coded, affordable, and either a
// DB dialogue entry (`sender == 0 && option_type == kGossipOptionGossip`) or a
// scripted gossip entry (`sender != 0`).
bool IsSelectableGossipOption(const GossipOptionCandidate& c,
                              uint32_t bot_money_copper);

// Filters and formats up to `max_options` selectable gossip menu options for
// `npc_name` (in ascending `list_id` order).
std::vector<GossipOptionChoice> SelectGossipOptions(
    uint32_t menu_id,
    const std::vector<GossipOptionCandidate>& candidates,
    uint32_t bot_money_copper,
    std::string_view npc_name,
    size_t max_options = 8);

// Pure execution-time validation before sending CMSG_GOSSIP_SELECT_OPTION.
// Returns empty string when valid, else a concise error message.
std::string ValidateGossipSelect(bool in_combat,
                                 bool npc_in_range,
                                 bool menu_matches_npc,
                                 bool menu_id_matches,
                                 bool item_exists,
                                 bool is_coded,
                                 uint32_t box_money,
                                 uint32_t bot_money);

struct DestroyItemCandidate {
  uint8_t bag = 0;
  uint8_t slot = 0;
  uint32_t entry = 0;
  std::string name;
  uint32_t quality = 0;
  uint32_t item_class = 0;
  uint32_t bonding = 0;
  uint32_t start_quest = 0;
  uint32_t flags = 0;
  uint32_t sell_price = 0;
  uint32_t stack_count = 1;
  bool in_general_bag = true;
  bool is_equipped_or_bag_slot = false;
  bool required_for_active_quest = false;
};

struct DestroyItemOption {
  uint8_t bag = 0;
  uint8_t slot = 0;
  uint32_t entry = 0;
  std::string guid;
  std::string label;
};

// Enforces C++ safety invariants for item destruction: never equipped gear or
// bags, never outside general bags, never hearthstone, never no-user-destroy,
// never quest or active-quest-required items, never containers/quivers/keys/
// projectiles, and never uncommon (green) or higher quality items. Returns
// empty string when safe to offer/destroy, else a concise reason.
std::string DestroySafetyBlockReason(const DestroyItemCandidate& c);

// Selects up to `max_options` lowest-value safe items to offer for brain-chosen
// destruction when `free_bag_slots == 0`, alive, out of combat, and not in a
// trade window. Prefers grey (poor) over white (common), then lowest total
// stack vendor value, deduplicating by item entry.
std::vector<DestroyItemOption> SelectDestroyItemOptions(
    uint32_t free_bag_slots,
    bool alive,
    bool in_combat,
    bool in_trade,
    const std::vector<DestroyItemCandidate>& candidates,
    size_t max_options = 4);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_NPC_GOSSIP_AND_BAGS_H_
