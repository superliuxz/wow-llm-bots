// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTIONS_OPTIONS_H_
#define MOD_LLM_BRAIN_OPTIONS_OPTIONS_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "options/action_types.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

using OptionFacts = wlb::snapshot::OptionFacts;

// WoW 3.3.5a UNIT_NPC_FLAGS bits (UnitDefines.h), kept here so options.cc and
// unit tests need no AzerothCore headers.
inline constexpr uint32_t kNpcFlagGossip = 0x00000001;
inline constexpr uint32_t kNpcFlagQuestGiver = 0x00000002;
inline constexpr uint32_t kNpcFlagTrainer = 0x00000010;
inline constexpr uint32_t kNpcFlagTrainerClass = 0x00000020;
inline constexpr uint32_t kNpcFlagTrainerProfession = 0x00000040;
inline constexpr uint32_t kNpcFlagVendor = 0x00000080;
inline constexpr uint32_t kNpcFlagRepair = 0x00001000;
inline constexpr uint32_t kNpcFlagFlightMaster = 0x00002000;
inline constexpr uint32_t kNpcFlagInnkeeper = 0x00010000;
inline constexpr uint32_t kTalkNpcFlags =
    kNpcFlagTrainer | kNpcFlagTrainerClass | kNpcFlagTrainerProfession |
    kNpcFlagVendor | kNpcFlagRepair | kNpcFlagFlightMaster | kNpcFlagInnkeeper;
// Internal OptionFacts bit (not a WoW UNIT_NPC_FLAGS bit): set for alive
// nearby creatures that fail bot->IsValidAttackTarget (e.g. immune or
// non-attackable neutral/event NPCs) so options.cc does not offer an
// impossible Attack option.
inline constexpr uint32_t kNpcFlagNotAttackable = 0x80000000u;

// What one numbered option does, and to what.
struct OptionTarget {
  enum class Kind {
    kAttack,
    kLoot,
    kGather,  // A herb or ore node: looted with the gathering skill.
    kUse,     // Any other game object the body can click (chest, quest object).
    kTalk,    // A friendly NPC with a quest or service.
    kRest,    // Eat, drink or sit until full health and mana.
    kExplore,        // Walk to a grind spot in the bot's zone.
    kGoTo,           // Walk to a nearby hub, service or quest objective area.
    kKeep,           // Keep doing what I'm doing: the body carries on.
    kInvite,         // Invite a player to a group.
    kAcceptInvite,   // Accept a pending group invite.
    kDeclineInvite,  // Decline a pending group invite.
    kLeaveGroup,     // Leave the current party/raid.
  };
  Kind kind = Kind::kKeep;
  // The unit's or game object's guid ("0x..."), or stable spot/place id
  // ("spot:<map>:<x>:<y>", "place:<id>"); empty for kRest and kKeep.
  std::string guid;
  std::string name;
  // Destination world coordinates for kExplore and kGoTo (from OptionFacts).
  std::optional<wlb::snapshot::Vec3> pos = std::nullopt;
  // Typed action payload decoded once when `AddOptions` builds the option
  // (C++ cleanup #1: executors dispatch on `action` without `substr` parsing).
  BotAction action = KeepAction{};
};

std::string_view OptionKindName(OptionTarget::Kind kind);

// Adds to `snapshot` the options a player would have on this screen
// (docs/specs/snapshot.md §9, docs/specs/options.md) and returns
// what each one targets, in the same order. Pure: reads `snapshot` and `facts`,
// needs no game server, so the live module and `fixture_options` run the same
// code.
std::vector<OptionTarget> AddOptions(wlb::snapshot::Snapshot* snapshot,
                                     const OptionFacts& facts = {});

// Party member readiness facts for a `QUEST_FLAGS_PARTY_ACCEPT` escort quest
// (Now #8: "An escort waits until everyone has the quest").
struct EscortPartyMember {
  bool is_human = false;
  bool online = true;
  bool alive = true;
  bool same_map = true;
  float dist_to_npc_yd = 0.0f;
  bool has_quest = false;
  bool missing_prereq = false;
};

// Returns why a grouped bot should hold off starting an escort quest right now,
// or "" when solo, a party member already started/has the quest, or the leader
// has every party member nearby, alive, and past any prerequisite turn-in.
std::string EscortStartHoldReason(
    bool self_is_leader,
    const std::vector<EscortPartyMember>& members);

// WoW 3.3.5a LFG role bitmasks (`lfg::LfgRoles` in LFG.h), kept here so spec
// role selection is unit-testable without AzerothCore headers (Now #9 / T6).
inline constexpr uint8_t kLfgRoleTank = 0x02;
inline constexpr uint8_t kLfgRoleHealer = 0x04;
inline constexpr uint8_t kLfgRoleDamage = 0x08;

struct LfgRoleChoice {
  uint8_t mask = kLfgRoleDamage;
  const char* name = "DPS";
};

// Spec-correct Dungeon Finder roles for `class_id` (WoW `Classes` enum: 1
// warrior, 2 paladin, 5 priest, 6 DK, 7 shaman, 11 druid, ...) and 0-based
// primary talent tab `spec_tab`. Untalented or pre-level-10 characters get
// every role their class can play.
std::vector<LfgRoleChoice> SpecLfgRoleChoices(uint8_t class_id,
                                              uint8_t spec_tab,
                                              bool has_talents,
                                              uint8_t level);

// The options of a bot's recent snapshots, so a Choose naming (seq, option)
// can be resolved after the bot has moved on to newer snapshots. Keeps the
// snapshots built within `window` of the newest one: by time, not by count, so
// a slow answer in combat (snapshots every 250 ms) is still resolved. One per
// bot; world thread only.
class OptionHistory {
 public:
  explicit OptionHistory(base::TimeDelta window);
  OptionHistory(OptionHistory&&) noexcept;
  OptionHistory& operator=(OptionHistory&&) noexcept;
  OptionHistory(const OptionHistory&) = delete;
  OptionHistory& operator=(const OptionHistory&) = delete;
  ~OptionHistory();

  void Add(uint32_t seq,
           base::TimeTicks built,
           std::vector<OptionTarget> options);

  struct Lookup {
    // Set when found.
    const OptionTarget* target = nullptr;
    // When the snapshot that offered it was built.
    base::TimeTicks snapshot_built;
    // Why not, when not found.
    std::string error;
  };
  // `option` is 1-based, as the brain sees it.
  Lookup Find(uint32_t seq, uint32_t option) const;

 private:
  struct Entry {
    uint32_t seq = 0;
    base::TimeTicks built;
    std::vector<OptionTarget> options;
  };
  base::TimeDelta window_;
  std::deque<Entry> entries_;  // Oldest first.
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_OPTIONS_H_
