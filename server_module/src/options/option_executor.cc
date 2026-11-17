// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/option_executor.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "AiObjectContext.h"
#include "Creature.h"
#include "Event.h"
#include "GameObject.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Unit.h"
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

bool UseObjectNow(Player* bot, GameObject* go) {
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return false;
  }
  // Playerbots' "use" action takes the object as a chat link (the form
  // ChatHelper::FormatGameobject writes) and does what a click does: a chest,
  // herb or vein goes through its loot strategy (walk there, open, loot), any
  // other object gets CMSG_GAMEOBJ_USE when in reach.
  const std::string link =
      "|cffffff00|Hfound:" + std::to_string(go->GetGUID().GetRawValue()) + ":" +
      std::to_string(go->GetEntry()) + ":|h[" + go->GetName() + "]|h|r";
  if (!ai->DoSpecificAction("use", Event("llm-brain", link), /*silent=*/true)) {
    return false;
  }
  ai->ResetActionDuration();
  return true;
}

namespace {

std::string ExecuteObjectOption(Player* bot, const OptionTarget& target) {
  GameObject* go =
      ObjectAccessor::GetGameObject(*bot, ParseSnapshotGuid(target.guid));
  if (!go || !go->IsInWorld() || !go->isSpawned()) {
    return target.name + " is gone";
  }
  if (IsLootableObject(go) || go->IsAtInteractDistance(bot)) {
    return UseObjectNow(bot, go) ? "" : "can't use " + target.name + " now";
  }
  // Playerbots only uses a non-loot object within reach and has no strategy
  // that walks to it: walk there, as a player would before clicking. The
  // world script uses it on arrival (UseObjectNow), and the harness times
  // the step out if the body never gets there.
  float x = go->GetPositionX();
  float y = go->GetPositionY();
  float z = go->GetPositionZ();
  // On the bot's side of the object (the angle is relative to its facing).
  go->GetClosePoint(x, y, z, bot->GetObjectSize(), CONTACT_DISTANCE,
                    go->GetAngle(bot) - go->GetOrientation(), bot);
  bot->GetMotionMaster()->MovePoint(/*id=*/0, x, y, z);
  return "";
}

}  // namespace

std::string ExecuteOption(Player* bot, const OptionTarget& target) {
  if (target.kind == OptionTarget::Kind::kKeep) {
    return "";  // The body carries on with what it was doing.
  }
  PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
  if (!ai) {
    return "not a bot";
  }
  if (!bot->IsAlive()) {
    return "you are dead";  // A choice from before the bot died.
  }
  if (target.kind == OptionTarget::Kind::kGather ||
      target.kind == OptionTarget::Kind::kUse) {
    return ExecuteObjectOption(bot, target);
  }
  Unit* unit = ObjectAccessor::GetUnit(*bot, ParseSnapshotGuid(target.guid));
  if (!unit || !unit->IsInWorld()) {
    return target.name + " is gone";
  }
  AiObjectContext* context = ai->GetAiObjectContext();
  // Playerbots returns null for a value name it doesn't know (a renamed one
  // after an update): fail the choice, never dereference it.
  auto* current_target = context->GetValue<Unit*>("current target");
  auto* available_loot = context->GetValue<LootObjectStack*>("available loot");
  auto* loot_target = context->GetValue<LootObject>("loot target");
  if (!current_target || !available_loot || !available_loot->Get() ||
      !loot_target) {
    return "the body can't do that (playerbots values missing)";
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
            return "";
          }
          // Refused: leave the body's target as it was, so nothing acts on
          // the refused one later.
          current_target->Set(old_target);
          return "can't attack " + target.name + " now";
        }
        ai->ResetActionDuration();
      }
      return "";
    case OptionTarget::Kind::kLoot:
      if (!unit->isDead()) {
        return target.name + " isn't dead";
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
          return "nothing to loot on " + target.name;
        }
        // The loot strategy walks to the "loot target" and opens it; clear any
        // leftover check delay (SitDelay, RpgDelay, LootDelay) so UpdateAI acts
        // on the next world tick instead of sleeping it out.
        available_loot->Get()->Add(unit->GetGUID());
        loot_target->Set(loot);
        ai->ResetActionDuration();
      }
      return "";
    case OptionTarget::Kind::kGather:
    case OptionTarget::Kind::kUse:
    case OptionTarget::Kind::kKeep:
      break;  // Handled above.
  }
  return "";
}

}  // namespace llm_brain
