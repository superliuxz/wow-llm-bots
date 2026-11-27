// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/action_types.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

#include "base/strings/string_split.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

bool ParseUint32(std::string_view s, uint32_t* out) {
  if (s.empty()) {
    return false;
  }
  uint32_t val = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
  if (ec != std::errc{} || ptr != s.data() + s.size()) {
    return false;
  }
  *out = val;
  return true;
}

}  // namespace

BotAction DecodeOptionFactAction(std::string_view guid,
                                 std::string_view label) {
  if (guid == "rez:accept") {
    return ResurrectAction{ResurrectAction::Mode::kAccept};
  }
  if (guid == "rez:release") {
    return ResurrectAction{ResurrectAction::Mode::kRelease};
  }
  if (guid == "rez:self") {
    return ResurrectAction{ResurrectAction::Mode::kSelf};
  }
  if (guid == "rez:wait") {
    return ResurrectAction{ResurrectAction::Mode::kWait};
  }
  if (guid.starts_with("roll:")) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t slot = 0;
    uint32_t vote = 0;
    if (parts.size() != 4 || !ParseUint32(parts[2], &slot) ||
        !ParseUint32(parts[3], &vote) || vote > 3) {
      return InvalidAction{"invalid loot roll"};
    }
    return LootRollAction{std::string(parts[1]), slot,
                          static_cast<uint8_t>(vote), std::string(label)};
  }
  if (guid.starts_with("cc:")) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t spell = 0;
    if (parts.size() != 3 || !ParseUint32(parts[1], &spell) || spell == 0 ||
        parts[2].empty()) {
      return InvalidAction{"invalid crowd control"};
    }
    return CrowdControlAction{spell, std::string(parts[2]), std::string(label)};
  }
  if (guid.starts_with("taunt:")) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t spell = 0;
    if (parts.size() != 3 || !ParseUint32(parts[1], &spell) || spell == 0 ||
        parts[2].empty()) {
      return InvalidAction{"invalid taunt"};
    }
    return TauntAction{spell, std::string(parts[2]), std::string(label)};
  }
  if (guid == "popups:auto") {
    return GroupPolicyAction{GroupPolicyAction::Op::kAutoPopups};
  }
  if (guid == "popups:manual") {
    return GroupPolicyAction{GroupPolicyAction::Op::kManualPopups};
  }
  if (guid == "summon:accept" || guid == "summon:decline") {
    return PopupAnswerAction{PopupAnswerAction::Kind::kSummon,
                             guid == "summon:accept"};
  }
  if (guid == "boot:yes" || guid == "boot:no") {
    return PopupAnswerAction{PopupAnswerAction::Kind::kBootVote,
                             guid == "boot:yes"};
  }
  if (guid.starts_with("trade:")) {
    TradeAction trade;
    trade.label = std::string(label);
    if (guid == "trade:accept") {
      trade.op = TradeAction::Op::kAccept;
      return trade;
    }
    if (guid == "trade:cancel") {
      trade.op = TradeAction::Op::kCancel;
      return trade;
    }
    if (guid.starts_with("trade:open:") && guid.size() > 11) {
      trade.op = TradeAction::Op::kOpen;
      trade.target_guid = std::string(guid.substr(11));
      return trade;
    }
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t bag = 0;
    uint32_t slot = 0;
    if (parts.size() == 5 && parts[1] == "put" && ParseUint32(parts[2], &bag) &&
        ParseUint32(parts[3], &slot) && ParseUint32(parts[4], &trade.entry) &&
        bag <= 255 && slot <= 255 && trade.entry != 0) {
      trade.op = TradeAction::Op::kPutItem;
      trade.bag = static_cast<uint8_t>(bag);
      trade.slot = static_cast<uint8_t>(slot);
      return trade;
    }
    return InvalidAction{"invalid trade action"};
  }
  if (guid == "ready:start") {
    return ReadyCheckAction{true, true};
  }
  if (guid == "ready:yes") {
    return ReadyCheckAction{true};
  }
  if (guid == "ready:no") {
    return ReadyCheckAction{false};
  }
  if (guid.starts_with("ready:")) {
    return InvalidAction{"invalid ready check"};
  }
  if (guid.starts_with("qshare:")) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t qid = 0;
    if (parts.size() != 3 || !ParseUint32(parts[2], &qid)) {
      return InvalidAction{"invalid shared quest"};
    }
    if (parts[1] == "accept") {
      return SharedQuestAction{qid, true};
    }
    if (parts[1] == "decline") {
      return SharedQuestAction{qid, false};
    }
    return InvalidAction{"invalid shared quest action"};
  }
  if (guid.starts_with("qpush:")) {
    uint32_t qid = 0;
    if (!ParseUint32(guid.substr(6), &qid)) {
      return InvalidAction{"you can't share that quest"};
    }
    return ShareQuestAction{qid};
  }
  if (guid.starts_with("lfg:")) {
    if (guid.starts_with("lfg:queue:")) {
      uint32_t role_mask = 0;
      if (!ParseUint32(guid.substr(10), &role_mask) || role_mask == 0) {
        return InvalidAction{"invalid LFG role"};
      }
      return LfgAction{LfgAction::Op::kQueue, role_mask};
    }
    if (guid == "lfg:leave") {
      return LfgAction{LfgAction::Op::kLeave, 0};
    }
    if (guid.starts_with("lfg:role:")) {
      uint32_t role_mask = 0;
      if (!ParseUint32(guid.substr(9), &role_mask) || role_mask == 0) {
        return InvalidAction{"invalid LFG role"};
      }
      return LfgAction{LfgAction::Op::kRole, role_mask};
    }
    if (guid == "lfg:tele_in") {
      return LfgAction{LfgAction::Op::kTeleIn, 0};
    }
    if (guid == "lfg:tele_out") {
      return LfgAction{LfgAction::Op::kTeleOut, 0};
    }
    if (guid.starts_with("lfg:accept:")) {
      uint32_t pid = 0;
      if (!ParseUint32(guid.substr(11), &pid)) {
        return InvalidAction{"invalid LFG proposal"};
      }
      return LfgAction{LfgAction::Op::kAcceptProposal, pid};
    }
    if (guid.starts_with("lfg:decline:")) {
      uint32_t pid = 0;
      if (!ParseUint32(guid.substr(12), &pid)) {
        return InvalidAction{"invalid LFG proposal"};
      }
      return LfgAction{LfgAction::Op::kDeclineProposal, pid};
    }
    return InvalidAction{"unknown LFG action"};
  }
  if (guid.starts_with("grouppolicy:")) {
    const std::string_view what = guid.substr(12);
    if (what == "notank") {
      return GroupPolicyAction{GroupPolicyAction::Op::kNoTank};
    }
    if (what == "tank") {
      return GroupPolicyAction{GroupPolicyAction::Op::kTank};
    }
    if (what == "stay") {
      return GroupPolicyAction{GroupPolicyAction::Op::kStay};
    }
    if (what == "follow") {
      return GroupPolicyAction{GroupPolicyAction::Op::kFollow};
    }
    return InvalidAction{"unknown group policy"};
  }
  if (guid.starts_with("follow:")) {
    return FollowLeaderAction{std::string(guid.substr(7))};
  }
  if (guid.starts_with("mark:")) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    uint32_t icon = 0;
    if (parts.size() != 3 || !ParseUint32(parts[2], &icon) || icon >= 8) {
      return InvalidAction{"invalid raid mark"};
    }
    return MarkTargetAction{std::string(parts[1]), static_cast<uint8_t>(icon)};
  }
  if (guid == "dismount") {
    return DismountAction{};
  }
  if (guid.starts_with("mount:")) {
    uint32_t spell_id = 0;
    if (!ParseUint32(guid.substr(6), &spell_id) || spell_id == 0) {
      return InvalidAction{"invalid mount"};
    }
    return MountAction{spell_id, std::string(label)};
  }
  if (guid.starts_with("npc_")) {
    const std::vector<std::string_view> parts = base::SplitStringPiece(
        guid, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    if (parts.size() < 2) {
      return InvalidAction{"invalid NPC action"};
    }
    const std::string npc_guid(parts[1]);
    if (parts[0] == "npc_qaccept" && parts.size() == 3) {
      uint32_t qid = 0;
      if (!ParseUint32(parts[2], &qid)) {
        return InvalidAction{"invalid quest id"};
      }
      return NpcAction{NpcAction::Op::kQuestAccept, npc_guid, qid, 0};
    }
    if (parts[0] == "npc_qturnin" && parts.size() == 4) {
      uint32_t qid = 0;
      uint32_t reward_idx = 0;
      if (!ParseUint32(parts[2], &qid) || !ParseUint32(parts[3], &reward_idx)) {
        return InvalidAction{"invalid quest turn-in"};
      }
      return NpcAction{NpcAction::Op::kQuestTurnIn, npc_guid, qid, reward_idx};
    }
    if (parts[0] == "npc_train") {
      return NpcAction{NpcAction::Op::kTrain, npc_guid, 0, 0};
    }
    if (parts[0] == "npc_train_riding" && parts.size() == 3) {
      uint32_t spell_id = 0;
      if (!ParseUint32(parts[2], &spell_id) || spell_id == 0) {
        return InvalidAction{"invalid riding spell"};
      }
      return NpcAction{NpcAction::Op::kTrainRiding, npc_guid, spell_id, 0};
    }
    if (parts[0] == "npc_vendor") {
      return NpcAction{NpcAction::Op::kVendor, npc_guid, 0, 0};
    }
    if (parts[0] == "npc_buy" && parts.size() == 3) {
      uint32_t item_entry = 0;
      if (!ParseUint32(parts[2], &item_entry)) {
        return InvalidAction{"invalid vendor item"};
      }
      return NpcAction{NpcAction::Op::kBuy, npc_guid, item_entry, 0};
    }
    if (parts[0] == "npc_fly_discover") {
      return NpcAction{NpcAction::Op::kFlyDiscover, npc_guid, 0, 0};
    }
    if (parts[0] == "npc_fly" && parts.size() == 3) {
      uint32_t dest_node = 0;
      if (!ParseUint32(parts[2], &dest_node)) {
        return InvalidAction{"invalid flight destination"};
      }
      return NpcAction{NpcAction::Op::kFlyTo, npc_guid, dest_node, 0};
    }
    if (parts[0] == "npc_bind") {
      return NpcAction{NpcAction::Op::kBind, npc_guid, 0, 0};
    }
    return InvalidAction{"unknown NPC action"};
  }
  if (guid.starts_with("talent:")) {
    uint32_t tab_page = 0;
    if (!ParseUint32(guid.substr(7), &tab_page) || tab_page > 2) {
      return InvalidAction{"invalid talent tree"};
    }
    return SpendTalentsAction{tab_page, std::string(label)};
  }
  if (guid.starts_with("item:")) {
    uint32_t entry = 0;
    if (!ParseUint32(guid.substr(5), &entry) || entry == 0) {
      return InvalidAction{"invalid item id"};
    }
    return UseBagItemAction{entry, std::string(label)};
  }
  if (guid.starts_with("equip:")) {
    uint32_t entry = 0;
    if (!ParseUint32(guid.substr(6), &entry) || entry == 0) {
      return InvalidAction{"invalid item id"};
    }
    return EquipItemAction{entry, std::string(label)};
  }
  if (guid.starts_with("0x")) {
    return UseObjectAction{std::string(guid), std::string(label)};
  }
  return InvalidAction{"unknown action"};
}

