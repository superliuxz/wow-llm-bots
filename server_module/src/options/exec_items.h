// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Item, equipment, loot, object interaction, rest, and talent discovery &
// execution.

#ifndef MOD_LLM_BRAIN_OPTIONS_EXEC_ITEMS_H_
#define MOD_LLM_BRAIN_OPTIONS_EXEC_ITEMS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "options/action_types.h"
#include "options/exec_shared.h"
#include "snapshot/snapshot.h"

class Creature;
class Player;

namespace llm_brain {

struct ItemActionOutcome {
  std::string error;
  bool used = false;
  std::string info;
  uint32_t pending_spell_id = 0;
};

bool HasFoodOrDrinkAura(Player* bot);
void CheckBagFoodAndDrink(Player* bot, bool* has_food, bool* has_drink);
void ContinueRest(Player* bot);
bool IsRestInProgress(Player* bot, bool* had_food_drink_aura);

void CollectUsableBagItems(Player* bot,
                           std::vector<wlb::snapshot::OptionFacts_Item>* out);
void CollectBagDestroyActions(
    Player* bot,
    uint32_t free_bag_slots,
    std::vector<wlb::snapshot::OptionFacts_Action>* out);
void CollectTalentTrees(
    Player* bot,
    std::vector<wlb::snapshot::OptionFacts_TalentTree>* out);

bool HasTakeableLoot(Player* bot, Creature* creature);
std::string LootCorpseNow(Player* bot, Creature* creature);

ActionOutcome ExecuteLootAction(Player* bot, const LootAction& action);
ActionOutcome ExecuteObjectAction(Player* bot,
                                  const std::string& guid,
                                  const std::string& name);
ActionOutcome ExecuteRestAction(Player* bot);
ItemActionOutcome ExecuteUseBagItemAction(Player* bot,
                                          const UseBagItemAction& action);
ActionOutcome ExecuteEquipItemAction(Player* bot,
                                     const EquipItemAction& action);
ActionOutcome ExecuteDestroyItemAction(Player* bot,
                                       const DestroyItemAction& action);
ActionOutcome ExecuteSpendTalentsAction(Player* bot,
                                        const SpendTalentsAction& action);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_EXEC_ITEMS_H_
