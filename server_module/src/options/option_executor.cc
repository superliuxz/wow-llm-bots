// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/option_executor.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "AiObjectContext.h"
#include "Creature.h"
#include "Define.h"
#include "Event.h"
#include "GameObject.h"
#include "LastMovementValue.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Unit.h"
#include "UnitDefines.h"
#include "UpdateFields.h"
#include "base/strings/stringprintf.h"
#include "options/options.h"

namespace llm_brain {

ObjectGuid ParseSnapshotGuid(std::string_view text) {
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
  }
  uint64_t raw = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), raw, 16);
  if (error != std::errc() || end != text.data() + text.size()) {
    return ObjectGuid::Empty;
  }
  return ObjectGuid(raw);
}

bool IsLootableObject(GameObject* go) {
  return go->GetGoType() == GAMEOBJECT_TYPE_CHEST ||
         go->GetGOInfo()->GetLootId() != 0;
}

bool IsInUseRange(Player* bot, GameObject* go) {
  if (!IsLootableObject(go)) {
    return go->IsAtInteractDistance(bot);
  }
  // Playerbots' "use" refuses a lootable object past its LootDistance
  // (UseItemAction::UseGameObject); 1 yd of margin for the walk's last step.
  const float limit = sPlayerbotAIConfig.LootDistance;
  return limit <= 0 || bot->GetDistance(go) <= std::max(limit - 1.0f, 1.0f);
}

namespace {

// Why playerbots' "use" refuses `go`, checked in the order its
// UseItemAction::UseGameObject checks (it only tells a master, so we ask the
// same questions). Empty when none of them applies.
std::string WhyCantUse(Player* bot, GameObject* go) {
  const std::string name = go->GetName();
  const float limit = sPlayerbotAIConfig.LootDistance;
  if (limit > 0 && bot->GetDistance(go) > limit) {
    return base::StringPrintf("%s is %.0f yd away", name.c_str(),
                              bot->GetDistance(go));
  }
  if (go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE)) {
    return name + " can't be used now";
  }
  if (go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) &&
      !go->ActivateToQuest(bot)) {
    return name + " needs a quest you don't have";
  }
  if (bot->IsNonMeleeSpellCast(false)) {
    return "you are casting";
  }
  if (bot->GetLootGUID()) {
    return "you are already looting";
  }
  if (bot->IsInFlight() || bot->m_mover != bot) {
    return "you can't do that now";
  }
  if (IsLootableObject(go)) {
    const float dz = go->GetPositionZ() - bot->GetPositionZ();
    if (std::abs(dz) > INTERACTION_DISTANCE - 2.0f) {
      return base::StringPrintf("%s is %.0f yd %s you", name.c_str(),
                                std::abs(dz), dz > 0 ? "above" : "below");
    }
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot)) {
      // Playerbots won't gather with bags more than 80% full
      // (LootObject::IsLootPossible).
      if (auto* bag_space =
              ai->GetAiObjectContext()->GetValue<uint8>("bag space");
          bag_space && bag_space->Get() > 80) {
        return base::StringPrintf("your bags are %d%% full",
                                  static_cast<int>(bag_space->Get()));
      }
    }
    LootObject loot(bot, go->GetGUID());
    if (!loot.IsLootPossible(bot)) {
      return "you lack the skill, tool or key for " + name;
    }
  }
  return "";
}

// Walks to (x, y, z) and tells playerbots it is the bot's current movement,
// so its own idle movement (wander, travel) doesn't replace the walk before it
// arrives; combat movement still does.
void WalkTo(Player* bot, float x, float y, float z) {
  bot->GetMotionMaster()->MovePoint(/*id=*/0, x, y, z);
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return;
  }
  auto* last =
      ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement");
  if (!last) {
    return;
  }
  const float speed = bot->GetSpeed(MOVE_RUN);
  const float ms =
      speed > 0 ? bot->GetExactDist(x, y, z) / speed * 1000.0f : 1000.0f;
  last->Get().Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), ms + 500.0f,
                  MovementPriority::MOVEMENT_NORMAL);
}

// Walks to the bot's side of `target`, within contact distance.
void WalkNextTo(Player* bot, WorldObject* target) {
  float x = target->GetPositionX();
  float y = target->GetPositionY();
  float z = target->GetPositionZ();
  // The angle is relative to the target's facing.
  target->GetClosePoint(x, y, z, bot->GetObjectSize(), CONTACT_DISTANCE,
                        target->GetAngle(bot) - target->GetOrientation(), bot);
  WalkTo(bot, x, y, z);
}

}  // namespace

std::string UseObjectNow(Player* bot, GameObject* go) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return "not a bot";
  }
  if (std::string why = WhyCantUse(bot, go); !why.empty()) {
    return why;
  }
  // Playerbots' "use" action takes the object as a chat link (the form
  // ChatHelper::FormatGameobject writes) and does what a click does: a chest,
  // herb or vein goes through its loot strategy (walk there, open, loot), any
  // other object gets CMSG_GAMEOBJ_USE when in reach.
  const std::string link =
      "|cffffff00|Hfound:" + std::to_string(go->GetGUID().GetRawValue()) + ":" +
      std::to_string(go->GetEntry()) + ":|h[" + go->GetName() + "]|h|r";
  if (!ai->DoSpecificAction("use", Event("llm-brain", link), /*silent=*/true)) {
    return "can't use " + go->GetName() + " now";
  }
  ai->ResetActionDuration();
  return "";
}

