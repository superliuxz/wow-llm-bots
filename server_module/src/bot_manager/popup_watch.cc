// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/popup_watch.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

pb::Event Prompt(pb::PromptEvent::Kind kind,
                 std::string from = {},
                 std::string text = {},
                 float timeout_s = 0.0f) {
  pb::Event ev;
  pb::PromptEvent& prompt = ev.prompt.emplace();
  prompt.kind = kind;
  prompt.from = std::move(from);
  prompt.text = std::move(text);
  prompt.timeout_s = timeout_s;
  return ev;
}

}  // namespace

std::vector<pb::Event> PopupWatch::Update(const PopupState& now) {
  std::vector<pb::Event> out;
  if (now.ready_check && !last_.ready_check) {
    out.push_back(Prompt(pb::PromptEvent::READY_CHECK, {}, {}, 30.0f));
  }
  if (now.role_check && !last_.role_check) {
    out.push_back(Prompt(pb::PromptEvent::ROLE_CHECK));
  }
  if (now.lfg_proposal_id != 0 &&
      now.lfg_proposal_id != last_.lfg_proposal_id) {
    out.push_back(Prompt(pb::PromptEvent::LFG_PROPOSAL, {}, {}, 45.0f));
  }
  if (now.shared_quest_id != 0 &&
      now.shared_quest_id != last_.shared_quest_id) {
    out.push_back(Prompt(pb::PromptEvent::QUEST_SHARE, now.shared_quest_from,
                         now.shared_quest_title));
  }
  if (now.resurrect_offer && !last_.resurrect_offer) {
    out.push_back(Prompt(pb::PromptEvent::RESURRECT_OFFER));
  }
  if (now.summon && !last_.summon) {
    out.push_back(Prompt(pb::PromptEvent::SUMMON, now.summon_from, {}, 120.0f));
  }
  if (!now.boot_victim.empty() && now.boot_victim != last_.boot_victim) {
    out.push_back(Prompt(pb::PromptEvent::LFG_BOOT_VOTE, {},
                         now.boot_reason.empty()
                             ? now.boot_victim
                             : now.boot_victim + ": " + now.boot_reason,
                         60.0f));
  }
  for (const PopupState::Roll& roll : now.rolls) {
    const bool seen = std::any_of(
        last_.rolls.begin(), last_.rolls.end(), [&](const PopupState::Roll& r) {
          return r.item_guid == roll.item_guid && r.item_slot == roll.item_slot;
        });
    if (seen) {
      continue;
    }
    pb::Event ev;
    pb::LootRollEvent& lr = ev.loot_roll.emplace();
    pb::Item& item = lr.item.emplace();
    item.id = roll.item_id;
    item.name = roll.item_name;
    out.push_back(std::move(ev));
  }
  // A trade window opening, and the other side pressing Trade (the bot's
  // own press then completes the trade).
  if (!now.trader.empty() && now.trader != last_.trader) {
    out.push_back(Prompt(pb::PromptEvent::TRADE, now.trader, "window open"));
  } else if (!now.trader.empty() && now.trader_accepted &&
             !last_.trader_accepted) {
    out.push_back(Prompt(pb::PromptEvent::TRADE, now.trader, "they accepted"));
  }
  last_ = now;
  return out;
}

void PopupWatch::Reset() {
  last_ = PopupState{};
}

}  // namespace llm_brain