std::string ValidateTradeGold(bool has_trade,
                              uint32_t copper,
                              uint32_t bot_money) {
  if (!has_trade) {
    return "no trade window is open";
  }
  if (copper == 0) {
    return "invalid trade gold amount";
  }
  if (copper > bot_money) {
    return "not enough money";
  }
  return {};
}

bool IsKeepAction(const BotAction& action) {
  return std::holds_alternative<KeepAction>(action);
}

bool IsAttackAction(const BotAction& action) {
  return std::holds_alternative<AttackAction>(action);
}

bool IsRestAction(const BotAction& action) {
  return std::holds_alternative<RestAction>(action);
}

bool IsDeadAllowedAction(const BotAction& action) {
  if (std::holds_alternative<KeepAction>(action) ||
      std::holds_alternative<ResurrectAction>(action) ||
      std::holds_alternative<LootRollAction>(action) ||
      std::holds_alternative<ReadyCheckAction>(action) ||
      std::holds_alternative<PopupAnswerAction>(action) ||
      std::holds_alternative<LeaveGroupAction>(action) ||
      std::holds_alternative<AcceptInviteAction>(action) ||
      std::holds_alternative<DeclineInviteAction>(action)) {
    return true;
  }
  if (const auto* act = std::get_if<StandingActAction>(&action)) {
    return act->act.verb == wlb::snapshot::Command_Act::WAKE;
  }
  return false;
}

