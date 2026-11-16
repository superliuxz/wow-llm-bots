// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "option_executor.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "AiObjectContext.h"
#include "Event.h"
#include "LootObjectStack.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Unit.h"
#include "options.h"

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
  Unit* unit = ObjectAccessor::GetUnit(*bot, ParseGuid(target.guid));
  if (!unit || !unit->IsInWorld()) {
    return target.name + " is gone";
  }
  AiObjectContext* context = ai->GetAiObjectContext();
  switch (target.kind) {
    case OptionTarget::Kind::kAttack:
      // Playerbots' attack action attacks the "current target" and runs its
      // own checks; the combat strategies take over from there.
      context->GetValue<Unit*>("current target")->Set(unit);
      if (!ai->DoSpecificAction("attack", Event(), /*silent=*/true)) {
        return "can't attack " + target.name + " now";
      }
      return "";
    case OptionTarget::Kind::kLoot:
      if (!unit->isDead()) {
        return target.name + " isn't dead";
      }
      // The loot strategy walks to the "loot target" and opens it.
      context->GetValue<LootObjectStack*>("available loot")
          ->Get()
          ->Add(unit->GetGUID());
      context->GetValue<LootObject>("loot target")
          ->Set(LootObject(bot, unit->GetGUID()));
      return "";
    case OptionTarget::Kind::kKeep:
      break;
  }
  return "";
}

}  // namespace llm_brain
