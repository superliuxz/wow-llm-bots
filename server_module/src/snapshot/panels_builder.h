// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_PANELS_BUILDER_H_
#define MOD_LLM_BRAIN_SNAPSHOT_PANELS_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "snapshot/snapshot.h"

class Player;

namespace llm_brain {

// The client's text for item spells ("Use: Restores $o1 health over $d."):
// AzerothCore doesn't load spell descriptions, so they are read once from
// Spell.dbc in the server's DataDir at startup, for the spells items cast.
class ItemSpellTexts {
 public:
  // Null if Spell.dbc can't be read (tooltips then name the spell instead).
  static std::unique_ptr<ItemSpellTexts> Load();

  ItemSpellTexts(const ItemSpellTexts&) = delete;
  ItemSpellTexts& operator=(const ItemSpellTexts&) = delete;
  ~ItemSpellTexts();

  // The raw description (tokens not filled in), or null.
  const std::string* Find(uint32_t spell_id) const;
  size_t size() const { return texts_.size(); }

 private:
  explicit ItemSpellTexts(std::unordered_map<uint32_t, std::string> texts);

  std::unordered_map<uint32_t, std::string> texts_;
};

// The open panels of docs/plans/options-and-panels.md, as the client shows
// them. World thread only.
void FillBags(Player* bot, wlb::snapshot::Bags* bags);
void FillCharacter(Player* bot, wlb::snapshot::Character* character);
void FillQuestLog(Player* bot, wlb::snapshot::QuestLog* log);

// The tooltip of the thing `ref` points at (an item by entry id, a quest by
// id), or nullopt if there is no such thing for this bot. `texts` may be null.
std::optional<wlb::snapshot::Tooltipped> BuildTooltip(
    Player* bot,
    const wlb::snapshot::HoverRef& ref,
    const ItemSpellTexts* texts);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_PANELS_BUILDER_H_
