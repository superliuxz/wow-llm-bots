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
#include "bot_manager/tick_owner.h"
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

  // A party member's minimap ping (MSG_MINIMAP_PING) in world coordinates.
  struct PartyPing {
    uint64_t from_guid = 0;
    std::string from;
    float x = 0.0f;
    float y = 0.0f;
    uint32_t map_id = 0;
    uint32_t instance_id = 0;
    uint32_t ms = 0;  // getMSTime() when it arrived.
    bool operator==(const PartyPing&) const = default;
  };

  // A spell cast by this bot that missed its target (from SMSG_SPELL_GO's miss
  // list or SMSG_CAST_FAILED SPELL_FAILED_IMMUNE). Shown on the caster's screen
  // for a few seconds and cleared on logout, map change, and disconnect.
  struct SpellMiss {
    uint32_t seq = 0;
    uint32_t spell_id = 0;
    std::string spell;
    uint64_t target_guid = 0;
    std::string target;
    // One of "miss", "dodge", "parry", "resist", "immune", "evade".
    std::string reason;
    uint32_t map_id = 0;
    uint32_t instance_id = 0;
    uint32_t ms = 0;  // getMSTime() when it arrived.
    bool operator==(const SpellMiss&) const = default;
  };

  // Most recent SMSG_CAST_FAILED result for this bot (for reporting the
  // specific failure reason when an active option or act fails).
  struct CastFailure {
    uint32_t seq = 0;
    uint32_t spell_id = 0;
    uint8_t result = 0;
    uint32_t ms = 0;
    bool operator==(const CastFailure&) const = default;
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
  uint32_t pending_boot_until_ms = 0;

  // Someone opened a trade with the bot (SMSG_TRADE_STATUS BEGIN_TRADE): a
  // brain-led bot's window opens on the next world tick, as the client's
  // does (OpenRequestedTrade); the brain then trades or cancels.
  bool trade_requested = false;

  // Group GUID that had a human player (`LeaveAbandonedHumanGroup`, Now #9 /
  // T11) and when the last human member went offline.
  uint64_t human_group_guid = 0;
  std::optional<uint32_t> human_offline_since_ms;

  // Pending timed popups observed via outgoing packets.
  uint32_t pending_shared_quest_id = 0;
  uint32_t pending_shared_quest_ms = 0;
  bool pending_shared_quest_confirm = false;
  std::optional<uint32_t> pending_ready_check_ms;
  uint32_t pending_lfg_proposal_id = 0;
  uint32_t pending_lfg_proposal_ms = 0;

  // The last party minimap ping, seen in outgoing packets (cleared on logout,
  // map change and disconnect; the snapshot drops it once stale or the pinger
  // left the group).
  std::optional<PartyPing> party_ping;

  // Where a `to ping` act sent the bot to hold (out of combat it doesn't
  // follow the leader back): cleared once it is out of combat with the
  // leader more than kPingStayLeaderYd away, by a new choice or `follow`,
  // and on logout, map change and disconnect.
  struct PingStay {
    uint32_t map_id = 0;
    uint32_t instance_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool operator==(const PingStay&) const = default;
  };
  std::optional<PingStay> ping_stay;

  // While a dodge moves the bot out (ActMovesOut), getMSTime() until which
  // playerbots' spells that would hold it still wait (BrainPolicyListener).
  // Refreshed every tick of the move, so it lapses by itself if the module
  // stops ticking; cleared on logout, map change and disconnect.
  uint32_t moving_out_until_ms = 0;
  // While a `use` of a stealth-only spell walks in (ActHoldsStealth),
  // getMSTime() until which playerbots' `unstealth` and spell casts wait
  // (BrainPolicyListener): they would break the stealth the act entered.
  // Refreshed every tick like moving_out_until_ms; cleared on logout, map
  // change and disconnect.
  uint32_t act_stealth_until_ms = 0;

  // Who answers this bot's popups (tick_owner.h): set every world tick by
  // BotManager::Update from the harness connection, read by
  // BrainPolicyListener. Playerbots-led after a logout until that next tick.
  TickOwner tick_owner = TickOwner::kPlayerbots;

  // Last outdoor stray-recall tick (`KeepNearHumanLeader`, Now #8).
  uint32_t stray_recalled_ms = 0;

  // Combat movement/targeting memory (cleared on combat end / map change /
  // disconnect).
  std::vector<uint64_t> avoided_targets;
  InterruptedSpot last_interrupted;

  // Most recent spell miss and cast failure for this bot (cleared on logout,
  // map change, and disconnect; monotonic seqs survive map change/disconnect so
  // consecutive misses never alias).
  uint32_t spell_miss_seq = 0;
  std::optional<SpellMiss> spell_miss;
  uint32_t cast_failure_seq = 0;
  std::optional<CastFailure> cast_failure;
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
