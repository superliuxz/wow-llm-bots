// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/tick_owner.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace llm_brain {
namespace {

constexpr auto kGatedActions = std::to_array<GatedAction>({
    // Gear: the brain's `Equip` choices would be undone.
    {"equip upgrade", ActionGate::kAlways},
    {"equip upgrades packet action", ActionGate::kAlways},
    // Popups the brain answers (and using a soulstone or ankh, Now #6).
    {"accept invitation", ActionGate::kBrainLed},
    {"accept quest share", ActionGate::kBrainLed},
    {"lfg accept", ActionGate::kBrainLed},
    {"lfg join", ActionGate::kBrainLed},
    {"lfg leave", ActionGate::kBrainLed},
    {"lfg role check", ActionGate::kBrainLed},
    {"lfg teleport", ActionGate::kBrainLed},
    {"loot roll", ActionGate::kBrainLed},
    {"ready check", ActionGate::kBrainLed},
    {"roll", ActionGate::kBrainLed},
    {"self resurrect", ActionGate::kBrainLed},
    // The trade window (Objective 9a): playerbots' handlers of
    // SMSG_TRADE_STATUS (the "trade status" trigger) and
    // SMSG_TRADE_STATUS_EXTENDED cancel a non-master's trade, ask random bots
    // for gold, and accept on their own.
    {"accept trade", ActionGate::kBrainLed},
    {"trade status extended", ActionGate::kBrainLed},
    // World-state checks in BrainPolicyListener.
    {"apply oil", ActionGate::kCondition},
    {"apply stone", ActionGate::kCondition},
    {"auto release", ActionGate::kCondition},
    {"flee", ActionGate::kCondition},
    {"return to stay position", ActionGate::kCondition},
    {"stay", ActionGate::kCondition},
});

}  // namespace

const char* TickOwnerName(TickOwner owner) {
  return owner == TickOwner::kBrain ? "brain-led" : "playerbots-led";
}

TickOwner TickOwnerFor(bool harness_connected,
                       bool in_battleground,
                       bool llm_healthy,
                       bool popup_deadline_missed) {
  return harness_connected && !in_battleground && llm_healthy &&
                 !popup_deadline_missed
             ? TickOwner::kBrain
             : TickOwner::kPlayerbots;
}

std::span<const GatedAction> GatedActions() {
  return kGatedActions;
}

const GatedAction* FindGatedAction(std::string_view name) {
  const auto it =
      std::find_if(kGatedActions.begin(), kGatedActions.end(),
                   [name](const GatedAction& a) { return a.name == name; });
  return it == kGatedActions.end() ? nullptr : &*it;
}

bool BlockedFor(const GatedAction& action, TickOwner owner) {
  switch (action.gate) {
    case ActionGate::kAlways:
      return true;
    case ActionGate::kBrainLed:
      return owner == TickOwner::kBrain;
    case ActionGate::kCondition:
      return false;
  }
  return false;
}

std::vector<std::string> MissingActionNames(
    std::span<const GatedAction> gated,
    std::span<const std::string> supported) {
  std::vector<std::string> missing;
  for (const GatedAction& a : gated) {
    if (std::find(supported.begin(), supported.end(), a.name) ==
        supported.end()) {
      missing.emplace_back(a.name);
    }
  }
  return missing;
}

std::string_view HealerCombatStrategyDelta(HealerSpec spec, bool in_group) {
  switch (spec) {
    case HealerSpec::kNone:
      return "";
    case HealerSpec::kPriestDiscipline:
      return in_group ? ",-holy dps,-shadow debuff,-shadow aoe,+disc"
                      : ",+holy dps,+shadow debuff,+shadow aoe";
    case HealerSpec::kPriestHoly:
      return in_group ? ",-holy dps,-shadow debuff,-shadow aoe,+holy heal"
                      : ",+holy dps,+shadow debuff,+shadow aoe";
    case HealerSpec::kDruidRestoration:
      return in_group ? ",-aoe,+resto" : ",+aoe";
    case HealerSpec::kShamanRestoration:
      return in_group ? ",-ele,-caster aoe,+resto" : ",+ele,+caster aoe";
    case HealerSpec::kPaladinHoly:
      return in_group ? ",-ret,-baoe,+holy" : ",+ret,+baoe";
  }
  return "";
}

}  // namespace llm_brain
