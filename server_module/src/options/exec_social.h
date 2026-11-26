// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Group, LFG, loot roll, ready check, resurrection, shared quest, follow,
// raid marking, and mount discovery & execution.

#ifndef MOD_LLM_BRAIN_OPTIONS_EXEC_SOCIAL_H_
#define MOD_LLM_BRAIN_OPTIONS_EXEC_SOCIAL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "options/act_executor.h"
#include "options/action_types.h"
#include "options/exec_items.h"
#include "options/exec_shared.h"
#include "snapshot/snapshot.h"

class Player;

namespace llm_brain {

void CollectGroupAndUpkeepActions(
    Player* bot,
    const wlb::snapshot::Snapshot& snapshot,
    std::vector<wlb::snapshot::OptionFacts_Action>* out);

ActionOutcome ExecuteInviteAction(Player* bot, const InviteAction& action);
ActionOutcome ExecuteAcceptInviteAction(Player* bot,
                                        const AcceptInviteAction& action);
ActionOutcome ExecuteDeclineInviteAction(Player* bot,
                                         const DeclineInviteAction& action);
ActionOutcome ExecuteLeaveGroupAction(Player* bot);
ActionOutcome ExecuteResurrectAction(Player* bot,
                                     const ResurrectAction& action);
ActionOutcome ExecuteLootRollAction(Player* bot, const LootRollAction& action);
// Casts the crowd-control spell on its target with `act: use` physics
// (StartSpellUseOn): a multi-tick walk-in / stealth / cast fills `*out_active`.
ActStartResult ExecuteCrowdControlAction(Player* bot,
                                         const CrowdControlAction& action,
                                         base::TimeTicks now,
                                         ActiveAct* out_active);
ActStartResult ExecuteTauntAction(Player* bot,
                                  const TauntAction& action,
                                  base::TimeTicks now,
                                  ActiveAct* out_active);
ActionOutcome ExecutePopupAnswerAction(Player* bot,
                                       const PopupAnswerAction& action);
// Opens, fills, accepts or cancels the trade window through the client's
// own packets (CMSG_INITIATE_TRADE, CMSG_SET_TRADE_ITEM, CMSG_SET_TRADE_GOLD,
// CMSG_ACCEPT_TRADE, CMSG_CANCEL_TRADE), so the server's checks apply.
ActionOutcome ExecuteTradeAction(Player* bot, const TradeAction& action);
ActionOutcome ExecuteTradeGold(Player* bot, uint32_t copper);
// With the brain's `auto_popups` policy set, answers what is open now: rez
// offers from the party, summons, ready checks, role checks, proposals, and
// loot rolls (Need a usable upgrade, else Greed, else Pass). Returns a short
// line per answer, for the brain. World thread.
std::vector<std::string> AnswerPopupsByPolicy(Player* bot);
ActionOutcome ExecuteReadyCheckAction(Player* bot,
                                      const ReadyCheckAction& action);
ActionOutcome ExecuteSharedQuestAction(Player* bot,
                                       const SharedQuestAction& action);
ActionOutcome ExecuteShareQuestAction(Player* bot,
                                      const ShareQuestAction& action);
ActionOutcome ExecuteLfgAction(Player* bot, const LfgAction& action);
ActionOutcome ExecuteGroupPolicyAction(Player* bot,
                                       const GroupPolicyAction& action);
ActionOutcome ExecuteFollowLeaderAction(Player* bot,
                                        const FollowLeaderAction& action);
ActionOutcome ExecuteMarkTargetAction(Player* bot,
                                      const MarkTargetAction& action);
ItemActionOutcome ExecuteMountAction(Player* bot, const MountAction& action);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_EXEC_SOCIAL_H_