bool IsLootInProgress(Player* bot, ObjectGuid guid) {
  if (bot->GetLootGUID() == guid) {
    return true;
  }
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return false;
  }
  auto* loot_target =
      ai->GetAiObjectContext()->GetValue<LootObject>("loot target");
  return loot_target && loot_target->Get().guid == guid;
}

namespace {

ExecuteResult ExecuteObjectOption(Player* bot, const OptionTarget& target) {
  GameObject* go =
      ObjectAccessor::GetGameObject(*bot, ParseSnapshotGuid(target.guid));
  if (!go || !go->IsInWorld() || !go->isSpawned()) {
    return {target.name + " is gone"};
  }
  if (IsInUseRange(bot, go)) {
    std::string error = UseObjectNow(bot, go);
    const bool used = error.empty();
    return {std::move(error), used};
  }
  // Out of reach: walk there first, as a player would before clicking. The
  // world script uses it on arrival, and the harness times the step out if
  // the body never gets there.
  WalkNextTo(bot, go);
  return {};
}

}  // namespace

ExecuteResult ExecuteOption(Player* bot, const OptionTarget& target) {
  if (target.kind == OptionTarget::Kind::kKeep) {
    return {};  // The body carries on with what it was doing.
  }
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return {"not a bot"};
  }
  if (!bot->IsAlive()) {
    return {"you are dead"};  // A choice from before the bot died.
  }
  if (target.kind == OptionTarget::Kind::kGather ||
      target.kind == OptionTarget::Kind::kUse) {
    return ExecuteObjectOption(bot, target);
  }
  Unit* unit = ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(target.guid));
  if (!unit || !unit->IsInWorld()) {
    return {target.name + " is gone"};
  }
  AiObjectContext* context = ai->GetAiObjectContext();
  // Playerbots returns null for a value name it doesn't know (a renamed one
  // after an update): fail the choice, never dereference it.
  auto* current_target = context->GetValue<Unit*>("current target");
  auto* available_loot = context->GetValue<LootObjectStack*>("available loot");
  auto* loot_target = context->GetValue<LootObject>("loot target");
  if (!current_target || !available_loot || !available_loot->Get() ||
      !loot_target) {
    return {"the body can't do that (playerbots values missing)"};
  }
  switch (target.kind) {
    case OptionTarget::Kind::kAttack:
      // Playerbots' attack action attacks the "current target" and runs its
      // own checks; the combat strategies take over from there.
      {
        Unit* old_target = current_target->Get();
        current_target->Set(unit);
        if (!ai->DoSpecificAction("attack", Event(), /*silent=*/true)) {
          // Playerbots' AttackAction also refuses when the body already has
          // this target in attack mode (e.g. still closing in on it). That is
          // the choice already being carried out, not a failure: reporting
          // it woke the brain, which chose the same again, in a loop
          // (6 wakes in ~1 s, 2026-11-17).
          if (old_target == unit || bot->GetVictim() == unit) {
            ai->ResetActionDuration();
            return {};
          }
          // Refused: leave the body's target as it was, so nothing acts on
          // the refused one later.
          current_target->Set(old_target);
          return {"can't attack " + target.name + " now"};
        }
        ai->ResetActionDuration();
      }
      return {};
    case OptionTarget::Kind::kLoot:
      if (!unit->isDead()) {
        return {target.name + " isn't dead"};
      }
      {
        // The snapshot that offered it may be seconds old: someone else may
        // have looted it since. Only loot rights are checked here, not
        // IsLootPossible(), which also wants the corpse within 3 yd of the
        // bot's height: true once it has walked there, not up a slope or
        // stairs before it sets off.
        Creature* creature = unit->ToCreature();
        LootObject loot(bot, unit->GetGUID());
        if (!creature || loot.IsEmpty() || !bot->isAllowedToLoot(creature)) {
          return {"nothing to loot on " + target.name};
        }
        // Playerbots' loot strategy doesn't set off while the corpse is more
        // than 3 yd above or below (IsLootPossible): walk there ourselves, and
        // it takes over once level with it.
        if (std::abs(unit->GetPositionZ() - bot->GetPositionZ()) >
            INTERACTION_DISTANCE - 2.0f) {
          WalkNextTo(bot, unit);
        }
        // The loot strategy walks to the "loot target" and opens it; clear any
        // leftover check delay (SitDelay, RpgDelay, LootDelay) so UpdateAI acts
        // on the next world tick instead of sleeping it out.
        available_loot->Get()->Add(unit->GetGUID());
        loot_target->Set(loot);
        ai->ResetActionDuration();
      }
      return {};
    case OptionTarget::Kind::kGather:
    case OptionTarget::Kind::kUse:
    case OptionTarget::Kind::kKeep:
      break;  // Handled above.
  }
  return {};
}

}  // namespace llm_brain
