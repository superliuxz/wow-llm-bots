// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "orders.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"

namespace harness {
namespace {

// The grammar's words, in one place for the parser, the printer and the GBNF.
constexpr std::array<std::pair<Who, std::string_view>, 8> kWho = {{
    {Who::kMe, "me"},
    {Who::kTank, "tank"},
    {Who::kOfftank, "offtank"},
    {Who::kHealer, "healer"},
    {Who::kLeader, "leader"},
    {Who::kAnyone, "anyone"},
    {Who::kBoss, "boss"},
    {Who::kIt, "it"},
}};
constexpr std::array<std::pair<Icon, std::string_view>, 8> kIcons = {{
    {Icon::kSkull, "skull"},
    {Icon::kCross, "cross"},
    {Icon::kSquare, "square"},
    {Icon::kMoon, "moon"},
    {Icon::kTriangle, "triangle"},
    {Icon::kDiamond, "diamond"},
    {Icon::kCircle, "circle"},
    {Icon::kStar, "star"},
}};
constexpr std::array<std::string_view, 4> kStates = {
    "flying", "grounded", "untargetable", "submerged"};
constexpr std::array<std::string_view, 4> kMyStates = {
    "in vehicle", "mounted", "swimming", "nearest to it"};
constexpr std::array<std::string_view, 3> kStops = {"casting", "attacking",
                                                    "moving"};
constexpr std::array<std::string_view, 4> kPetWords = {"stay", "follow",
                                                       "passive", "defensive"};

std::optional<Who> ParseWho(std::string_view s) {
  for (const auto& [who, word] : kWho) {
    if (s == word) {
      return who;
    }
  }
  return std::nullopt;
}

std::string_view WhoWord(Who who) {
  for (const auto& [w, word] : kWho) {
    if (w == who) {
      return word;
    }
  }
  return "?";
}

std::optional<Icon> ParseIcon(std::string_view s) {
  for (const auto& [icon, word] : kIcons) {
    if (s == word) {
      return icon;
    }
  }
  return std::nullopt;
}

std::string_view IconWord(Icon icon) {
  for (const auto& [i, word] : kIcons) {
    if (i == icon) {
      return word;
    }
  }
  return "?";
}

template <size_t N>
bool OneOf(std::string_view s, const std::array<std::string_view, N>& words) {
  for (std::string_view word : words) {
    if (s == word) {
      return true;
    }
  }
  return false;
}

// A name as a player sees it: starts with a letter, digit or apostrophe, then
// those, spaces, colons, commas or hyphens (tools/check-trigger-grammar.py).
bool IsName(std::string_view s) {
  if (s.empty() || s.back() == ' ') {
    return false;
  }
  const auto first = [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '\'';
  };
  if (!first(s.front())) {
    return false;
  }
  for (char c : s) {
    if (!first(c) && c != ' ' && c != ':' && c != ',' && c != '-') {
      return false;
    }
  }
  return true;
}

// "12" or "1.5".
std::optional<double> ParseNum(std::string_view s) {
  const size_t dot = s.find('.');
  const std::string_view whole = s.substr(0, dot);
  if (whole.empty() || whole.find_first_not_of("0123456789") != whole.npos) {
    return std::nullopt;
  }
  if (dot != s.npos) {
    const std::string_view frac = s.substr(dot + 1);
    if (frac.empty() || frac.find_first_not_of("0123456789") != frac.npos) {
      return std::nullopt;
    }
  }
  double value = 0;
  if (!base::StringToDouble(s, &value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> ParseCount(std::string_view s) {
  if (s.empty() || s.find_first_not_of("0123456789") != s.npos) {
    return std::nullopt;
  }
  return ParseNum(s);
}

std::string NumText(double value) {
  if (value == static_cast<double>(static_cast<long long>(value))) {
    return base::NumberToString(static_cast<long long>(value));
  }
  return base::StringPrintf("%g", value);
}

std::optional<std::string_view> After(std::string_view s, std::string_view p) {
  if (s.starts_with(p)) {
    return s.substr(p.size());
  }
  return std::nullopt;
}

std::optional<std::string_view> Before(std::string_view s, std::string_view p) {
  if (s.ends_with(p)) {
    return s.substr(0, s.size() - p.size());
  }
  return std::nullopt;
}

std::optional<Thing> ParseThing(std::string_view s) {
  Thing t;
  if (std::optional<Who> who = ParseWho(s)) {
    t.kind = Thing::Kind::kWho;
    t.who = *who;
    return t;
  }
  if (s == "my target") {
    t.kind = Thing::Kind::kMyTarget;
    return t;
  }
  if (s == "group" || s == "allies" || s == "focus") {
    t.kind = s == "group"    ? Thing::Kind::kGroup
             : s == "allies" ? Thing::Kind::kAllies
                             : Thing::Kind::kFocus;
    return t;
  }
  if (const auto icon_word = After(s, "marker ")) {
    if (std::optional<Icon> icon = ParseIcon(*icon_word)) {
      t.kind = Thing::Kind::kMarker;
      t.icon = *icon;
      return t;
    }
  }
  if (const auto kind = After(s, "nearest ")) {
    if (*kind == "ally" || *kind == "player" || *kind == "enemy" ||
        IsName(*kind)) {
      t.kind = Thing::Kind::kNearest;
      t.name = std::string(*kind);
      return t;
    }
  }
  if (const auto mech = After(s, "ally with "); mech && IsName(*mech)) {
    t.kind = Thing::Kind::kAllyWith;
    t.name = std::string(*mech);
    return t;
  }
  if (IsName(s)) {
    t.kind = Thing::Kind::kName;
    t.name = std::string(s);
    return t;
  }
  return std::nullopt;
}

std::string ThingText(const Thing& t) {
  switch (t.kind) {
    case Thing::Kind::kWho:
      return std::string(WhoWord(t.who));
    case Thing::Kind::kMyTarget:
      return "my target";
    case Thing::Kind::kMarker:
      return base::StrCat({"marker ", IconWord(t.icon)});
    case Thing::Kind::kNearest:
      return base::StrCat({"nearest ", t.name});
    case Thing::Kind::kAllyWith:
      return base::StrCat({"ally with ", t.name});
    case Thing::Kind::kGroup:
      return "group";
    case Thing::Kind::kAllies:
      return "allies";
    case Thing::Kind::kFocus:
      return "focus";
    case Thing::Kind::kName:
      return t.name;
  }
  return "?";
}

// "<30" / ">2": the comparison and its number (a count when `count`).
std::optional<std::pair<char, double>> ParseCmp(std::string_view s,
                                                bool count) {
  if (s.empty() || (s.front() != '<' && s.front() != '>')) {
    return std::nullopt;
  }
  const std::optional<double> v =
      count ? ParseCount(s.substr(1)) : ParseNum(s.substr(1));
  if (!v) {
    return std::nullopt;
  }
  return std::make_pair(s.front(), *v);
}

// "<N>s".
std::optional<double> ParseSeconds(std::string_view s) {
  const auto n = Before(s, "s");
  return n ? ParseNum(*n) : std::nullopt;
}

// The `mech ...` forms: a name, then what is said about it.
std::optional<Check> ParseMechCheck(std::string_view s) {
  Check t;
  const auto with_mech = [&](std::string_view mech, Check::Kind kind) {
    if (!IsName(mech)) {
      return false;
    }
    t.mech = std::string(mech);
    t.kind = kind;
    return true;
  };
  if (const auto mech = Before(s, " cast");
      mech && with_mech(*mech, Check::Kind::kCast)) {
    return t;
  }
  if (const auto mech = Before(s, " appears");
      mech && with_mech(*mech, Check::Kind::kAppears)) {
    return t;
  }
  // "... in last Ns" after "cast" or "appears".
  if (const size_t at = s.rfind(" in last "); at != s.npos) {
    const std::optional<double> secs = ParseSeconds(s.substr(at + 9));
    const std::string_view head = s.substr(0, at);
    if (secs) {
      if (const auto mech = Before(head, " cast");
          mech && with_mech(*mech, Check::Kind::kCastInLast)) {
        t.value = *secs;
        return t;
      }
      if (const auto mech = Before(head, " appears");
          mech && with_mech(*mech, Check::Kind::kAppearsInLast)) {
        t.value = *secs;
        return t;
      }
    }
  }
  if (const size_t at = s.rfind(" count"); at != s.npos) {
    if (const auto cmp = ParseCmp(s.substr(at + 6), /*count=*/true);
        cmp && with_mech(s.substr(0, at), Check::Kind::kCount)) {
      t.cmp = cmp->first;
      t.value = cmp->second;
      return t;
    }
  }
  // The "... on who" forms.
  const size_t on = s.rfind(" on ");
  if (on == s.npos) {
    return std::nullopt;
  }
  const std::optional<Who> who = ParseWho(s.substr(on + 4));
  if (!who) {
    return std::nullopt;
  }
  t.who = *who;
  const std::string_view head = s.substr(0, on);
  if (const auto mech = Before(head, " cast");
      mech && with_mech(*mech, Check::Kind::kCastOn)) {
    return t;
  }
  if (const size_t at = head.rfind(" stacks"); at != head.npos) {
    if (const auto cmp = ParseCmp(head.substr(at + 7), /*count=*/true);
        cmp && with_mech(head.substr(0, at), Check::Kind::kStacks)) {
      t.cmp = cmp->first;
      t.value = cmp->second;
      return t;
    }
  }
  if (const size_t at = head.rfind(" ends<"); at != head.npos) {
    if (const std::optional<double> secs = ParseSeconds(head.substr(at + 6));
        secs && with_mech(head.substr(0, at), Check::Kind::kEnds)) {
      t.cmp = '<';
      t.value = *secs;
      return t;
    }
  }
  if (with_mech(head, Check::Kind::kOn)) {
    return t;
  }
  return std::nullopt;
}

// The `thing ...` forms.
std::optional<Check> ParseThingCheck(std::string_view s) {
  Check t;
  if (const size_t at = s.find(" targets "); at != s.npos) {
    std::optional<Thing> a = ParseThing(s.substr(0, at));
    std::optional<Thing> b = ParseThing(s.substr(at + 9));
    if (a && b) {
      t.kind = Check::Kind::kTargets;
      t.a = std::move(*a);
      t.b = std::move(*b);
      return t;
    }
  }
  if (const size_t at = s.rfind(" hp"); at != s.npos) {
    if (const auto cmp = ParseCmp(s.substr(at + 3), /*count=*/false)) {
      if (std::optional<Thing> a = ParseThing(s.substr(0, at))) {
        t.kind = Check::Kind::kHp;
        t.a = std::move(*a);
        t.cmp = cmp->first;
        t.value = cmp->second;
        return t;
      }
    }
  }
  if (const size_t at = s.rfind(" is marked "); at != s.npos) {
    std::optional<Icon> icon = ParseIcon(s.substr(at + 11));
    std::optional<Thing> a = ParseThing(s.substr(0, at));
    if (icon && a) {
      t.kind = Check::Kind::kIsMarked;
      t.a = std::move(*a);
      t.icon = *icon;
      return t;
    }
  }
  if (const size_t at = s.rfind(" is "); at != s.npos) {
    const std::string_view state = s.substr(at + 4);
    std::optional<Thing> a = ParseThing(s.substr(0, at));
    if (OneOf(state, kStates) && a) {
      t.kind = Check::Kind::kIsState;
      t.a = std::move(*a);
      t.state = std::string(state);
      return t;
    }
  }
  for (const auto& [suffix, kind] :
       {std::pair{std::string_view(" in sight"), Check::Kind::kInSight},
        std::pair{std::string_view(" out of sight"),
                  Check::Kind::kOutOfSight}}) {
    if (const auto head = Before(s, suffix)) {
      if (std::optional<Thing> a = ParseThing(*head)) {
        t.kind = kind;
        t.a = std::move(*a);
        return t;
      }
    }
  }
  for (const auto& [word, kind] :
       {std::pair{std::string_view(" within "), Check::Kind::kWithin},
        std::pair{std::string_view(" beyond "), Check::Kind::kBeyond}}) {
    const size_t at = s.find(word);
    if (at == s.npos) {
      continue;
    }
    std::optional<Thing> a = ParseThing(s.substr(0, at));
    std::string_view rest = s.substr(at + word.size());
    std::optional<Thing> b;
    if (const size_t of = rest.find(" of "); of != rest.npos) {
      b = ParseThing(rest.substr(of + 4));
      if (!b) {
        continue;
      }
      rest = rest.substr(0, of);
    }
    const std::optional<double> yards = ParseNum(rest);
    if (a && yards) {
      t.kind = kind;
      t.a = std::move(*a);
      t.b = std::move(b);
      t.value = *yards;
      return t;
    }
  }
  return std::nullopt;
}

std::optional<Check> ParseCheck(std::string_view s) {
  Check t;
  if (const auto digit = After(s, "phase ");
      digit && digit->size() == 1 && base::IsAsciiDigit(digit->front())) {
    t.kind = Check::Kind::kPhase;
    t.value = digit->front() - '0';
    return t;
  }
  for (const auto& [prefix, kind] :
       {std::pair{std::string_view("after "), Check::Kind::kAfter},
        std::pair{std::string_view("every "), Check::Kind::kEvery}}) {
    if (const auto rest = After(s, prefix)) {
      if (const std::optional<double> secs = ParseSeconds(*rest)) {
        t.kind = kind;
        t.value = *secs;
        return t;
      }
    }
  }
  if (const auto rest = After(s, "mana_points")) {
    if (const auto cmp = ParseCmp(*rest, /*count=*/true)) {
      t.kind = Check::Kind::kManaPoints;
      t.cmp = cmp->first;
      t.value = cmp->second;
      return t;
    }
  }
  if (const auto rest = After(s, "mana")) {
    if (const auto cmp = ParseCmp(*rest, /*count=*/false)) {
      t.kind = Check::Kind::kMana;
      t.cmp = cmp->first;
      t.value = cmp->second;
      return t;
    }
  }
  if (const auto rest = After(s, "me behind ")) {
    if (std::optional<Thing> a = ParseThing(*rest)) {
      t.kind = Check::Kind::kMeBehind;
      t.a = std::move(*a);
      return t;
    }
  }
  if (const auto rest = After(s, "my target is ")) {
    if (std::optional<Thing> a = ParseThing(*rest)) {
      t.kind = Check::Kind::kMyTargetIs;
      t.a = std::move(*a);
      return t;
    }
  }
  if (const auto rest = After(s, "me has "); rest && IsName(*rest)) {
    t.kind = Check::Kind::kMeHas;
    t.mech = std::string(*rest);
    return t;
  }
  if (const auto rest = After(s, "me "); rest && OneOf(*rest, kMyStates)) {
    t.kind = Check::Kind::kMeState;
    t.state = std::string(*rest);
    return t;
  }
  if (std::optional<Check> mech = ParseMechCheck(s)) {
    return mech;
  }
  return ParseThingCheck(s);
}

std::string CheckText(const Check& t) {
  const std::string cmp =
      base::StrCat({std::string(1, t.cmp), NumText(t.value)});
  switch (t.kind) {
    case Check::Kind::kOn:
      return base::StrCat({t.mech, " on ", WhoWord(t.who)});
    case Check::Kind::kStacks:
      return base::StrCat({t.mech, " stacks", cmp, " on ", WhoWord(t.who)});
    case Check::Kind::kEnds:
      return base::StrCat(
          {t.mech, " ends<", NumText(t.value), "s on ", WhoWord(t.who)});
    case Check::Kind::kCast:
      return base::StrCat({t.mech, " cast"});
    case Check::Kind::kCastOn:
      return base::StrCat({t.mech, " cast on ", WhoWord(t.who)});
    case Check::Kind::kCastInLast:
      return base::StrCat({t.mech, " cast in last ", NumText(t.value), "s"});
    case Check::Kind::kAppears:
      return base::StrCat({t.mech, " appears"});
    case Check::Kind::kAppearsInLast:
      return base::StrCat({t.mech, " appears in last ", NumText(t.value), "s"});
    case Check::Kind::kCount:
      return base::StrCat({t.mech, " count", cmp});
    case Check::Kind::kWithin:
    case Check::Kind::kBeyond:
      return base::StrCat(
          {ThingText(t.a),
           t.kind == Check::Kind::kWithin ? " within " : " beyond ",
           NumText(t.value),
           t.b ? base::StrCat({" of ", ThingText(*t.b)}) : ""});
    case Check::Kind::kTargets:
      return base::StrCat(
          {ThingText(t.a), " targets ", t.b ? ThingText(*t.b) : "?"});
    case Check::Kind::kHp:
      return base::StrCat({ThingText(t.a), " hp", cmp});
    case Check::Kind::kMana:
      return base::StrCat({"mana", cmp});
    case Check::Kind::kManaPoints:
      return base::StrCat({"mana_points", cmp});
    case Check::Kind::kIsState:
      return base::StrCat({ThingText(t.a), " is ", t.state});
    case Check::Kind::kIsMarked:
      return base::StrCat({ThingText(t.a), " is marked ", IconWord(t.icon)});
    case Check::Kind::kInSight:
      return base::StrCat({ThingText(t.a), " in sight"});
    case Check::Kind::kOutOfSight:
      return base::StrCat({ThingText(t.a), " out of sight"});
    case Check::Kind::kMeBehind:
      return base::StrCat({"me behind ", ThingText(t.a)});
    case Check::Kind::kMyTargetIs:
      return base::StrCat({"my target is ", ThingText(t.a)});
    case Check::Kind::kMeState:
      return base::StrCat({"me ", t.state});
    case Check::Kind::kMeHas:
      return base::StrCat({"me has ", t.mech});
    case Check::Kind::kPhase:
      return base::StrCat({"phase ", NumText(t.value)});
    case Check::Kind::kAfter:
      return base::StrCat({"after ", NumText(t.value), "s"});
    case Check::Kind::kEvery:
      return base::StrCat({"every ", NumText(t.value), "s"});
  }
  return "?";
}

std::optional<Place> ParsePlace(std::string_view s) {
  Place p;
  for (const auto& [prefix, kind] :
       {std::pair{std::string_view("behind "), Place::Kind::kBehind},
        std::pair{std::string_view("in front of "), Place::Kind::kInFrontOf},
        std::pair{std::string_view("max range of "),
                  Place::Kind::kMaxRangeOf}}) {
    if (const auto rest = After(s, prefix)) {
      if (std::optional<Thing> t = ParseThing(*rest)) {
        p.kind = kind;
        p.thing = std::move(*t);
        return p;
      }
    }
  }
  if (const size_t at = s.find(" yd from "); at != s.npos) {
    std::optional<double> yards = ParseNum(s.substr(0, at));
    std::optional<Thing> t = ParseThing(s.substr(at + 9));
    if (yards && t) {
      p.kind = Place::Kind::kYdFrom;
      p.yards = *yards;
      p.thing = std::move(*t);
      return p;
    }
  }
  if (std::optional<Thing> t = ParseThing(s)) {
    p.kind = Place::Kind::kThing;
    p.thing = std::move(*t);
    return p;
  }
  return std::nullopt;
}

std::string PlaceText(const Place& p) {
  switch (p.kind) {
    case Place::Kind::kThing:
      return ThingText(p.thing);
    case Place::Kind::kBehind:
      return base::StrCat({"behind ", ThingText(p.thing)});
    case Place::Kind::kInFrontOf:
      return base::StrCat({"in front of ", ThingText(p.thing)});
    case Place::Kind::kMaxRangeOf:
      return base::StrCat({"max range of ", ThingText(p.thing)});
    case Place::Kind::kYdFrom:
      return base::StrCat({NumText(p.yards), " yd from ", ThingText(p.thing)});
  }
  return "?";
}

std::optional<Act> ParseAct(std::string_view s) {
  Act a;
  const auto thing_act = [&](std::string_view prefix, Act::Verb verb) {
    const auto rest = After(s, prefix);
    if (!rest) {
      return false;
    }
    std::optional<Thing> t = ParseThing(*rest);
    if (!t) {
      return false;
    }
    a.verb = verb;
    a.thing = std::move(*t);
    return true;
  };
  const auto name_act = [&](std::string_view prefix, Act::Verb verb) {
    const auto rest = After(s, prefix);
    if (!rest || !IsName(*rest)) {
      return false;
    }
    a.verb = verb;
    a.name = std::string(*rest);
    return true;
  };
  if (s == "jump" || s == "wake" || s == "leave vehicle") {
    a.verb = s == "jump"   ? Act::Verb::kJump
             : s == "wake" ? Act::Verb::kWake
                           : Act::Verb::kLeaveVehicle;
    return a;
  }
  if (s == "stop") {
    a.verb = Act::Verb::kStop;
    return a;
  }
  if (const auto what = After(s, "stop "); what && OneOf(*what, kStops)) {
    a.verb = Act::Verb::kStop;
    a.what = std::string(*what);
    return a;
  }
  if (const auto rest = After(s, "to ")) {
    if (std::optional<Place> p = ParsePlace(*rest)) {
      a.verb = Act::Verb::kTo;
      a.place = std::move(*p);
      return a;
    }
  }
  if (s == "away" || s.starts_with("away ")) {
    a.verb = Act::Verb::kAway;
    std::string_view rest = s == "away" ? "" : s.substr(5);
    // "away 10", "away group", "away nearest ally 10".
    const size_t space = rest.rfind(' ');
    const std::string_view last =
        space == rest.npos ? rest : rest.substr(space + 1);
    if (const std::optional<double> yards = ParseNum(last);
        yards && !last.empty()) {
      a.yards = *yards;
      rest = space == rest.npos ? "" : rest.substr(0, space);
    }
    if (!rest.empty()) {
      std::optional<Thing> t = ParseThing(rest);
      if (!t) {
        return std::nullopt;
      }
      a.thing = std::move(*t);
    }
    return a;
  }
  if (const auto rest = After(s, "use ")) {
    if (const size_t on = rest->rfind(" on "); on != rest->npos) {
      std::optional<Thing> t = ParseThing(rest->substr(on + 4));
      if (t && IsName(rest->substr(0, on))) {
        a.verb = Act::Verb::kUse;
        a.name = std::string(rest->substr(0, on));
        a.thing = std::move(*t);
        return a;
      }
    }
    if (name_act("use ", Act::Verb::kUse)) {
      return a;
    }
  }
  if (const auto rest = After(s, "mark ")) {
    const size_t space = rest->rfind(' ');
    if (space != rest->npos) {
      std::optional<Icon> icon = ParseIcon(rest->substr(space + 1));
      std::optional<Thing> t = ParseThing(rest->substr(0, space));
      if (icon && t) {
        a.verb = Act::Verb::kMark;
        a.thing = std::move(*t);
        a.icon = *icon;
        return a;
      }
    }
  }
  if (const auto rest = After(s, "pet ")) {
    if (OneOf(*rest, kPetWords)) {
      a.verb = Act::Verb::kPet;
      a.what = std::string(*rest);
      return a;
    }
    if (const auto target = After(*rest, "attack ")) {
      if (std::optional<Thing> t = ParseThing(*target)) {
        a.verb = Act::Verb::kPet;
        a.what = "attack";
        a.thing = std::move(*t);
        return a;
      }
    }
  }
  if (thing_act("face away ", Act::Verb::kFaceAway) ||
      thing_act("face ", Act::Verb::kFace) ||
      thing_act("avoid ", Act::Verb::kAvoid) ||
      thing_act("target ", Act::Verb::kTarget) ||
      thing_act("interact ", Act::Verb::kInteract) ||
      thing_act("board ", Act::Verb::kBoard) ||
      thing_act("kite ", Act::Verb::kKite) ||
      name_act("cancel ", Act::Verb::kCancel) ||
      name_act("equip ", Act::Verb::kEquip)) {
    return a;
  }
  return std::nullopt;
}

std::string ActText(const Act& a) {
  const std::string thing = a.thing ? ThingText(*a.thing) : "";
  switch (a.verb) {
    case Act::Verb::kTo:
      return base::StrCat({"to ", PlaceText(a.place)});
    case Act::Verb::kAway: {
      std::string out = "away";
      if (a.thing) {
        base::StrAppend(&out, {" ", thing});
      }
      if (a.yards) {
        base::StrAppend(&out, {" ", NumText(*a.yards)});
      }
      return out;
    }
    case Act::Verb::kAvoid:
      return base::StrCat({"avoid ", thing});
    case Act::Verb::kTarget:
      return base::StrCat({"target ", thing});
    case Act::Verb::kUse:
      return a.thing ? base::StrCat({"use ", a.name, " on ", thing})
                     : base::StrCat({"use ", a.name});
    case Act::Verb::kStop:
      return a.what.empty() ? "stop" : base::StrCat({"stop ", a.what});
    case Act::Verb::kFace:
      return base::StrCat({"face ", thing});
    case Act::Verb::kFaceAway:
      return base::StrCat({"face away ", thing});
    case Act::Verb::kJump:
      return "jump";
    case Act::Verb::kCancel:
      return base::StrCat({"cancel ", a.name});
    case Act::Verb::kMark:
      return base::StrCat({"mark ", thing, " ", IconWord(a.icon)});
    case Act::Verb::kInteract:
      return base::StrCat({"interact ", thing});
    case Act::Verb::kBoard:
      return base::StrCat({"board ", thing});
    case Act::Verb::kLeaveVehicle:
      return "leave vehicle";
    case Act::Verb::kEquip:
      return base::StrCat({"equip ", a.name});
    case Act::Verb::kPet:
      return a.what == "attack" ? base::StrCat({"pet attack ", thing})
                                : base::StrCat({"pet ", a.what});
    case Act::Verb::kKite:
      return base::StrCat({"kite ", thing});
    case Act::Verb::kWake:
      return "wake";
  }
  return "?";
}

std::string Literal(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

template <typename Words>
std::string Alternatives(const Words& words) {
  std::vector<std::string> out;
  for (const auto& word : words) {
    if constexpr (requires { word.second; }) {
      out.push_back(Literal(word.second));
    } else {
      out.push_back(Literal(word));
    }
  }
  return base::JoinString(out, " | ");
}

}  // namespace

std::optional<When> ParseWhen(std::string_view text) {
  When when;
  if (text == "always") {
    when.always = true;
    return when;
  }
  for (std::string_view part : base::SplitStringPieceUsingSubstr(
           text, " & ", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    Condition c;
    if (const auto rest = After(part, "not ")) {
      c.negated = true;
      part = *rest;
    }
    std::optional<Check> test = ParseCheck(part);
    if (!test ||
        when.conditions.size() >= static_cast<size_t>(kMaxConditions)) {
      return std::nullopt;
    }
    c.test = std::move(*test);
    when.conditions.push_back(std::move(c));
  }
  if (when.conditions.empty()) {
    return std::nullopt;
  }
  return when;
}

std::optional<std::vector<Act>> ParseActs(std::string_view text) {
  std::vector<Act> acts;
  for (std::string_view part : base::SplitStringPieceUsingSubstr(
           text, ", then ", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    std::optional<Act> act = ParseAct(part);
    if (!act || acts.size() >= static_cast<size_t>(kMaxOrderSteps)) {
      return std::nullopt;
    }
    acts.push_back(std::move(*act));
  }
  if (acts.empty()) {
    return std::nullopt;
  }
  return acts;
}

std::optional<Order> ParseOrder(std::string_view text) {
  const size_t arrow = text.find(" -> ");
  if (arrow == text.npos) {
    return std::nullopt;
  }
  std::optional<When> when = ParseWhen(text.substr(0, arrow));
  std::optional<std::vector<Act>> steps = ParseActs(text.substr(arrow + 4));
  if (!when || !steps) {
    return std::nullopt;
  }
  return Order{std::move(*when), std::move(*steps)};
}

std::string FormatOrder(const Order& order) {
  std::vector<std::string> conds;
  for (const Condition& c : order.when.conditions) {
    conds.push_back(base::StrCat({c.negated ? "not " : "", CheckText(c.test)}));
  }
  std::vector<std::string> steps;
  for (const Act& a : order.steps) {
    steps.push_back(ActText(a));
  }
  return base::StrCat(
      {order.when.always ? "always" : base::JoinString(conds, " & "), " -> ",
       base::JoinString(steps, ", then ")});
}

std::string OrderGrammar(const std::vector<std::string>& names) {
  std::vector<std::string> literals;
  for (const std::string& name : names) {
    if (IsName(name) && name.size() <= 60) {
      literals.push_back(Literal(name));
    }
  }
  if (literals.empty()) {
    return "";
  }
  return base::StrCat({
      "oline ::= \"\\norder: \" owhen \" -> \" oact (\", then \" oact){0,",
      base::NumberToString(kMaxOrderSteps - 1),
      "}\n"
      "owhen ::= \"always\" | ocond (\" & \" ocond){0,",
      base::NumberToString(kMaxConditions - 1),
      "}\n"
      "ocond ::= \"not \"? otest\n"
      "otest ::= oname \" on \" owho | oname \" stacks\" ocmp ocnt \" on \" "
      "owho"
      " | oname \" ends<\" onum \"s on \" owho | oname \" cast\""
      " | oname \" cast on \" owho | oname \" cast in last \" onum \"s\""
      " | oname \" appears\" | oname \" appears in last \" onum \"s\""
      " | oname \" count\" ocmp ocnt"
      " | othing (\" within \" | \" beyond \") onum (\" of \" othing)?"
      " | othing \" targets \" othing | othing \" hp\" ocmp onum"
      " | \"mana\" ocmp onum | \"mana_points\" ocmp ocnt"
      " | othing \" is \" ostate | othing \" is marked \" oicon"
      " | othing \" in sight\" | othing \" out of sight\""
      " | \"me behind \" othing | \"my target is \" othing"
      " | \"me \" omystate | \"me has \" oname"
      " | \"phase \" [0-9] | \"after \" onum \"s\" | \"every \" onum \"s\"\n"
      "ocmp ::= \"<\" | \">\"\n"
      "onum ::= [0-9] [0-9]? [0-9]? (\".\" [0-9])?\n"
      "ocnt ::= [0-9] [0-9]? [0-9]?\n"
      "owho ::= ",
      Alternatives(kWho),
      "\n"
      "othing ::= owho | \"my target\" | \"marker \" oicon | \"nearest \" okind"
      " | \"ally with \" oname | \"group\" | \"allies\" | \"focus\" | oname\n"
      "okind ::= \"ally\" | \"player\" | \"enemy\" | oname\n"
      "ostate ::= ",
      Alternatives(kStates),
      "\n"
      "omystate ::= ",
      Alternatives(kMyStates),
      "\n"
      "oicon ::= ",
      Alternatives(kIcons),
      "\n"
      "oact ::= \"to \" oplace | \"away\" (\" \" othing)? (\" \" onum)?"
      " | \"avoid \" othing | \"target \" othing | \"use \" oname (\" on \" "
      "othing)?"
      " | \"stop\" (\" casting\" | \" attacking\" | \" moving\")?"
      " | \"face \" othing | \"face away \" othing | \"jump\" | \"cancel \" "
      "oname"
      " | \"mark \" othing \" \" oicon | \"interact \" othing | \"board \" "
      "othing"
      " | \"leave vehicle\" | \"equip \" oname | \"pet \" opet | \"kite \" "
      "othing"
      " | \"wake\"\n"
      "oplace ::= othing | \"behind \" othing | \"in front of \" othing"
      " | \"max range of \" othing | onum \" yd from \" othing\n"
      "opet ::= \"attack \" othing | \"stay\" | \"follow\" | \"passive\""
      " | \"defensive\"\n"
      "oname ::= ",
      base::JoinString(literals, " | "),
      "\n",
  });
}

}  // namespace harness
