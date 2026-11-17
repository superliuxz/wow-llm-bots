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
#include "LootObjectStack.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Unit.h"
#include "options/options.h"

namespace llm_brain {
namespace {

// The snapshot writes guids as "0x" + 16 hex digits.
ObjectGuid ParseGuid(std::string_view text) {
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
  Unit* unit = ObjectAccessor::GetUnit(*bot, ParseGuid(target.guid));
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
    case OptionTarget::Kind::kKeep:
      break;
  }
  return "";
}

}  // namespace llm_brain
