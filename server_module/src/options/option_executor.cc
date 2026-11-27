// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/option_executor.h"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "AiObjectContext.h"
#include "Event.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Unit.h"
#include "base/time/time.h"
#include "options/act_executor.h"
#include "options/action_types.h"
#include "options/exec_items.h"
#include "options/exec_movement.h"
#include "options/exec_npc.h"
#include "options/exec_shared.h"
#include "options/exec_social.h"
#include "options/options.h"
#include "options/position_facts.h"

namespace llm_brain {
namespace {

ExecuteResult FromOutcome(ActionOutcome outcome) {
  return {.error = std::move(outcome.error),
          .used = outcome.used,
          .info = std::move(outcome.info)};
}

ExecuteResult FromItemOutcome(ItemActionOutcome outcome) {
  return {.error = std::move(outcome.error),
          .used = outcome.used,
          .info = std::move(outcome.info),
          .pending_spell_id = outcome.pending_spell_id};
}

ExecuteResult ExecuteAttackAction(Player* bot, const AttackAction& action) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {.error = "not a bot"};
  }
  Unit* unit = ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(action.guid));
  if (!unit || !unit->IsInWorld()) {
    return {.error = action.name + " is gone"};
  }
  AiObjectContext* context = ai->GetAiObjectContext();
  auto* current_target = context->GetValue<Unit*>("current target");
  if (!current_target) {
    return {.error = "the body can't do that (playerbots values missing)"};
  }
  ClearAvoidedTargetForBot(bot->GetGUID().GetRawValue(),
                           unit->GetGUID().GetRawValue());
  Unit* old_target = current_target->Get();
  current_target->Set(unit);
  if (!ai->DoSpecificAction("attack", Event(), /*silent=*/true)) {
    if (old_target == unit || bot->GetVictim() == unit) {
      ai->ResetActionDuration();
      return {};
    }
    if (ShouldApproachOutOfLosTarget(unit->IsAlive(), bot->IsFriendlyTo(unit),
                                     bot->IsValidAttackTarget(unit),
                                     bot->IsWithinLOSInMap(unit)) &&
        WalkNextTo(bot, unit)) {
      ai->ResetActionDuration();
      return {};
    }
    current_target->Set(old_target);
    return {.error = "can't attack " + action.name + " now"};
  }
  ai->ResetActionDuration();
  return {};
}

}  // namespace

std::string ValidateAction(Player* bot, const BotAction& action) {
  if (std::holds_alternative<KeepAction>(action)) {
    return "";
  }
  if (const auto* invalid = std::get_if<InvalidAction>(&action)) {
    return invalid->error.empty() ? "invalid action" : invalid->error;
  }
  if (!bot || !bot->IsInWorld()) {
    return "bot not in world";
  }
  if (GET_PLAYERBOT_AI(bot) == nullptr) {
    return "not a bot";
  }
  if (!IsDeadAllowedAction(action) && !bot->IsAlive()) {
    return "you are dead";
  }
  if (bot->IsInCombat()) {
    if (std::holds_alternative<TalkAction>(action) ||
        std::holds_alternative<NpcAction>(action) ||
        std::holds_alternative<RestAction>(action) ||
        std::holds_alternative<WalkAction>(action) ||
        std::holds_alternative<EquipItemAction>(action) ||
        std::holds_alternative<DestroyItemAction>(action)) {
      return "you are in combat";
    }
    if (std::holds_alternative<SpendTalentsAction>(action)) {
      return "cannot spend talents in combat";
    }
    if (std::holds_alternative<MountAction>(action)) {
      return "cannot mount in combat";
    }
  }
  return "";
}

