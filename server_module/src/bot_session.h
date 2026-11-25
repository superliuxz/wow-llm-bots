// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Single per-bot session state registry for `mod-llm-brain` (C++ cleanup #2).
// Replaces scattered static maps across `bot_manager.cc` and `act_executor.cc`
// with one thread-safe `BotSession` per bot GUID and unified reset hooks for
// logout, map change, harness disconnect, and server shutdown.

#ifndef MOD_LLM_BRAIN_BOT_SESSION_H_
#define MOD_LLM_BRAIN_BOT_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

struct BotSession {
  struct GroupPolicy {
    bool not_tank = false;
    bool stay = false;
    bool operator==(const GroupPolicy&) const = default;
  };

  struct InterruptedSpot {
    bool active = false;
    uint32_t map_id = 0;
    uint32_t instance_id = 0;
    uint64_t target_guid = 0;
    wlb::snapshot::Vec3 prev_pos;
    float prev_target_dist_yd = 0.0f;
    bool was_attacking = false;
  };

  // Auto-release hold & explicit `rez:wait` timers (ms).
  std::optional<uint32_t> dead_since_ms;
  std::optional<uint32_t> rez_wait_until_ms;

  // Group policy chosen by the brain for `group_guid`. Survives logout: the
  // group does too.
  uint64_t group_guid = 0;
  GroupPolicy group_policy;

  // The area-damage strategies (`aoe`, `frost aoe`, ...) taken off while a
  // crowd-controlled mob is near (Now #4), given back when none is.
  std::vector<std::string> suppressed_aoe;

  // The brain's popup policy (Now #3): answer rez offers, summons, ready
  // checks, role checks, proposals and loot rolls automatically. Survives
  // logout.
  bool auto_popups = false;

  // A summon (warlock ritual, meeting stone) and a Dungeon Finder kick vote
  // waiting for an answer, seen in outgoing packets.
  std::string pending_summoner;
  std::string pending_boot_victim;
  std::string pending_boot_reason;

  // Pending timed popups observed via outgoing packets.
  uint32_t pending_shared_quest_id = 0;
  uint32_t pending_shared_quest_ms = 0;
  std::optional<uint32_t> pending_ready_check_ms;
  uint32_t pending_lfg_proposal_id = 0;
  uint32_t pending_lfg_proposal_ms = 0;

  // Combat movement/targeting memory (cleared on combat end / map change /
  // disconnect).
  std::vector<uint64_t> avoided_targets;
  InterruptedSpot last_interrupted;
};

class BotSessionRegistry {
 public:
  static BotSessionRegistry& Instance();

  // Executes `fn(BotSession&)` under lock, creating the entry if absent.
  template <typename Fn>
  auto WithSession(uint64_t bot_guid, Fn&& fn) {
    base::AutoLock lock(lock_);
    return std::forward<Fn>(fn)(sessions_[bot_guid]);
  }

  // Executes `fn(const BotSession*)` under lock (nullptr if no entry exists).
  template <typename Fn>
  auto InspectSession(uint64_t bot_guid, Fn&& fn) const {
    base::AutoLock lock(lock_);
    auto it = sessions_.find(bot_guid);
    return std::forward<Fn>(fn)(it == sessions_.end() ? nullptr : &it->second);
  }

  // Unified lifecycle resets so no per-bot state survives logout, map change,
  // or disconnect.
  void ResetOnLogout(uint64_t bot_guid);
  void ResetOnMapChange(uint64_t bot_guid);
  void ResetOnDisconnect(uint64_t bot_guid);
  void ClearAll();

  size_t size() const;

 private:
  mutable base::Lock lock_;
  std::unordered_map<uint64_t, BotSession> sessions_ GUARDED_BY(lock_);
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_SESSION_H_
