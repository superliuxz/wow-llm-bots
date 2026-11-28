// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure upkeep helpers for stack vendor buys, low-stock food/drink restocking,
// WotLK 3.3.5a class reagents & rogue poisons, hunter ammo auto-equip, and
// grouped errand validation (no AzerothCore headers; unit-tested directly).

#ifndef MOD_LLM_BRAIN_OPTIONS_UPKEEP_HELPERS_H_
#define MOD_LLM_BRAIN_OPTIONS_UPKEEP_HELPERS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llm_brain {

// Mirrors AzerothCore's Classes enum (SharedDefines.h). Static-asserted in
// exec_npc.cc.
inline constexpr uint8_t kClassWarrior = 1;
inline constexpr uint8_t kClassPaladin = 2;
inline constexpr uint8_t kClassHunter = 3;
inline constexpr uint8_t kClassRogue = 4;
inline constexpr uint8_t kClassPriest = 5;
inline constexpr uint8_t kClassDeathKnight = 6;
inline constexpr uint8_t kClassShaman = 7;
inline constexpr uint8_t kClassMage = 8;
inline constexpr uint8_t kClassWarlock = 9;
inline constexpr uint8_t kClassDruid = 11;

// Restock thresholds & target bag counts.
inline constexpr uint32_t kFoodRestockThreshold = 10;
inline constexpr uint32_t kFoodTargetStock = 20;
inline constexpr uint32_t kDrinkRestockThreshold = 10;
inline constexpr uint32_t kDrinkTargetStock = 20;
inline constexpr uint32_t kAmmoRestockThreshold = 400;
inline constexpr uint32_t kAmmoTargetStock = 1000;
inline constexpr uint32_t kReagentRestockThreshold = 5;
inline constexpr uint32_t kReagentTargetStock = 20;
inline constexpr uint32_t kSymbolOfKingsRestockThreshold = 20;
inline constexpr uint32_t kSymbolOfKingsTargetStock = 100;
inline constexpr uint32_t kPoisonRestockThreshold = 5;
inline constexpr uint32_t kPoisonTargetStock = 20;

struct StackBuyRequest {
  uint32_t current_count = 0;      // Items already in bags.
  uint32_t target_stock = 20;      // Desired total stock in bags.
  uint32_t buy_count = 1;          // Items per vendor purchase unit (BuyCount).
  uint32_t max_stack = 20;         // Max stack size for one bag slot.
  uint32_t unit_price_copper = 0;  // Discounted copper per purchase unit.
  uint32_t bot_money_copper = 0;   // Bot's current copper.
  uint32_t vendor_stock = 0;       // 0 = unlimited; else vendor item count.
  uint32_t max_storable_items = 0;  // 0 = uncapped; else max items bags fit.
};

struct StackBuyPlan {
  uint8_t purchase_units =
      0;                      // Count passed to BuyItemFromVendorSlot (1..255).
  uint32_t total_items = 0;   // purchase_units * buy_count.
  uint32_t total_copper = 0;  // purchase_units * unit_price_copper.
};

// Computes how many vendor purchase units to buy to restock toward
// `req.target_stock` (capped at one full stack, affordable copper, vendor
// limited stock, and available bag space). Returns `{0, 0, 0}` if already at
// target stock or unable to buy even 1 purchase unit.
StackBuyPlan ComputeStackBuyPlan(const StackBuyRequest& req);

// Formats a vendor buy option label such as
// "Buy Tough Jerky x20 (1s) from Vendor" (omitting xN for 1 item and cost for
// free items).
std::string FormatStackBuyLabel(std::string_view item_name,
                                std::string_view npc_name,
                                const StackBuyPlan& plan);

// True when food/drink bag stock is low enough to offer a vendor restock (not
// only when count == 0).
bool NeedsFoodRestock(uint32_t food_count);
bool NeedsDrinkRestock(uint32_t drink_count, bool uses_mana);

struct ClassReagentSpec {
  uint32_t item_id = 0;
  uint32_t restock_threshold = kReagentRestockThreshold;
  uint32_t target_stock = kReagentTargetStock;
};

// Returns WotLK 3.3.5a vendor-buyable class reagents appropriate for
// (`class_id`, `level`): Druid Rebirth seeds & Gift of the Wild reagents,
// Shaman Ankh, Mage Runes & Arcane Powder, Paladin Symbols, Priest Candles,
// Death Knight Corpse Dust.
std::vector<ClassReagentSpec> ClassReagentSpecsFor(uint8_t class_id,
                                                   uint32_t level);

enum class RoguePoisonFamily : uint8_t {
  kNone = 0,
  kInstant,
  kDeadly,
  kWound,
  kCrippling,
  kMindNumbing,
  kAnesthetic,
};

// Classifies a WotLK 3.3.5a Rogue poison item by its name prefix.
RoguePoisonFamily ClassifyRoguePoison(std::string_view item_name);

struct HunterAmmoCandidate {
  uint32_t item_id = 0;
  uint32_t item_level = 0;
  uint32_t count = 0;
  bool matches_ranged_weapon = false;
  bool usable = false;
};

// Picks the best matching, usable ammo item ID in bags to equip via
// `Player::SetAmmo`, or 0 if the hunter already has optimal ammo loaded or no
// valid ammo is available.
uint32_t PickHunterAmmoToEquip(
    bool is_hunter,
    bool alive,
    uint32_t current_ammo_id,
    uint32_t current_ammo_count,
    uint32_t current_ammo_ilvl,
    bool current_ammo_matches_ranged,
    const std::vector<HunterAmmoCandidate>& bag_ammo);

// Pure validation for leader-triggered upkeep errands (`go repair`, `go
// train`). Returns empty string when allowed, else a concise rejection reason.
std::string ValidateLeaderErrandCommand(bool alive,
                                        bool in_combat,
                                        bool party_in_combat,
                                        bool in_battleground);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_UPKEEP_HELPERS_H_
