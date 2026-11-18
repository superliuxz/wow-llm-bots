// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Standing orders (docs/specs/triggers.md, "The grammar (final, 2026-11-17)"
// and "Decided (2026-11-18)"): `<when> -> <act>, then <act>`, written by the
// brain, watched for by the harness, carried out by the module. This file is
// the language: the types, the parser and the printer. Names (a spell, aura,
// unit, object, item as a player sees it) are free text here; which names a
// bot may write is the per-call grammar's job (OrderGrammar).

#ifndef HARNESS_ORDERS_H_
#define HARNESS_ORDERS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace harness {

inline constexpr int kMaxOrders = 7;      // Per plan.
inline constexpr int kMaxConditions = 4;  // Per `when`, joined by " & ".
inline constexpr int kMaxOrderSteps = 3;  // Per order, joined by ", then ".

enum class Who { kMe, kTank, kOfftank, kHealer, kLeader, kAnyone, kBoss, kIt };
enum class Icon {
  kSkull,
  kCross,
  kSquare,
  kMoon,
  kTriangle,
  kDiamond,
  kCircle,
  kStar
};

// Something an order points at.
struct Thing {
  enum class Kind {
    kWho,       // who
    kMyTarget,  // "my target"
    kMarker,    // "marker skull"
    kNearest,   // "nearest ally" / "nearest player" / "nearest enemy" /
                // "nearest <name>"
    kAllyWith,  // "ally with <mech>"
    kGroup,
    kAllies,
    kFocus,
    kName,
  };
  Kind kind = Kind::kWho;
  Who who = Who::kMe;
  Icon icon = Icon::kSkull;
  // kNearest: "ally", "player", "enemy" or a name; kAllyWith: the mechanic;
  // kName: the name.
  std::string name;

  bool operator==(const Thing&) const = default;
};

// One test of a condition (the `test` rule).
struct Check {
  enum class Kind {
    kOn,             // mech on who
    kStacks,         // mech stacks<cnt on who
    kEnds,           // mech ends<Ns on who
    kCast,           // mech cast
    kCastOn,         // mech cast on who
    kCastInLast,     // mech cast in last Ns
    kAppears,        // mech appears
    kAppearsInLast,  // mech appears in last Ns
    kCount,          // mech count<cnt
    kWithin,         // thing within N (of thing)
    kBeyond,         // thing beyond N (of thing)
    kTargets,        // thing targets thing
    kHp,             // thing hp<N
    kMana,           // mana<N (percent)
    kManaPoints,     // mana_points<cnt
    kIsState,        // thing is flying|grounded|untargetable|submerged
    kIsMarked,       // thing is marked icon
    kInSight,        // thing in sight
    kOutOfSight,     // thing out of sight
    kMeBehind,       // me behind thing
    kMyTargetIs,     // my target is thing
    kMeState,        // me in vehicle|mounted|swimming|nearest to it
    kMeHas,          // me has name
    kPhase,          // phase digit
    kAfter,          // after Ns
    kEvery,          // every Ns
  };
  Kind kind = Kind::kOn;
  std::string mech;  // The spell / aura / unit name; kMeHas: the name.
  Who who = Who::kMe;
  Thing a;                 // The first thing.
  std::optional<Thing> b;  // kWithin / kBeyond "of thing", kTargets' target.
  char cmp = '<';          // '<' or '>'.
  double value = 0;        // Seconds, yards, percent, a count, a phase.
  std::string state;       // kIsState / kMeState: the state as written.
  Icon icon = Icon::kSkull;

  bool operator==(const Check&) const = default;
};

struct Condition {
  bool negated = false;  // "not ..."
  Check test;

  bool operator==(const Condition&) const = default;
};

struct When {
  bool always = false;
  std::vector<Condition> conditions;  // Up to kMaxConditions, all must hold.

  bool operator==(const When&) const = default;
};

// Where `to` goes.
struct Place {
  enum class Kind { kThing, kBehind, kInFrontOf, kMaxRangeOf, kYdFrom };
  Kind kind = Kind::kThing;
  Thing thing;
  double yards = 0;  // kYdFrom.

  bool operator==(const Place&) const = default;
};

// One step (the `act` rule).
struct Act {
  enum class Verb {
    kTo,
    kAway,
    kAvoid,
    kTarget,
    kUse,
    kStop,
    kFace,
    kFaceAway,
    kJump,
    kCancel,
    kMark,
    kInteract,
    kBoard,
    kLeaveVehicle,
    kEquip,
    kPet,
    kKite,
    kWake,
  };
  Verb verb = Verb::kWake;
  Place place;                  // kTo.
  std::optional<Thing> thing;   // kAway (optional), kUse "on", and the rest.
  std::optional<double> yards;  // kAway's distance.
  std::string name;             // kUse / kCancel / kEquip.
  // kStop: "", "casting", "attacking" or "moving"; kPet: "attack", "stay",
  // "follow", "passive" or "defensive".
  std::string what;
  Icon icon = Icon::kSkull;  // kMark.

  bool operator==(const Act&) const = default;
};

struct Order {
  When when;
  std::vector<Act> steps;  // 1..kMaxOrderSteps, run one after another.

  bool operator==(const Order&) const = default;
};

// Parse the parts as the grammar writes them; nullopt if `text` isn't one.
std::optional<When> ParseWhen(std::string_view text);
std::optional<std::vector<Act>> ParseActs(std::string_view text);
std::optional<Order> ParseOrder(std::string_view text);

// The text of an order, as the grammar writes it (ParseOrder's inverse).
std::string FormatOrder(const Order& order);

// The GBNF rules for `order` lines in a plan (`\norder: ` + an order, up to
// kMaxOrders), with names limited to `names` (what the bot can see or knows:
// units, objects, auras, its spells and items); "" when `names` is empty.
// Rule names are prefixed `o` so they don't clash with the plan's.
std::string OrderGrammar(const std::vector<std::string>& names);

}  // namespace harness

#endif  // HARNESS_ORDERS_H_
