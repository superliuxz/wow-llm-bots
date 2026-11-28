// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/popup_watch.h"

#include <algorithm>
#include <cstdint>
#include <optional>
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
  for (const PopupState::SharedQuest& sq : now.shared_quests) {
    if (sq.quest_id == 0) {
      continue;
    }
    const bool seen =
        std::any_of(last_.shared_quests.begin(), last_.shared_quests.end(),
                    [&](const PopupState::SharedQuest& prev) {
                      return prev.quest_id == sq.quest_id;
                    });
    if (!seen) {
      out.push_back(Prompt(pb::PromptEvent::QUEST_SHARE, sq.from, sq.title));
    }
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

bool HasTimedPopup(const PopupState& state, bool has_group_invite) {
  return has_group_invite || state.ready_check || state.role_check ||
         state.lfg_proposal_id != 0 || !state.shared_quests.empty() ||
         state.resurrect_offer || state.summon || !state.boot_victim.empty() ||
         !state.rolls.empty();
}

bool UpdatePopupDeadline(std::optional<uint64_t>* open_since_ms,
                         bool has_timed_popup,
                         uint64_t now_ms,
                         uint32_t deadline_ms) {
  if (!has_timed_popup) {
    open_since_ms->reset();
    return false;
  }
  if (!open_since_ms->has_value()) {
    *open_since_ms = now_ms;
  }
  const uint64_t opened_at = **open_since_ms;
  return now_ms >= opened_at && (now_ms - opened_at) >= deadline_ms;
}

}  // namespace llm_brain
