// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_BOT_MANAGER_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_BOT_MANAGER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bot_manager/errand_hold.h"
#include "bot_manager/popup_watch.h"
#include "options/quest_helpers.h"

class Group;
class Player;
class Unit;

namespace llm_brain {

// Owns which of our bots are online. Replaces playerbots' random-bot manager,
// which is switched off (Playerbots.RandomBotAutologin = 0): our bots are fixed
// characters with their own memory, so nothing may pick, rotate, teleport or
// refresh them behind our back (docs/playerbots-random-bot-audit.md).
//
// Bots log in through playerbots' public login (no master), so playerbots' own
// session and AI plumbing runs them; the stock AI is the bot's body. Playerbots
// resets a bot's strategies to its "companion" defaults on login and after
// many events, so the manager re-applies ours whenever that happened.
//
// World thread only.
class BotManager {
 public:
  // Strategy changes in playerbots' syntax ("-follow,+grind,+new rpg").
  struct Strategies {
    std::string combat;
    std::string non_combat;
    std::string dead;
  };

  BotManager(std::vector<std::string> names, Strategies strategies);
  BotManager(const BotManager&) = delete;
  BotManager& operator=(const BotManager&) = delete;
  ~BotManager();

  // Every world update: attaches policy listeners on every tick (O(1) hash
  // lookup) and runs Tick() at most once per second to log bots in and keep
  // their strategies applied.
  void Update(uint32_t diff_ms);
  // Logs our bots out (server shutdown).
  void Stop();

  // Thread-safe: whether an LLM harness is currently connected. While false,
  // BrainPolicyListener lets Playerbots handle invites, loot rolls, ready
  // checks, quest shares, and LFG popups so human groupmates never stall when
  // the harness is down (A-M9).
  static void SetBrainConnected(bool connected);
  static bool IsBrainConnected();

  // Thread-safe: whether the connected harness reports its LLM backend as
  // healthy. While false, popup ownership hands off to Playerbots until the
  // LLM recovers and succeeds again.
  static void SetLlmHealthy(bool healthy);
  static bool IsLlmHealthy();

  // Optional per-bot strategy suffix applied on top of the server-wide
  // `strategies_` (used by test benchmarks to mix baseline and stripped-
  // strategy parties in the same wave). Thread-safe.
  static void SetBotStrategyOverride(const std::string& bot_name,
                                     std::string combat_extra,
                                     std::string non_combat_extra);
  static void ClearBotStrategyOverride(const std::string& bot_name);
  static void ClearBotStrategyOverrides();

  // World thread only: how the brain chose to play in its current group
  // (RH-M1, RH-M2). Applied as playerbots strategies on top of the defaults;
  // it belongs to that one group, so it is gone once the bot is in another
  // group or none.
  struct GroupPolicy {
    // Someone else tanks: no taunts, pulls or tank assist.
    bool not_tank = false;
    // Wait here instead of following the leader.
    bool stay = false;
    // True only when `stay` was explicitly requested during combat; an
    // out-of-combat wait releases automatically when combat begins.
    bool stay_in_combat = false;

    bool operator==(const GroupPolicy&) const = default;
  };
  static GroupPolicy GetGroupPolicy(Player* bot);
  static void SetGroupPolicy(Player* bot, GroupPolicy policy);

  // World thread only: whether a `to ping` act still holds the bot where it
  // went (BotSession::ping_stay): it holds through a fight, and is dropped
  // once the bot is out of combat with its group's leader more than
  // kPingStayLeaderYd away (or gone, or on another map).
  static constexpr float kPingStayLeaderYd = 30.0f;
  static bool PingStayHolds(Player* bot);
  static void ClearPingStay(Player* bot);
  // Starts that hold where the bot stands now.
  static void StartPingStay(Player* bot);

  // Thread-safe: pauses playerbots' `follow`, `KeepSafeGroupFollow`, and the
  // 40 yd leash (`KeepNearHumanLeader`) while a grouped follower runs a
  // brain-initiated errand (loot, gather, talk, use, walk, rest, NPC
  // interaction). Immediately yields on P0 combat (self or party), completion,
  // timeout, map/instance change, or disconnect (Milestone 5a).
  static bool StartErrandHold(Player* bot, ErrandKind kind);
  static bool ErrandHoldActive(Player* bot);
  static void ClearErrandHold(Player* bot);