bool IsPendingCastAction(const BotAction& action) {
  return std::holds_alternative<UseBagItemAction>(action) ||
         std::holds_alternative<MountAction>(action);
}

bool IsGameObjectAction(const BotAction& action) {
  return std::holds_alternative<GatherAction>(action) ||
         std::holds_alternative<UseObjectAction>(action);
}

bool IsMovementActVerb(wlb::snapshot::Command_Act::Verb verb) {
  return verb == wlb::snapshot::Command_Act::TO ||
         verb == wlb::snapshot::Command_Act::AWAY ||
         verb == wlb::snapshot::Command_Act::OUT_OF ||
         verb == wlb::snapshot::Command_Act::BACK ||
         verb == wlb::snapshot::Command_Act::KITE ||
         verb == wlb::snapshot::Command_Act::INTERACT ||
         verb == wlb::snapshot::Command_Act::BOARD;
}

std::string_view ActionKindName(const BotAction& action) {
  return std::visit(
      [](const auto& a) -> std::string_view {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, KeepAction>)
          return "keep";
        if constexpr (std::is_same_v<T, AttackAction>)
          return "attack";
        if constexpr (std::is_same_v<T, LootAction>)
          return "loot";
        if constexpr (std::is_same_v<T, GatherAction>)
          return "gather";
        if constexpr (std::is_same_v<T, UseObjectAction>)
          return "use_object";
        if constexpr (std::is_same_v<T, TalkAction>)
          return "talk";
        if constexpr (std::is_same_v<T, RestAction>)
          return "rest";
        if constexpr (std::is_same_v<T, WalkAction>)
          return a.is_explore ? "explore" : "go_to";
        if constexpr (std::is_same_v<T, InviteAction>)
          return "invite";
        if constexpr (std::is_same_v<T, AcceptInviteAction>)
          return "accept_invite";
        if constexpr (std::is_same_v<T, DeclineInviteAction>)
          return "decline_invite";
        if constexpr (std::is_same_v<T, LeaveGroupAction>)
          return "leave_group";
        if constexpr (std::is_same_v<T, ResurrectAction>)
          return "resurrect";
        if constexpr (std::is_same_v<T, LootRollAction>)
          return "loot_roll";
        if constexpr (std::is_same_v<T, ReadyCheckAction>)
          return "ready_check";
        if constexpr (std::is_same_v<T, SharedQuestAction>)
          return "shared_quest";
        if constexpr (std::is_same_v<T, ShareQuestAction>)
          return "share_quest";
        if constexpr (std::is_same_v<T, LfgAction>)
          return "lfg";
        if constexpr (std::is_same_v<T, GroupPolicyAction>)
          return "group_policy";
        if constexpr (std::is_same_v<T, PopupAnswerAction>)
          return "popup_answer";
        if constexpr (std::is_same_v<T, TradeAction>)
          return "trade";
        if constexpr (std::is_same_v<T, CrowdControlAction>)
          return "crowd_control";
        if constexpr (std::is_same_v<T, TauntAction>)
          return "taunt";
        if constexpr (std::is_same_v<T, FollowLeaderAction>)
          return "follow";
        if constexpr (std::is_same_v<T, MarkTargetAction>)
          return "mark";
        if constexpr (std::is_same_v<T, MountAction>)
          return "mount";
        if constexpr (std::is_same_v<T, DismountAction>)
          return "dismount";
        if constexpr (std::is_same_v<T, NpcAction>)
          return "npc";
        if constexpr (std::is_same_v<T, SpendTalentsAction>)
          return "talents";
        if constexpr (std::is_same_v<T, UseBagItemAction>)
          return "use_item";
        if constexpr (std::is_same_v<T, EquipItemAction>)
          return "equip_item";
        if constexpr (std::is_same_v<T, StandingActAction>)
          return "act";
        if constexpr (std::is_same_v<T, InvalidAction>)
          return "invalid";
      },
      action);
}

}  // namespace llm_brain