ExecuteResult ExecuteAction(Player* bot,
                            const BotAction& action,
                            ActionRunContext* ctx) {
  if (std::string err = ValidateAction(bot, action); !err.empty()) {
    return {.error = std::move(err)};
  }
  return std::visit(
      [&](const auto& a) -> ExecuteResult {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, KeepAction>) {
          return {};
        } else if constexpr (std::is_same_v<T, InvalidAction>) {
          return {.error = a.error.empty() ? "invalid action" : a.error};
        } else if constexpr (std::is_same_v<T, AttackAction>) {
          return ExecuteAttackAction(bot, a);
        } else if constexpr (std::is_same_v<T, LootAction>) {
          return FromOutcome(ExecuteLootAction(bot, a));
        } else if constexpr (std::is_same_v<T, GatherAction>) {
          return FromOutcome(ExecuteObjectAction(bot, a.guid, a.name));
        } else if constexpr (std::is_same_v<T, UseObjectAction>) {
          return FromOutcome(ExecuteObjectAction(bot, a.guid, a.name));
        } else if constexpr (std::is_same_v<T, TalkAction>) {
          return FromOutcome(ExecuteTalkAction(bot, a));
        } else if constexpr (std::is_same_v<T, RestAction>) {
          return FromOutcome(ExecuteRestAction(bot));
        } else if constexpr (std::is_same_v<T, WalkAction>) {
          const DungeonNavConfig nav = (ctx && ctx->dungeon_nav)
                                           ? *ctx->dungeon_nav
                                           : DungeonNavConfig{};
          WalkStepResult step =
              StartWalkAction(bot, a, ctx ? ctx->walk : nullptr, nav);
          if (!step.error.empty()) {
            return {.error = std::move(step.error),
                    .used = false,
                    .path_time = step.path_time};
          }
          return {.used = step.arrived, .path_time = step.path_time};
        } else if constexpr (std::is_same_v<T, InviteAction>) {
          return FromOutcome(ExecuteInviteAction(bot, a));
        } else if constexpr (std::is_same_v<T, AcceptInviteAction>) {
          return FromOutcome(ExecuteAcceptInviteAction(bot, a));
        } else if constexpr (std::is_same_v<T, DeclineInviteAction>) {
          return FromOutcome(ExecuteDeclineInviteAction(bot, a));
        } else if constexpr (std::is_same_v<T, LeaveGroupAction>) {
          return FromOutcome(ExecuteLeaveGroupAction(bot));
        } else if constexpr (std::is_same_v<T, ResurrectAction>) {
          return FromOutcome(ExecuteResurrectAction(bot, a));
        } else if constexpr (std::is_same_v<T, LootRollAction>) {
          return FromOutcome(ExecuteLootRollAction(bot, a));
        } else if constexpr (std::is_same_v<T, ReadyCheckAction>) {
          return FromOutcome(ExecuteReadyCheckAction(bot, a));
        } else if constexpr (std::is_same_v<T, SharedQuestAction>) {
          return FromOutcome(ExecuteSharedQuestAction(bot, a));
        } else if constexpr (std::is_same_v<T, ShareQuestAction>) {
          return FromOutcome(ExecuteShareQuestAction(bot, a));
        } else if constexpr (std::is_same_v<T, LfgAction>) {
          return FromOutcome(ExecuteLfgAction(bot, a));
        } else if constexpr (std::is_same_v<T, GroupPolicyAction>) {
          return FromOutcome(ExecuteGroupPolicyAction(bot, a));
        } else if constexpr (std::is_same_v<T, PopupAnswerAction>) {
          return FromOutcome(ExecutePopupAnswerAction(bot, a));
        } else if constexpr (std::is_same_v<T, TradeAction>) {
          return FromOutcome(ExecuteTradeAction(bot, a));
        } else if constexpr (std::is_same_v<T, CrowdControlAction>) {
          ActiveAct scratch;
          ActiveAct* out =
              (ctx && ctx->out_active_act) ? ctx->out_active_act : &scratch;
          const base::TimeTicks now =
              (ctx && !ctx->now.is_null()) ? ctx->now : base::TimeTicks::Now();
          ActStartResult res = ExecuteCrowdControlAction(bot, a, now, out);
          const bool started = !res.done && res.error.empty();
          return {.error = std::move(res.error),
                  .used = res.done,
                  .info = res.done ? a.label : "",
                  .act_started = started};
        } else if constexpr (std::is_same_v<T, TauntAction>) {
          ActiveAct scratch;
          ActiveAct* out =
              (ctx && ctx->out_active_act) ? ctx->out_active_act : &scratch;
          const base::TimeTicks now =
              (ctx && !ctx->now.is_null()) ? ctx->now : base::TimeTicks::Now();
          ActStartResult res = ExecuteTauntAction(bot, a, now, out);
          const bool started = !res.done && res.error.empty();
          return {.error = std::move(res.error),
                  .used = res.done,
                  .info = res.done ? a.label : "",
                  .act_started = started};
        } else if constexpr (std::is_same_v<T, FollowLeaderAction>) {
          return FromOutcome(ExecuteFollowLeaderAction(bot, a));
        } else if constexpr (std::is_same_v<T, MarkTargetAction>) {
          return FromOutcome(ExecuteMarkTargetAction(bot, a));
        } else if constexpr (std::is_same_v<T, MountAction>) {
          return FromItemOutcome(ExecuteMountAction(bot, a));
        } else if constexpr (std::is_same_v<T, DismountAction>) {
          return FromOutcome(ExecuteDismountAction(bot));
        } else if constexpr (std::is_same_v<T, NpcAction>) {
          return FromOutcome(ExecuteNpcAction(bot, a));
        } else if constexpr (std::is_same_v<T, SpendTalentsAction>) {
          return FromOutcome(ExecuteSpendTalentsAction(bot, a));
        } else if constexpr (std::is_same_v<T, UseBagItemAction>) {
          return FromItemOutcome(ExecuteUseBagItemAction(bot, a));
        } else if constexpr (std::is_same_v<T, EquipItemAction>) {
          return FromOutcome(ExecuteEquipItemAction(bot, a));
        } else if constexpr (std::is_same_v<T, DestroyItemAction>) {
          return FromOutcome(ExecuteDestroyItemAction(bot, a));
        } else if constexpr (std::is_same_v<T, StandingActAction>) {
          ActiveAct scratch;
          ActiveAct* out =
              (ctx && ctx->out_active_act) ? ctx->out_active_act : &scratch;
          const base::TimeTicks now =
              (ctx && !ctx->now.is_null()) ? ctx->now : base::TimeTicks::Now();
          ActStartResult res = StartAct(bot, a.command_id, a.act, now, out);
          return {.error = std::move(res.error), .used = res.done};
        }
      },
      action);
}

ExecuteResult ExecuteOption(Player* bot,
                            const OptionTarget& target,
                            PendingWalk* walk,
                            const DungeonNavConfig& dungeon_nav) {
  ActionRunContext ctx;
  ctx.walk = walk;
  ctx.dungeon_nav = &dungeon_nav;
  return ExecuteAction(bot, target.action, &ctx);
}

}  // namespace llm_brain
