// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_POPUP_WATCH_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_POPUP_WATCH_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "snapshot/snapshot.h"

namespace llm_brain {

// How long a timed popup may stay unanswered by the brain before popup
// ownership temporarily hands off to playerbots (well under ready-check's
// 30 s, LFG proposal's 45 s, and loot roll's 60 s).
inline constexpr uint32_t kPopupFallbackDeadlineMs = 15000;

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
  // The open trade window: who with, and whether they pressed Trade.
  std::string trader;
  bool trader_accepted = false;
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

// Whether `state` (or a pending group invite) has an open timed popup that
// should fall back to playerbots if the brain misses `kPopupFallbackDeadlineMs`
// (excludes trade windows, where a human may take longer to place items).
bool HasTimedPopup(const PopupState& state, bool has_group_invite = false);

// Updates `open_since_ms` for a bot's timed-popup deadline and returns true
// once a timed popup has stayed open for at least `deadline_ms`.
bool UpdatePopupDeadline(std::optional<uint64_t>* open_since_ms,
                         bool has_timed_popup,
                         uint64_t now_ms,
                         uint32_t deadline_ms = kPopupFallbackDeadlineMs);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_POPUP_WATCH_H_
