// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_session.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "base/synchronization/lock.h"
#include "bot_manager/tank_swap.h"

namespace llm_brain {

// static
BotSessionRegistry& BotSessionRegistry::Instance() {
  static auto* registry = new BotSessionRegistry();
  return *registry;
}

void BotSessionRegistry::ResetOnLogout(uint64_t bot_guid) {
  TankSwapTracker::Instance().ClearForBot(bot_guid);
  base::AutoLock lock(lock_);
  auto it = sessions_.find(bot_guid);
  if (it == sessions_.end()) {
    return;
  }
  if (it->second.group_guid == 0 && it->second.human_group_guid == 0 &&
      !it->second.auto_popups) {
    sessions_.erase(it);
    return;
  }
  // A relog keeps its group, so it keeps the brain's role policy (`not_tank`)
  // for that group, whether the group came with a human, and the brain's popup
  // policy; transient `stay` holds never survive logout.
  BotSession kept;
  kept.group_guid = it->second.group_guid;
  kept.group_policy.not_tank = it->second.group_policy.not_tank;
  kept.human_group_guid = it->second.human_group_guid;
  kept.auto_popups = it->second.auto_popups;
  it->second = std::move(kept);
}

void BotSessionRegistry::ResetOnMapChange(uint64_t bot_guid) {
  TankSwapTracker::Instance().ClearForBot(bot_guid);
  base::AutoLock lock(lock_);
  if (auto it = sessions_.find(bot_guid); it != sessions_.end()) {
    it->second.last_interrupted = {};
    it->second.out_of_retry = {};
    it->second.avoided_targets.clear();
    it->second.stop_dps = false;
    it->second.combat_position_hold = false;
    it->second.skull_rti_suppressed = false;
    it->second.brain_target_pin = {};
    it->second.group_policy.stay = false;
    it->second.group_policy.stay_in_combat = false;
    it->second.stray_recalled_ms = 0;
    it->second.party_ping.reset();
    it->second.ping_stay.reset();
    it->second.moving_out_until_ms = 0;
    it->second.act_stealth_until_ms = 0;
    it->second.trade_requested = false;
    it->second.spell_miss.reset();
    it->second.cast_failure.reset();
    it->second.human_offline_since_ms.reset();
    it->second.popup_open_since_ms.reset();
    it->second.pending_rolls_seen.clear();
    it->second.pending_shared_quests.clear();
    it->second.corpse_run_step.reset();
    it->second.corpse_run_progress = {};
    it->second.errand_hold = {};
    it->second.human_brb_pause = {};
    it->second.leader_approvals = {};
    it->second.wipe_release_until_ms.reset();
  }
}

void BotSessionRegistry::ResetOnDisconnect(uint64_t bot_guid) {
  TankSwapTracker::Instance().ClearForBot(bot_guid);
  base::AutoLock lock(lock_);
  if (auto it = sessions_.find(bot_guid); it != sessions_.end()) {
    it->second.last_interrupted = {};
    it->second.out_of_retry = {};
    it->second.avoided_targets.clear();
    it->second.stop_dps = false;
    it->second.combat_position_hold = false;
    it->second.skull_rti_suppressed = false;
    it->second.brain_target_pin = {};
    it->second.group_policy.stay = false;
    it->second.group_policy.stay_in_combat = false;
    it->second.stray_recalled_ms = 0;
    it->second.party_ping.reset();
    it->second.ping_stay.reset();
    it->second.moving_out_until_ms = 0;
    it->second.act_stealth_until_ms = 0;
    it->second.trade_requested = false;
    it->second.spell_miss.reset();
    it->second.cast_failure.reset();
    it->second.human_offline_since_ms.reset();
    it->second.popup_open_since_ms.reset();
    it->second.pending_rolls_seen.clear();
    it->second.pending_shared_quests.clear();
    it->second.corpse_run_step.reset();
    it->second.corpse_run_progress = {};
    it->second.errand_hold = {};
    it->second.human_brb_pause = {};
    it->second.leader_approvals = {};
    it->second.wipe_release_until_ms.reset();
  }
}

void BotSessionRegistry::ClearAll() {
  TankSwapTracker::Instance().ClearAll();
  base::AutoLock lock(lock_);
  sessions_.clear();
}

size_t BotSessionRegistry::size() const {
  base::AutoLock lock(lock_);
  return sessions_.size();
}

}  // namespace llm_brain