  // Thread-safe: whether a leader "stop dps" command currently pauses
  // offensive attacks and non-positive spell casts for `bot`.
  static void SetStopDps(Player* bot, bool stop);
  static bool IsStopDps(Player* bot);

  // Thread-safe: whether a brain combat movement/spread act currently holds
  // this bot's combat position against the healer-to-tank leash (M1b).
  // Cleared when combat ends, on explicit follow/ping-stay clear, or on map
  // change / disconnect / logout.
  static void SetCombatPositionHold(Player* bot, bool hold);
  static bool CombatPositionHoldActive(Player* bot);

  // Thread-safe: records that `bot`'s brain explicitly issued a taunt on
  // `target` so other tanks in the same group suppress playerbots' "lose
  // aggro" reflex taunt instead of ping-ponging aggro back (M1b).
  static void RecordBrainTaunt(Player* bot, Unit* target);

  // World thread only: the brain chose to lie dead hoping for a rez (RH-M3):
  // playerbots' auto release waits `wait_ms` from now, anywhere (by default
  // it waits up to 90 s, and only in a dungeon with a living resurrector).
  // Forgotten once the bot is alive again.
  static void WaitForResurrection(Player* bot, uint32_t wait_ms);

  // World thread only: pending shared-quest / ready-check / LFG-proposal
  // prompts captured via ServerScript::OnPacketSent (works for standalone
  // no-master bots) while BrainPolicyListener blocks Playerbots' auto-handlers.
  static std::vector<PendingSharedQuest> GetPendingSharedQuests(Player* bot);
  static std::optional<PendingSharedQuest> TakePendingSharedQuest(
      Player* bot,
      uint32_t quest_id);
  static void ClearPendingSharedQuests(Player* bot);
  static bool HasPendingReadyCheck(Player* bot);
  static void ClearPendingReadyCheck(Player* bot);
  static uint32_t GetPendingLfgProposal(Player* bot);
  static void ClearPendingLfgProposal(Player* bot);
  static std::pair<std::string, std::string> GetPendingBootVote(Player* bot);

  // World thread only: the timed popups open for `bot` right now (ready
  // check, LFG role check / proposal / boot vote, summon, shared quest,
  // resurrect offer, loot rolls it hasn't voted on), for PopupWatch to turn
  // into wake events.
  static PopupState ReadPopupState(Player* bot);

  // World thread only: someone opened a trade with a brain-led `bot`: open
  // its trade window, as a player's client does by itself (CMSG_BEGIN_TRADE).
  // Accepting, filling and cancelling it are the brain's choices. A
  // playerbots-led bot's request is playerbots' (TradeStatusAction).
  static void OpenRequestedTrade(Player* bot);

  // World thread only: true when `group` has at least one disconnected member
  // slot that is not one of our bots or a random playerbot (i.e. an offline
  // human player).
  bool GroupHasOfflineHuman(Group* group) const;

 private:
  struct Bot {
    std::string name;
    uint64_t guid = 0;  // 0: unknown character name.
    uint64_t next_login_ms = 0;
    // Seen connected on the last tick: a bot that just dropped waits
    // kLoginRetryMs before it is logged in again, like a failed login.
    bool was_online = false;
  };

  void Tick();
  void EnsureStrategies(Player* player);
  void KeepCorpseRun(Player* player);
  void KeepNearHumanLeader(Player* player);
  void KeepSafeGroupFollow(Player* player);
  void KeepHealerInTankRangeAndLos(Player* player);
  void LeaveAbandonedHumanGroup(Player* player);
  bool IsKnownBotGuid(uint64_t raw_guid) const;

  const Strategies strategies_;
  std::vector<Bot> bots_;
  uint64_t now_ms_ = 0;
  uint32_t since_tick_ms_ = 0;
};

// Registers the ServerScript that captures outgoing server prompts
// (MSG_RAID_READY_CHECK, SMSG_QUESTGIVER_QUEST_DETAILS with divider,
// SMSG_LFG_PROPOSAL_UPDATE) for standalone bots.
void AddLlmBrainBotManagerScripts();

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_BOT_MANAGER_H_
