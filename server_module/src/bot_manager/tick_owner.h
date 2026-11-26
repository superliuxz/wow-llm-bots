// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Who owns a managed bot's decisions each tick: the LLM brain, or playerbots'
// own reflexes. BrainPolicyListener reads the bot's mode
// (BotSession::tick_owner) and the one table of playerbots actions it gates,
// so the boundary between the two lives here rather than in a chain of
// string compares.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_TICK_OWNER_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_TICK_OWNER_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace llm_brain {

// - kBrain ("brain-led"): a harness of this build is connected. The brain
//   answers popups (invites, rolls, ready checks, LFG, quest shares, self
//   resurrect); playerbots still fights, heals, follows and loots.
// - kPlayerbots ("playerbots-led"): no harness (or one that was refused), a
//   bot in a battleground, or one just logged in: playerbots answers popups
//   too, so a human's group never stalls on a bot (A-M9).
enum class TickOwner : uint8_t { kPlayerbots, kBrain };

const char* TickOwnerName(TickOwner owner);

// A managed bot's mode. Battlegrounds are left to playerbots (no policy
// listener or brain strategies are set up there).
TickOwner TickOwnerFor(bool harness_connected, bool in_battleground);

// How BrainPolicyListener gates one playerbots action.
enum class ActionGate : uint8_t {
  // Never runs for a managed bot (gear is the brain's choice even while the
  // harness is down: playerbots' auto-equip would undo it).
  kAlways,
  // Blocked while the bot is brain-led; playerbots runs it when not.
  kBrainLed,
  // Allowed or not by a world-state check in BrainPolicyListener.
  kCondition,
};

struct GatedAction {
  std::string_view name;  // Playerbots' action name (its context's key).
  ActionGate gate;
};

// Every playerbots action name BrainPolicyListener treats specially, so that
// startup can check each still exists (MissingActionNames).
std::span<const GatedAction> GatedActions();

// The entry for `name`, or nullptr when the action isn't gated.
const GatedAction* FindGatedAction(std::string_view name);

// Whether a kAlways / kBrainLed action is blocked for a bot in `owner` mode.
// kCondition actions are never blocked here (the listener decides).
bool BlockedFor(const GatedAction& action, TickOwner owner);

// The gated action names absent from `supported` (playerbots' action
// factories), in table order: a rename upstream would otherwise silently
// stop the gate.
std::vector<std::string> MissingActionNames(
    std::span<const GatedAction> gated,
    std::span<const std::string> supported);

// Healer talent specializations whose solo combat extras (damage strategies)
// are toggled on solo and off in a group.
enum class HealerSpec : uint8_t {
  kNone,
  kPriestDiscipline,
  kPriestHoly,
  kDruidRestoration,
  kShamanRestoration,
  kPaladinHoly,
};

// Combat strategy delta for a healer spec solo vs. grouped.
// Solo adds playerbots' damage extras so a solo healer can quest. Grouped
// removes those extras and explicitly restores the spec's healing strategy,
// because playerbots' combat strategy factories for Priest (`holy dps` vs.
// `holy heal`/`disc`), Shaman (`ele` vs. `resto`), and Paladin (`ret` vs.
// `holy`) are mutually exclusive — removing the solo DPS strategy alone would
// otherwise leave a grouped healer with no combat healing strategy.
std::string_view HealerCombatStrategyDelta(HealerSpec spec, bool in_group);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_TICK_OWNER_H_
