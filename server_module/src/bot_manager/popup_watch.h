// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_POPUP_WATCH_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_POPUP_WATCH_H_

#include <cstdint>
#include <string>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

// The timed popups a bot's client would show right now. BrainPolicyListener
// blocks playerbots' auto-answers to these, so the brain must hear about them
// while they are still open (ready check ~30 s, LFG proposal ~45 s, loot roll
// 60 s). Filled from the game on the world thread (ReadPopupState in
// bot_manager.cc); plain data so the edge detection below is unit-testable.
struct PopupState {
  struct Roll {
    uint64_t item_guid = 0;
    uint32_t item_slot = 0;
    uint32_t item_id = 0;
    std::string item_name;

    bool operator==(const Roll&) const = default;
  };

  bool ready_check = false;
  bool role_check = false;
  uint32_t lfg_proposal_id = 0;
  uint32_t shared_quest_id = 0;
  std::string shared_quest_title;
  std::string shared_quest_from;
  bool resurrect_offer = false;
  bool summon = false;
  std::string summon_from;
  std::string boot_victim;
  std::string boot_reason;
  std::vector<Roll> rolls;
};

// Turns successive PopupStates of one bot into one alerting event per popup
// that newly appeared (a popup that stays open is reported once; one that
// closes and reopens is reported again). Not thread-safe: one per bot, used on
// the world thread.
class PopupWatch {
 public:
  std::vector<wlb::snapshot::Event> Update(const PopupState& now);
  void Reset();

 private:
  PopupState last_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_POPUP_WATCH_H_
