// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Unified typed action representation for both chosen options
// (`Command::Choose`) and standing-order acts (`Command::Act`) (C++ cleanup
// #1). Replaces string option-id parsing (`substr`/`SplitStringPiece`) in
// executors and world script with one `std::variant` payload decoded once at
// option creation.

#ifndef MOD_LLM_BRAIN_OPTIONS_ACTION_TYPES_H_
#define MOD_LLM_BRAIN_OPTIONS_ACTION_TYPES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "snapshot/snapshot.h"

namespace llm_brain {

struct KeepAction {};

struct AttackAction {
  std::string guid;
  std::string name;
};

struct LootAction {
  std::string guid;
  std::string name;
};

struct GatherAction {
  std::string guid;
  std::string name;
};

struct UseObjectAction {
  std::string guid;
  std::string name;
};

struct TalkAction {
  std::string guid;
  std::string name;
};

struct RestAction {};

struct WalkAction {
  std::string id;
  std::string name;
  std::optional<wlb::snapshot::Vec3> dest;
  bool is_explore = false;
};

struct InviteAction {
  std::string player_name;
};

struct AcceptInviteAction {
  std::string from;
};

struct DeclineInviteAction {
  std::string from;
};

struct LeaveGroupAction {};

struct ResurrectAction {
  enum class Mode : uint8_t { kAccept, kRelease, kWait, kSelf };
  Mode mode = Mode::kRelease;
};

struct LootRollAction {
  std::string item_guid;
  uint32_t slot = 0;
  uint8_t vote = 0;
  std::string label;
};

struct ReadyCheckAction {
  bool ready = true;
  bool start = false;  // The leader starts one (Now #5).
};

struct SharedQuestAction {
  uint32_t quest_id = 0;
  bool accept = true;
};

struct ShareQuestAction {
  uint32_t quest_id = 0;
};

struct LfgAction {
  enum class Op : uint8_t {
    kQueue,
    kLeave,
    kRole,
    kTeleIn,
    kTeleOut,
    kAcceptProposal,
    kDeclineProposal,
  };
  Op op = Op::kLeave;
  uint32_t value =
      0;  // role mask for kQueue/kRole; proposal id for accept/decline.
};

struct GroupPolicyAction {
  enum class Op : uint8_t {
    kNoTank,
    kTank,
    kStay,
    kFollow,
    kAutoPopups,
    kManualPopups
  };
  Op op = Op::kFollow;
};

// Casts a crowd-control spell on a marked mob (Now #4).
struct CrowdControlAction {
  uint32_t spell_id = 0;
  std::string target_guid;
  std::string label;
};

// Answers a summon or a Dungeon Finder kick vote (Now #3).
struct PopupAnswerAction {
  enum class Kind : uint8_t { kSummon, kBootVote };
  Kind kind = Kind::kSummon;
  bool yes = true;
};

// The trade window (Objective 9a): open one with a party member, put a bag
// item up for trade, accept (press Trade), or cancel.
struct TradeAction {
  enum class Op : uint8_t { kOpen, kPutItem, kAccept, kCancel };
  Op op = Op::kCancel;
  std::string target_guid;  // kOpen.
  uint8_t bag = 0;          // kPutItem: the item's bag and slot
  uint8_t slot = 0;         // (Player::GetItemByPos) and entry.
  uint32_t entry = 0;
  std::string label;
};

struct FollowLeaderAction {
  std::string leader_guid;
};

struct MarkTargetAction {
  std::string target_guid;
  uint8_t icon = 0;
};

struct MountAction {
  uint32_t spell_id = 0;
  std::string label;
};

struct NpcAction {
  enum class Op : uint8_t {
    kQuestAccept,
    kQuestTurnIn,
    kTrain,
    kVendor,
    kBuy,
    kFlyDiscover,
    kFlyTo,
    kBind,
  };
  Op op = Op::kVendor;
  std::string npc_guid;
  uint32_t arg1 = 0;  // quest_id, item_entry, or dest_node
  uint32_t arg2 = 0;  // reward_idx for kQuestTurnIn
};

struct SpendTalentsAction {
  uint32_t tab_page = 0;
  std::string tree_name;
};

struct UseBagItemAction {
  uint32_t entry = 0;
  std::string name;
};

struct EquipItemAction {
  uint32_t entry = 0;
  std::string name;
};

struct StandingActAction {
  uint32_t command_id = 0;
  wlb::snapshot::Command_Act act;
};

// Malformed or unrecognized option fact payload (preserved so validation
// reports a clear error instead of silently falling through to GameObject).
struct InvalidAction {
  std::string error;
};

using BotAction = std::variant<KeepAction,
                               AttackAction,
                               LootAction,
                               GatherAction,
                               UseObjectAction,
                               TalkAction,
                               RestAction,
                               WalkAction,
                               InviteAction,
                               AcceptInviteAction,
                               DeclineInviteAction,
                               LeaveGroupAction,
                               ResurrectAction,
                               LootRollAction,
                               ReadyCheckAction,
                               SharedQuestAction,
                               ShareQuestAction,
                               LfgAction,
                               GroupPolicyAction,
                               PopupAnswerAction,
                               TradeAction,
                               CrowdControlAction,
                               FollowLeaderAction,
                               MarkTargetAction,
                               MountAction,
                               NpcAction,
                               SpendTalentsAction,
                               UseBagItemAction,
                               EquipItemAction,
                               StandingActAction,
                               InvalidAction>;

// Decodes a wire/fact action guid (`npc_*`, `roll:*`, `ready:*`, `qshare:*`,
// `lfg:*`, `qpush:*`, `grouppolicy:*`, `follow:*`, `mark:*`, `mount:*`,
// `talent:*`, `item:*`, `equip:*`, `rez:*`, `trade:*`) into a typed `BotAction`
// once at option creation. Returns `InvalidAction` if malformed.
BotAction DecodeOptionFactAction(std::string_view guid, std::string_view label);

// Pure validation for `Command_Act::TRADE_GOLD` before sending
// CMSG_SET_TRADE_GOLD. Returns empty string if valid, else an error message.
std::string ValidateTradeGold(bool has_trade,
                              uint32_t copper,
                              uint32_t bot_money);

bool IsKeepAction(const BotAction& action);
bool IsAttackAction(const BotAction& action);
bool IsRestAction(const BotAction& action);
bool IsDeadAllowedAction(const BotAction& action);
bool IsPendingCastAction(const BotAction& action);
bool IsGameObjectAction(const BotAction& action);
bool IsMovementActVerb(wlb::snapshot::Command_Act::Verb verb);
std::string_view ActionKindName(const BotAction& action);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_ACTION_TYPES_H_
