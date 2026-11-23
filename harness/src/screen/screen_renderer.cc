// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/screen_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <flat_set>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

// Quest text is long (a quest log of 25 quests ran to thousands of
// characters): each piece is cut to this many bytes, at a character boundary,
// with "..." (review of f40b0b6, finding 7).
constexpr size_t kMaxQuestText = 240;
constexpr size_t kMaxTooltip = 400;

std::string Capped(std::string_view text, size_t max) {
  if (text.size() <= max) {
    return std::string(text);
  }
  size_t end = max;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
    --end;  // Not inside a UTF-8 character.
  }
  return base::StrCat({text.substr(0, end), "..."});
}

namespace pb = wlb::snapshot;

// Facing and "behind it" only matter within melee range.
constexpr float kMeleeYd = 10;

// Free bag slots are written from this many down (and "bags full" at 0).
constexpr uint32_t kFewBagSlots = 4;

// "THREAT_AGGRO" -> "threat aggro".
std::string Word(std::string_view name) {
  std::string out(name);
  for (char& c : out) {
    c = c == '_' ? ' ' : base::ToLowerASCII(c);
  }
  return out;
}

// The enum value's name, or "" when it is unset (zero).
template <typename Enum>
std::string NameOf(Enum value) {
  return value == static_cast<Enum>(0) ? "" : std::string(pb::EnumName(value));
}

// Values that are the normal case and so left out of the text.
bool IsDefaultName(std::string_view name) {
  return name == "IDLE" || name == "LEVEL" || name == "MEDIUM" ||
         name == "THREAT_NONE" || name == "NORMAL" || name == "NAMEPLATE";
}

// "key yes" for a set flag.
void AddFlag(std::vector<std::string>* bits, bool value, const char* key) {
  if (value) {
    bits->push_back(base::StrCat({key, " yes"}));
  }
}

// "key name" for a set, non-default enum value.
void AddEnum(std::vector<std::string>* bits,
             const std::string& name,
             const char* key) {
  if (!name.empty() && !IsDefaultName(name)) {
    bits->push_back(base::StrCat({key, " ", Word(name)}));
  }
}

// "health 24%" for a unit below full health (0 means not shown).
// Only when shown at all (a far unit's health isn't), and not full.
void AddPercent(std::vector<std::string>* bits,
                std::optional<float> pct,
                const char* key) {
  if (pct && *pct < 100) {
    bits->push_back(base::StringPrintf("%s %.0f%%", key, *pct));
  }
}

// A distance safe to sort and print: a missing or broken one (NaN, inf)
// counts as 0 (here), never as undefined behavior in std::sort.
float Finite(float yd) {
  return std::isfinite(yd) ? yd : 0.0f;
}

std::string JoinSet(const std::flat_set<std::string>& items,
                    std::string_view separator) {
  return base::JoinString(std::vector<std::string>(items.begin(), items.end()),
                          separator);
}

// Where something is: "28 yd SW, below, out of sight, you're behind it".
std::string Where(const pb::Relative& rel) {
  const std::string dir = NameOf(rel.dir);
  std::vector<std::string> parts;
  if (dir == "HERE" || (rel.dist_yd == 0 && dir.empty())) {
    parts.push_back("here");
  } else if (rel.dist_yd != 0) {
    parts.push_back(dir.empty()
                        ? base::StringPrintf("%.0f yd", Finite(rel.dist_yd))
                        : base::StringPrintf("%.0f yd %s", Finite(rel.dist_yd),
                                             dir.c_str()));
  } else {
    parts.push_back(dir);
  }
  if (rel.height == pb::Relative::ABOVE || rel.height == pb::Relative::BELOW) {
    parts.push_back(Word(pb::EnumName(rel.height)));
  }
  if (rel.in_los && !*rel.in_los) {  // Unset: not checked yet.
    parts.push_back("out of sight");
  }
  if (rel.behind_it) {
    parts.push_back("you're behind it");
  }
  if (rel.facing_me) {
    parts.push_back("facing you");
  }
  return base::JoinString(parts, ", ");
}

std::string TightWhere(const pb::Relative& rel_in) {
  pb::Relative rel = rel_in;
  if (rel.dist_yd > kMeleeYd) {
    rel.behind_it = false;
    rel.facing_me = false;
  }
  return Where(rel);
}

// A unit's pack: "(id 1234, size 3)".
std::string PackText(const pb::Unit::Pack& pack) {
  std::vector<std::string> parts;
  if (pack.id != 0) {
    parts.push_back(base::StrCat({"id ", base::NumberToString(pack.id)}));
  }
  if (pack.size != 0) {
    parts.push_back(base::StrCat({"size ", base::NumberToString(pack.size)}));
  }
  return base::StrCat({"(", base::JoinString(parts, ", "), ")"});
}

// Who a unit is: "level 5 troll priest", or "" when nothing is known.
std::string Who(const pb::Unit& u) {
  std::vector<std::string> parts;
  if (u.level) {
    parts.push_back(base::StrCat({"level ", base::NumberToString(*u.level)}));
  }
  if (!u.looks_like.empty()) {
    parts.push_back(u.looks_like);
  }
  if (u.player && !u.player->class_.empty()) {
    parts.push_back(base::ToLowerASCII(u.player->class_));
  }
  return base::JoinString(parts, " ");
}

// Everything about a unit except its name and position, in the order of the
// Python prototype: who it is, how it looks, how hurt, what it does, then the
// rest of its fields by field number.
std::string UnitLook(const pb::Unit& u) {
  std::vector<std::string> bits;
  if (std::string who = Who(u); !who.empty()) {
    bits.push_back(std::move(who));
  }
  for (const std::string& name :
       {NameOf(u.level_color), NameOf(u.kind), NameOf(u.reaction),
        NameOf(u.classification), NameOf(u.size)}) {
    if (!name.empty() && !IsDefaultName(name)) {
      bits.push_back(Word(name));
    }
  }
  AddPercent(&bits, u.health_pct, "health");
  AddPercent(&bits, u.power_pct, "power");
  const std::string state = NameOf(u.activity.state);
  if (!state.empty() && !IsDefaultName(state)) {
    bits.push_back(Word(state) + (u.activity.with.empty()
                                      ? ""
                                      : base::StrCat({" ", u.activity.with})));
  }
  // `dead` is already in the activity.
  AddFlag(&bits, u.lootable, "lootable");
  AddFlag(&bits, u.skinnable, "skinnable");
  AddFlag(&bits, u.tapped_by_other, "tapped by other");
  AddFlag(&bits, u.targeting_me, "targeting me");
  if (!u.target.empty()) {
    bits.push_back(base::StrCat({"target ", u.target}));
  }
  AddEnum(&bits, NameOf(u.threat_status), "threat status");
  AddEnum(&bits, NameOf(u.quest_marker), "quest marker");
  AddFlag(&bits, u.pvp_flagged, "pvp flagged");
  AddEnum(&bits, NameOf(u.raid_marker), "raid marker");
  if (u.pack && (u.pack->id != 0 || u.pack->size != 0)) {
    bits.push_back(base::StrCat({"pack ", PackText(*u.pack)}));
  }
  if (!u.visual_state.empty()) {
    std::vector<std::string> states;
    for (pb::Unit::VisualState vs : u.visual_state) {
      states.push_back(Word(pb::EnumName(vs)));
    }
    bits.push_back(
        base::StrCat({"visual state [", base::JoinString(states, "; "), "]"}));
  }
  if (!u.quest_relevant.empty()) {
    bits.push_back(base::StrCat({"quest relevant ", u.quest_relevant}));
  }
  if (!u.subtitle.empty()) {
    bits.push_back(base::StrCat({"subtitle ", u.subtitle}));
  }
  return base::JoinString(bits, ", ");
}

// "herb, state active": the usual state of an object (usable, ready) is left
// out.
std::string ObjectLook(const pb::GameObject& go) {
  std::vector<std::string> bits;
  const std::string kind = NameOf(go.kind);
  if (!kind.empty() && !IsDefaultName(kind)) {
    bits.push_back(Word(kind));
  }
  AddPercent(&bits, go.health_pct, "health");
  // A usable object in the ready state is the normal case; written only when
  // it is the first thing said (the Python prototype's string replace left
  // those in).
  if (go.usable && bits.empty()) {
    bits.push_back("usable yes");
  }
  if (!go.state.empty() && !(go.state == "ready" && !bits.empty())) {
    bits.push_back(base::StrCat({"state ", go.state}));
  }
  AddFlag(&bits, go.highlighted, "highlighted");
  if (!go.owner.empty()) {
    bits.push_back(base::StrCat({"owner ", go.owner}));
  }
  if (go.capture_progress_pct != 0) {
    bits.push_back(
        base::StringPrintf("capture progress pct %g", go.capture_progress_pct));
  }
  return base::JoinString(bits, ", ");
}

// What the bot can do with a unit or object.
struct Thing {
  std::string guid;
  std::string name;
  std::string look;
  pb::Relative rel;
  std::optional<OptionKind> option;
  int module_option = 0;  // 1-based index into snapshot.options, or 0.
};

std::optional<OptionKind> UnitOption(const pb::Unit& u) {
  if (u.kind != pb::Unit::NPC && u.kind != pb::Unit::CRITTER) {
    return std::nullopt;
  }
  if (u.dead) {
    return u.lootable ? std::optional(OptionKind::kLoot) : std::nullopt;
  }
  switch (u.reaction) {
    case pb::Unit::HOSTILE:
    case pb::Unit::NEUTRAL:
      return OptionKind::kAttack;
    case pb::Unit::FRIENDLY:
      return OptionKind::kTalk;
    default:
      return std::nullopt;
  }
}

std::optional<OptionKind> ObjectOption(const pb::GameObject& go) {
  if (!go.usable) {
    return std::nullopt;
  }
  return go.kind == pb::GameObject::HERB || go.kind == pb::GameObject::ORE
             ? OptionKind::kGather
             : OptionKind::kUse;
}

std::string_view OptionVerb(OptionKind kind) {
  switch (kind) {
    case OptionKind::kAttack:
      return "Attack ";
    case OptionKind::kTalk:
      return "Talk to ";
    case OptionKind::kLoot:
      return "Loot ";
    case OptionKind::kGather:
      return "Gather ";
    case OptionKind::kUse:
      return "Use ";
    case OptionKind::kKeep:
    case OptionKind::kExplore:
    case OptionKind::kRest:
    case OptionKind::kTown:
    case OptionKind::kInvite:
    case OptionKind::kAcceptInvite:
    case OptionKind::kDeclineInvite:
    case OptionKind::kLeaveGroup:
      return "";
  }
  return "";
}

ScreenOption FromModuleOption(const pb::Option& option) {
  OptionKind kind = OptionKind::kKeep;
  switch (option.kind) {
    case pb::Option::ATTACK:
      kind = OptionKind::kAttack;
      break;
    case pb::Option::TALK:
      kind = OptionKind::kTalk;
      break;
    case pb::Option::LOOT:
      kind = OptionKind::kLoot;
      break;
    case pb::Option::GATHER:
      kind = OptionKind::kGather;
      break;
    case pb::Option::USE:
      kind = OptionKind::kUse;
      break;
    case pb::Option::REST:
      kind = OptionKind::kRest;
      break;
    case pb::Option::EXPLORE:
      kind = OptionKind::kExplore;
      break;
    case pb::Option::GO_TO:
      kind = OptionKind::kTown;
      break;
    case pb::Option::INVITE:
      kind = OptionKind::kInvite;
      break;
    case pb::Option::ACCEPT_INVITE:
      kind = OptionKind::kAcceptInvite;
      break;
    case pb::Option::DECLINE_INVITE:
      kind = OptionKind::kDeclineInvite;
      break;
    case pb::Option::LEAVE_GROUP:
      kind = OptionKind::kLeaveGroup;
      break;
    case pb::Option::KEEP:
    case pb::Option::KIND_UNSPECIFIED:
      kind = OptionKind::kKeep;
      break;
  }
  return {kind, option.guid, option.name};
}

// One line per distinct (name, look): the same thing seen several times is
// one line with a count and the range of distances. When `has_module_options`
// is false, entries the bot can act on get the next option number; when true,
// only groups carrying a `module_option` from `snapshot.options` get that
// exact number.
std::vector<std::string> ThingLines(const std::vector<Thing>& things,
                                    bool has_module_options,
                                    std::vector<OptionKind>* options,
                                    std::vector<ScreenOption>* details,
                                    std::vector<bool>* placed) {
  struct Group {
    std::string name;
    std::string look;
    std::optional<OptionKind> option;
    int module_option = 0;
    std::vector<const Thing*> items;
  };
  std::vector<Group> groups;
  for (const Thing& t : things) {
    const std::string name = t.name.empty() ? "?" : t.name;
    bool merged = false;
    for (Group& g : groups) {
      if (g.name == name && g.look == t.look && g.option == t.option &&
          (g.module_option == 0 || t.module_option == 0)) {
        if (g.module_option == 0 && t.module_option > 0) {
          g.module_option = t.module_option;
        }
        g.items.push_back(&t);
        merged = true;
        break;
      }
    }
    if (!merged) {
      groups.push_back({name, t.look, t.option, t.module_option, {&t}});
    }
  }

  std::vector<std::string> lines;
  for (const Group& group : groups) {
    std::string mark = "-";
    std::string verb;
    if (has_module_options) {
      if (group.module_option > 0) {
        const size_t idx = static_cast<size_t>(group.module_option - 1);
        mark = base::StrCat({base::NumberToString(group.module_option), "."});
        verb = std::string(OptionVerb((*options)[idx]));
        (*placed)[idx] = true;
      }
    } else if (group.option) {
      options->push_back(*group.option);
      details->push_back({*group.option, group.items[0]->guid, group.name});
      mark = base::StrCat({base::NumberToString(options->size()), "."});
      verb = std::string(OptionVerb(*group.option));
    }
    if (group.items.size() == 1) {
      lines.push_back(
          base::StrCat({mark, " ", verb, group.name, ": ", group.look, "; ",
                        TightWhere(group.items[0]->rel)}));
      continue;
    }
    std::vector<float> dists;
    std::flat_set<std::string> dirs;
    for (const Thing* t : group.items) {
      dists.push_back(Finite(t->rel.dist_yd));
      const std::string dir = NameOf(t->rel.dir);
      if (!dir.empty()) {
        dirs.insert(dir);
      }
    }
    std::sort(dists.begin(), dists.end());
    const std::string dir_suffix =
        dirs.empty() ? "" : base::StrCat({" ", JoinSet(dirs, "/")});
    lines.push_back(base::StringPrintf(
        "%s %s%zux %s: %s; %.0f-%.0f yd%s", mark.c_str(), verb.c_str(),
        group.items.size(), group.name.c_str(), group.look.c_str(),
        dists.front(), dists.back(), dir_suffix.c_str()));
  }
  return lines;
}

// Nameless far units (a model is all a player sees): counts by look, with the
// range of distances and the directions.
std::string FarSummary(const std::vector<const pb::Unit*>& units) {
  std::vector<std::string> order;
  std::flat_map<std::string, std::vector<const pb::Relative*>> groups;
  for (const pb::Unit* u : units) {
    const std::string look = UnitLook(*u);
    if (groups.find(look) == groups.end()) {
      order.push_back(look);
    }
    groups[look].push_back(&u->rel);
  }
  std::vector<std::string> parts;
  for (const std::string& look : order) {
    std::vector<float> dists;
    std::flat_set<std::string> dirs;
    for (const pb::Relative* rel : groups[look]) {
      dists.push_back(Finite(rel->dist_yd));
      const std::string dir = NameOf(rel->dir);
      if (!dir.empty()) {
        dirs.insert(dir);
      }
    }
    std::sort(dists.begin(), dists.end());
    const std::string span =
        dists.front() == dists.back()
            ? base::StringPrintf("%.0f", dists.front())
            : base::StringPrintf("%.0f-%.0f", dists.front(), dists.back());
    const std::string joined_dirs = JoinSet(dirs, "/");
    parts.push_back(base::StringPrintf("%zux %s (%s yd %s)",
                                       groups[look].size(), look.c_str(),
                                       span.c_str(), joined_dirs.c_str()));
  }
  return base::JoinString(parts, "; ");
}

// The main-screen cues (snapshot.md §4, `indicators`): free bag slots, the
// damaged-armor figure, the talent and mail icons. Only the ones showing; bag
// space only once it runs low (with plenty of room it is noise).
void IndicatorBits(const pb::Self_Indicators& ind,
                   std::vector<std::string>* bits) {
  if (ind.bag_free_slots == 0u) {
    bits->push_back("bags full");
  } else if (ind.bag_free_slots && *ind.bag_free_slots <= kFewBagSlots) {
    bits->push_back(base::StrCat(
        {"bags ", base::NumberToString(*ind.bag_free_slots), " free"}));
  }
  std::vector<std::string> damaged;
  std::vector<std::string> broken;
  for (const pb::Self_Indicators::Durability& d : ind.durability_warning) {
    const std::string slot = d.slot.empty() ? "?" : base::ToLowerASCII(d.slot);
    if (d.level == pb::Self_Indicators::Durability::BROKEN) {
      broken.push_back(slot);
    } else if (d.level == pb::Self_Indicators::Durability::DAMAGED) {
      damaged.push_back(slot);
    }
  }
  if (!damaged.empty()) {
    bits->push_back(
        base::StrCat({"damaged armor ", base::JoinString(damaged, ", ")}));
  }
  if (!broken.empty()) {
    bits->push_back(
        base::StrCat({"broken armor ", base::JoinString(broken, ", ")}));
  }
  if (ind.talent_points_unspent) {
    bits->push_back("unspent talent points");
  }
  if (ind.new_mail) {
    bits->push_back("new mail");
  }
}

bool IsSelfPartyLeader(const pb::Snapshot& snapshot) {
  const pb::Self& self = snapshot.base.self;
  if (self.group_leader || (self.unit.group && self.unit.group->leader)) {
    return true;
  }
  if (snapshot.ext.party && !snapshot.ext.party->leader.empty()) {
    const std::string& leader = snapshot.ext.party->leader;
    if ((!snapshot.bot.empty() && leader == snapshot.bot) ||
        (!self.unit.name.empty() && leader == self.unit.name)) {
      return true;
    }
  }
  return false;
}

std::string SelfLine(const pb::Self& s, bool party_leader) {
  std::vector<std::string> who;
  who.push_back(
      s.unit.level.value_or(0) != 0
          ? base::StrCat({"level ", base::NumberToString(*s.unit.level)})
          : "level ?");
  const std::string gender = NameOf(s.gender);
  if (!gender.empty()) {
    who.push_back(Word(gender));
  }
  if (s.unit.player && !s.unit.player->class_.empty()) {
    who.push_back(base::ToLowerASCII(s.unit.player->class_));
  }
  std::vector<std::string> bits;
  bits.push_back(base::StrCat({s.unit.name.empty() ? "?" : s.unit.name, ", ",
                               base::JoinString(who, " ")}));
  bits.push_back(base::StrCat({"health ", base::NumberToString(s.health), "/",
                               base::NumberToString(s.health_max)}));
  if (s.power_max != 0) {
    const std::string power_type = NameOf(s.power_type);
    bits.push_back(
        base::StrCat({power_type.empty() ? "power" : Word(power_type), " ",
                      base::NumberToString(s.power), "/",
                      base::NumberToString(s.power_max)}));
  }
  if (s.xp_next != 0) {
    bits.push_back(base::StrCat(
        {"xp ", base::NumberToString(s.xp), "/",
         base::NumberToString(s.xp_next),
         s.rested_xp != 0
             ? base::StrCat(
                   {" (rested ", base::NumberToString(s.rested_xp), ")"})
             : ""}));
  }
  if (s.money != 0) {
    bits.push_back(
        base::StrCat({"money ", base::NumberToString(s.money), "c"}));
  }
  // The life state first when not alive, then the flags.
  const std::string life = NameOf(s.state);
  if (!life.empty() && life != "ALIVE") {
    bits.push_back(Word(life));
  }
  const struct {
    bool value;
    const char* word;
  } flags[] = {{s.in_combat, "in combat"},
               {s.resting, "resting"},
               {s.mounted, "mounted"},
               {s.swimming, "swimming"},
               {s.in_flight, "in flight"},
               {s.camping, "camping"},
               {s.drunk != pb::Self::DRUNK_UNSPECIFIED, "drunk"},
               {party_leader, "party leader"}};
  for (const auto& flag : flags) {
    if (flag.value) {
      bits.push_back(flag.word);
    }
  }
  IndicatorBits(s.indicators, &bits);
  if (!s.unit.auras.empty()) {
    std::vector<std::string> buffs;
    std::vector<std::string> debuffs;
    for (const pb::Aura& aura : s.unit.auras) {
      if (aura.name.empty()) {
        continue;
      }
      std::string text = aura.name;
      if (aura.stacks > 1) {
        base::StrAppend(&text, {" x", base::NumberToString(aura.stacks)});
      }
      if (aura.remaining_s) {
        const float secs = Finite(*aura.remaining_s);
        if (secs > 0.0f) {
          const int rounded = static_cast<int>(std::lround(secs));
          base::StrAppend(
              &text, {" (",
                      rounded >= 60 ? base::NumberToString(rounded / 60) + "m"
                                    : base::NumberToString(rounded) + "s",
                      ")"});
        }
      }
      (aura.harmful ? debuffs : buffs).push_back(std::move(text));
    }
    if (!buffs.empty()) {
      bits.push_back(base::StrCat({"buffs: ", base::JoinString(buffs, ", ")}));
    }
    if (!debuffs.empty()) {
      bits.push_back(
          base::StrCat({"debuffs: ", base::JoinString(debuffs, ", ")}));
    }
  }
  return base::JoinString(bits, "; ");
}

std::string WhereLine(const pb::Where& w) {
  std::vector<std::string> places;
  if (!w.subzone.empty()) {
    places.push_back(w.subzone);
  }
  if (!w.zone.empty()) {
    places.push_back(w.zone);
  }
  std::string out = base::JoinString(places, ", ");
  if (!w.map.empty()) {
    base::StrAppend(&out, {" (", w.map, ")"});
  }
  if (w.indoors) {
    base::StrAppend(&out, {"; indoors"});
  }
  if (w.in_instance) {
    base::StrAppend(&out, {"; in an instance"});
  }
  return out;
}

// "name: look; where", leaving out the parts that are empty.
std::string NamedLine(std::string_view name,
                      const std::string& look,
                      const std::string& where) {
  std::string out(name.empty() ? "?" : name);
  if (!look.empty()) {
    base::StrAppend(&out, {": ", look});
  }
  if (!where.empty()) {
    base::StrAppend(&out, {"; ", where});
  }
  return out;
}

// Where a unit is, or "" when the snapshot doesn't say.
std::string UnitWhere(const pb::Unit& u) {
  return u.rel == pb::Relative{} ? "" : TightWhere(u.rel);
}

// A unit already written under "around me" (matched by guid) is not written
// again in full: the frames below only add what that line lacks.
bool IsAroundMe(const pb::Unit& u, const std::flat_set<std::string>& guids) {
  return !u.guid.empty() && guids.contains(u.guid);
}

// A unit in a frame (target, focus). When it is around me, only its health
// and death: they tell which of several same-named units it is.
std::string FrameLine(const pb::Unit& u, bool around_me) {
  if (!around_me) {
    return NamedLine(u.name, UnitLook(u), UnitWhere(u));
  }
  std::vector<std::string> bits;
  AddPercent(&bits, u.health_pct, "health");
  if (u.dead) {
    bits.push_back("dead");
  }
  return NamedLine(u.name, base::JoinString(bits, ", "), "");
}

// "target: Mottled Boar: health 45%; targets you; my threat 100%".
std::string TargetLine(const pb::Target& t,
                       const pb::Unit& target,
                       bool around_me,
                       std::string_view my_name) {
  pb::Unit u = target;
  if (!t.target_of_target.empty()) {  // Said once, below.
    u.target.clear();
    u.targeting_me = false;
  }
  std::vector<std::string> parts = {FrameLine(u, around_me)};
  if (!t.target_of_target.empty()) {
    parts.push_back(t.target_of_target == my_name
                        ? "targets you"
                        : base::StrCat({"targets ", t.target_of_target}));
  }
  if (Finite(t.my_threat_pct) > 0) {
    parts.push_back(base::StringPrintf("my threat %.0f%%", t.my_threat_pct));
  }
  return base::JoinString(parts, "; ");
}

// Names in first-seen order, the same name several times as "2x Kobold".
std::string CountedNames(const std::vector<std::string>& names) {
  std::vector<std::string> order;
  std::flat_map<std::string, int> counts;
  for (const std::string& name : names) {
    if (counts[name]++ == 0) {
      order.push_back(name);
    }
  }
  std::vector<std::string> parts;
  for (const std::string& name : order) {
    const int count = counts[name];
    parts.push_back(
        count == 1 ? name
                   : base::StrCat({base::NumberToString(count), "x ", name}));
  }
  return base::JoinString(parts, ", ");
}

// "attacked by 2x Kobold Vermin; my threat Kobold Vermin 40% (top Gatda);
// combo points 3". `skip_threat_on` already has its threat on the target line.
std::string CombatLine(const pb::Combat& c,
                       std::string_view my_name,
                       std::string_view skip_threat_on) {
  std::vector<std::string> bits;
  if (!c.attackers.empty()) {
    std::vector<std::string> names;
    for (const pb::Unit& u : c.attackers) {
      names.push_back(!u.name.empty()         ? u.name
                      : !u.looks_like.empty() ? u.looks_like
                                              : "?");
    }
    bits.push_back(base::StrCat({"attacked by ", CountedNames(names)}));
  }
  if (!c.duel_opponent.empty()) {
    bits.push_back(base::StrCat({"dueling ", c.duel_opponent}));
  }
  std::vector<std::string> threat;
  for (const pb::Combat::Threat& t : c.threat) {
    if (t.unit.empty() || t.unit == skip_threat_on) {
      continue;
    }
    std::string entry = base::StringPrintf("%s %.0f%%", t.unit.c_str(),
                                           Finite(t.my_threat_pct));
    if (!t.top.empty() && t.top != my_name) {
      base::StrAppend(&entry, {" (top ", t.top, ")"});
    }
    threat.push_back(std::move(entry));
  }
  if (!threat.empty()) {
    bits.push_back(
        base::StrCat({"my threat ", base::JoinString(threat, ", ")}));
  }
  if (c.resources) {
    const struct {
      uint32_t value;
      const char* word;
    } resources[] = {{c.resources->combo_points, "combo points"},
                     {c.resources->soul_shards, "soul shards"},
                     {c.resources->ammo, "ammo"}};
    for (const auto& resource : resources) {
      if (resource.value != 0) {
        bits.push_back(base::StrCat(
            {resource.word, " ", base::NumberToString(resource.value)}));
      }
    }
  }
  if (!c.summary.empty()) {
    bits.push_back(c.summary);
  }
  return base::JoinString(bits, "; ");
}

// One party member: who (unless around me), role and state, where (unless
// around me).
std::string MemberLine(const pb::Unit& u, bool around_me, bool leader) {
  std::vector<std::string> bits;
  if (!around_me) {
    if (std::string who = Who(u); !who.empty()) {
      bits.push_back(std::move(who));
    }
  }
  if (leader) {
    bits.push_back("leader");
  }
  if (u.group) {
    const std::string role = NameOf(u.group->role);
    if (!role.empty()) {
      bits.push_back(Word(role));
    }
    if (u.group->main_tank) {
      bits.push_back("main tank");
    }
    if (u.group->main_assist) {
      bits.push_back("main assist");
    }
    if (!u.group->online) {
      bits.push_back("offline");
    } else if (!u.group->in_range) {
      bits.push_back("out of range");
    }
    if (u.group->aggro) {
      bits.push_back("has aggro");
    }
  }
  if (u.dead) {
    bits.push_back("dead");
  }
  AddPercent(&bits, u.health_pct, "health");
  AddPercent(&bits, u.power_pct, "power");
  if (!u.target.empty()) {
    bits.push_back(base::StrCat({"target ", u.target}));
  }
  return NamedLine(u.name, base::JoinString(bits, ", "),
                   around_me ? "" : UnitWhere(u));
}

// "party: leader you; loot group loot" and a line per member.
std::vector<std::string> PartyLines(
    const pb::Party& p,
    const std::flat_set<std::string>& guids,
    std::string_view my_name,
    const std::optional<pb::GroupPart>& self_group,
    bool self_is_leader,
    const std::vector<pb::Unit>& nearby_units,
    bool in_combat) {
  std::vector<std::string> header;
  bool leader_listed = false;
  for (const pb::Unit& u : p.members) {
    leader_listed |= (!p.leader.empty() && u.name == p.leader) ||
                     (u.group && u.group->leader);
  }
  if (!leader_listed) {
    if (self_is_leader) {
      header.push_back("leader you");
    } else if (!p.leader.empty()) {
      header.push_back(base::StrCat(
          {"leader ", p.leader == my_name ? std::string("you") : p.leader}));
    }
  }
  if (!p.loot_method.empty()) {
    header.push_back(base::StrCat({"loot ", Word(p.loot_method)}));
  }
  if (!p.missing_buffs_i_can_give.empty()) {
    header.push_back(
        base::StrCat({"missing my buffs ",
                      base::JoinString(p.missing_buffs_i_can_give, ", ")}));
  }
  std::vector<std::string> lines;
  if (header.empty() && p.members.empty()) {
    return lines;
  }
  lines.push_back(
      header.empty()
          ? "party:"
          : base::StrCat({"party: ", base::JoinString(header, "; ")}));
  for (const pb::Unit& u : p.members) {
    const bool leader = (!p.leader.empty() && u.name == p.leader) ||
                        (u.group && u.group->leader);
    lines.push_back(
        base::StrCat({"  - ", MemberLine(u, IsAroundMe(u, guids), leader)}));
  }

  // Factual hurt summary (A5): who is below 100% HP (rounded) with pure
  // distance + LoS facts (no hardcoded heal-range threshold).
  std::vector<const pb::Unit*> hurt;
  for (const pb::Unit& u : p.members) {
    if (!u.dead && u.group && u.group->online && u.health_pct &&
        std::isfinite(*u.health_pct) && std::lround(*u.health_pct) < 100) {
      hurt.push_back(&u);
    }
  }
  if (!hurt.empty()) {
    std::sort(hurt.begin(), hurt.end(),
              [](const pb::Unit* a, const pb::Unit* b) {
                if (*a->health_pct != *b->health_pct) {
                  return *a->health_pct < *b->health_pct;
                }
                return a->name < b->name;
              });
    std::vector<std::string> parts;
    for (const pb::Unit* u : hurt) {
      std::string info;
      if (!u->group->in_range) {
        info = base::StringPrintf("%s %.0f%% (out of range)", u->name.c_str(),
                                  *u->health_pct);
      } else {
        const float dist = Finite(u->rel.dist_yd);
        info = base::StringPrintf("%s %.0f%% (%.0f yd", u->name.c_str(),
                                  *u->health_pct, dist);
        if (u->rel.in_los && !*u->rel.in_los) {
          info += ", out of sight";
        }
        info += ")";
      }
      parts.push_back(std::move(info));
    }
    lines.push_back(
        base::StrCat({"  party hurt: ", base::JoinString(parts, ", ")}));
  }

  // Factual loose-mob summary (A5): only when at least one designated tank
  // exists in the group (including `self` when marked tank), flag living
  // hostile or pulled neutral mobs targeting a non-tank party member.
  if (in_combat) {
    std::flat_set<std::string> tank_names;
    std::flat_set<std::string> member_names;
    if (self_group && (self_group->main_tank || self_group->role == pb::TANK) &&
        !my_name.empty()) {
      tank_names.insert(std::string(my_name));
    }
    for (const pb::Unit& u : p.members) {
      member_names.insert(u.name);
      if (u.group && (u.group->main_tank || u.group->role == pb::TANK)) {
        tank_names.insert(u.name);
      }
    }
    if (!tank_names.empty()) {
      std::vector<std::string> loose;
      for (const pb::Unit& u : nearby_units) {
        if (u.dead ||
            (u.reaction != pb::Unit::HOSTILE &&
             u.reaction != pb::Unit::NEUTRAL) ||
            u.target.empty()) {
          continue;
        }
        const bool target_is_party = member_names.contains(u.target) ||
                                     (!my_name.empty() && u.target == my_name);
        if (!target_is_party || tank_names.contains(u.target)) {
          continue;
        }
        loose.push_back(base::StringPrintf("%s (targeting %s, %.0f yd)",
                                           u.name.c_str(), u.target.c_str(),
                                           Finite(u.rel.dist_yd)));
      }
      if (!loose.empty()) {
        lines.push_back(
            base::StrCat({"  loose mobs: ", base::JoinString(loose, ", ")}));
      }
    }
  }
  return lines;
}

// " 3/8" for an objective's progress, " (done)" for a finished one without.
std::string ProgressText(const pb::Objective& o) {
  if (o.progress && o.progress->need != 0) {
    return base::StrCat({" ", base::NumberToString(o.progress->have), "/",
                         base::NumberToString(o.progress->need)});
  }
  return o.done ? " (done)" : "";
}

// The quest tracker: a line per quest ("Kobold Camp Cleanup (level 2,
// complete)") with its objectives under it ("- Kobold Vermin slain 3/8");
// the bot's own goals after them. The module sends one Objective per quest
// objective, each with the quest's QuestPart.
std::vector<std::string> QuestLines(const std::vector<pb::Objective>& tracker) {
  struct Quest {
    pb::QuestPart part;
    std::vector<const pb::Objective*> objectives;
  };
  std::vector<Quest> quests;
  std::vector<const pb::Objective*> goals;
  for (const pb::Objective& o : tracker) {
    if (!o.quest) {
      goals.push_back(&o);
      continue;
    }
    const pb::QuestPart& part = *o.quest;
    Quest* quest = nullptr;
    for (Quest& q : quests) {
      if (part.quest_id != 0
              ? q.part.quest_id == part.quest_id
              : q.part.quest_id == 0 && q.part.quest == part.quest) {
        quest = &q;
        break;
      }
    }
    if (!quest) {
      quest = &quests.emplace_back(Quest{part, {}});
    }
    quest->part.complete |= part.complete;
    quest->part.failed |= part.failed;
    quest->part.daily |= part.daily;
    if (quest->part.level == 0) {
      quest->part.level = part.level;
    }
    if (quest->part.timer_s == 0) {
      quest->part.timer_s = part.timer_s;
    }
    if (!(o.title.empty() || o.title == part.quest) || o.progress) {
      quest->objectives.push_back(&o);
    }
  }

  std::vector<std::string> lines;
  if (!quests.empty()) {
    lines.push_back("quests:");
  }
  for (const Quest& q : quests) {
    std::vector<std::string> about;
    if (q.part.level != 0) {
      about.push_back(
          base::StrCat({"level ", base::NumberToString(q.part.level)}));
    }
    if (q.part.complete) {
      about.push_back("complete");
    }
    if (q.part.failed) {
      about.push_back("failed");
    }
    if (q.part.daily) {
      about.push_back("daily");
    }
    if (Finite(q.part.timer_s) > 0) {
      about.push_back(base::StringPrintf("%.0f s left", q.part.timer_s));
    }
    std::string line =
        base::StrCat({"  ", q.part.quest.empty() ? "?" : q.part.quest});
    if (!about.empty()) {
      base::StrAppend(&line, {" (", base::JoinString(about, ", "), ")"});
    }
    lines.push_back(std::move(line));
    for (const pb::Objective* o : q.objectives) {
      std::string text = base::StrCat({o->title, ProgressText(*o)});
      lines.push_back(
          base::StrCat({"    - ", base::TrimWhitespaceASCII(text)}));
    }
  }
  if (!goals.empty()) {
    lines.push_back("goals:");
  }
  for (const pb::Objective* o : goals) {
    lines.push_back(base::StrCat(
        {"  - ", o->title.empty() ? "?" : o->title, ProgressText(*o)}));
  }
  return lines;
}

std::string ItemWords(const pb::Item& item) {
  std::string out = item.name;
  if (item.count > 1) {
    base::StrAppend(&out, {" x", base::NumberToString(item.count)});
  }
  std::vector<std::string> bits;
  if (item.quality == pb::Item::POOR) {
    bits.push_back("gray");
  } else if (item.quality == pb::Item::RARE_QUALITY) {
    bits.push_back("rare");
  } else if (item.quality >= pb::Item::UNCOMMON) {
    bits.push_back(Word(pb::EnumName(item.quality)));
  }
  if (item.kind != pb::Item::KIND_UNSPECIFIED && item.kind != pb::Item::OTHER &&
      item.kind != pb::Item::JUNK) {
    bits.push_back(Word(pb::EnumName(item.kind)));
  }
  if (item.usable) {
    bits.push_back("usable");
  }
  if (Finite(item.cooldown_s) > 0) {
    bits.push_back(base::StringPrintf("cooldown %.0f s", item.cooldown_s));
  }
  if (item.charges > 0) {
    bits.push_back(
        base::StrCat({base::NumberToString(item.charges), " charges"}));
  }
  if (item.starts_quest) {
    bits.push_back("starts a quest");
  }
  if (!bits.empty()) {
    base::StrAppend(&out, {" (", base::JoinString(bits, ", "), ")"});
  }
  return out;
}

std::string ItemNames(const std::vector<pb::Item>& items) {
  std::vector<std::string> names;
  for (const pb::Item& item : items) {
    names.push_back(
        item.count > 1
            ? base::StrCat({item.name, " x", base::NumberToString(item.count)})
            : item.name);
  }
  return base::JoinString(names, ", ");
}

std::string Money(uint32_t copper) {
  std::vector<std::string> parts;
  if (copper >= 10000) {
    parts.push_back(base::StrCat({base::NumberToString(copper / 10000), "g"}));
  }
  if (copper % 10000 >= 100) {
    parts.push_back(
        base::StrCat({base::NumberToString(copper % 10000 / 100), "s"}));
  }
  if (copper % 100 != 0 || parts.empty()) {
    parts.push_back(base::StrCat({base::NumberToString(copper % 100), "c"}));
  }
  return base::JoinString(parts, " ");
}

std::vector<std::string> BagsLines(const pb::Bags& bags) {
  std::vector<std::string> head = {
      base::StringPrintf("%u of %u free", bags.free_slots, bags.total_slots)};
  if (bags.hearthstone) {
    head.push_back(base::StrCat(
        {"hearthstone ", bags.hearthstone->ready ? "ready" : "cooling down",
         bags.hearthstone->bind.empty()
             ? ""
             : base::StrCat({", home ", bags.hearthstone->bind})}));
  }
  std::vector<std::string> items;
  for (const pb::Item& item : bags.items) {
    items.push_back(ItemWords(item));
  }
  std::vector<std::string> lines = {
      base::StrCat({"bags (", base::JoinString(head, "; "), "):"})};
  if (!items.empty()) {
    lines.push_back(base::StrCat({"  ", base::JoinString(items, ", ")}));
  }
  return lines;
}

std::vector<std::string> CharacterLines(const pb::Character& c) {
  std::vector<std::string> worn;
  for (const pb::Item& item : c.equipped) {
    std::string text = item.name;
    if (item.equipment) {
      text = base::StrCat(
          {Word(pb::EnumName(item.equipment->slot)), " ", item.name});
      if (item.equipment->durability_pct > 0 &&
          item.equipment->durability_pct < 100) {
        base::StrAppend(&text,
                        {base::StringPrintf(" (durability %.0f%%)",
                                            item.equipment->durability_pct)});
      }
    }
    worn.push_back(std::move(text));
  }
  std::vector<std::string> stats;
  for (const pb::StatValue& s : c.stats) {
    const bool pct = std::string_view(pb::EnumName(s.stat)).ends_with("_PCT");
    stats.push_back(base::StrCat({Word(pb::EnumName(s.stat)), " ",
                                  pct ? base::StringPrintf("%.1f%%", s.value)
                                      : base::StringPrintf("%.0f", s.value)}));
  }
  std::vector<std::string> lines = {"character:"};
  if (!worn.empty()) {
    lines.push_back(
        base::StrCat({"  wearing: ", base::JoinString(worn, ", ")}));
  }
  if (!stats.empty()) {
    lines.push_back(base::StrCat({"  stats: ", base::JoinString(stats, ", ")}));
  }
  return lines;
}

std::vector<std::string> QuestLogLines(const pb::QuestLog& log) {
  std::vector<std::string> lines = {"quest log:"};
  for (const pb::QuestLog::Entry& e : log.quests) {
    if (!e.objective || !e.objective->quest) {
      continue;
    }
    const pb::QuestPart& q = *e.objective->quest;
    std::vector<std::string> about;
    if (q.level) {
      about.push_back(base::StrCat({"level ", base::NumberToString(q.level)}));
    }
    if (q.complete) {
      about.push_back("complete");
    }
    if (q.failed) {
      about.push_back("failed");
    }
    std::vector<std::string> parts;
    if (!e.description.empty()) {
      parts.push_back(Capped(e.description, kMaxQuestText));
    }
    if (!q.text.empty()) {
      parts.push_back(Capped(q.text, kMaxQuestText));
    }
    if (!e.rewards.empty()) {
      parts.push_back(base::StrCat({"rewards ", ItemNames(e.rewards)}));
    }
    if (!e.choices.empty()) {
      parts.push_back(base::StrCat({"choose one of ", ItemNames(e.choices)}));
    }
    if (e.money > 0) {
      parts.push_back(base::StrCat({"money ", Money(e.money)}));
    }
    std::string line = base::StrCat({"  ", q.quest});
    if (!about.empty()) {
      base::StrAppend(&line, {" (", base::JoinString(about, ", "), ")"});
    }
    if (!parts.empty()) {
      base::StrAppend(&line, {": ", base::JoinString(parts, "; ")});
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::string TooltipLine(const pb::Tooltipped& t) {
  std::string name = t.ref ? t.ref->name : "";
  std::vector<std::string> lines;
  if (t.tooltip) {
    lines = t.tooltip->lines;
  }
  if (!lines.empty() && (name.empty() || lines.front() == name)) {
    name = lines.front();
    lines.erase(lines.begin());
  }
  if (t.item && t.item->equipment && !t.compare.empty()) {
    lines.push_back(base::StrCat(
        {t.item->equipment->upgrade ? "an upgrade over " : "not better than ",
         t.compare.front().name, " (worn)"}));
  } else if (t.item && t.item->equipment && t.item->equipment->upgrade) {
    lines.push_back("an upgrade (that slot is empty)");
  }
  return base::StrCat({"pointing at ", name, ": ",
                       Capped(base::JoinString(lines, "; "), kMaxTooltip)});
}

void AddHoverable(std::vector<Hoverable>* out,
                  const std::string& name,
                  pb::HoverRef::Kind kind,
                  const std::string& id) {
  if (name.empty() || id.empty() || id == "0") {
    return;
  }
  for (const Hoverable& h : *out) {
    if (h.name == name) {
      return;
    }
  }
  pb::HoverRef ref;
  ref.kind = kind;
  ref.id = id;
  ref.name = name;
  out->push_back({name, std::move(ref)});
}

std::vector<Hoverable> Hoverables(const pb::Snapshot& snapshot) {
  std::vector<Hoverable> out;
  const pb::Extensions& ext = snapshot.ext;
  for (const pb::Objective& o : ext.quest_tracker) {
    if (o.quest) {
      AddHoverable(&out, o.quest->quest, pb::HoverRef::QUEST,
                   base::NumberToString(o.quest->quest_id));
    }
  }
  const auto items = [&out](const std::vector<pb::Item>& list) {
    for (const pb::Item& item : list) {
      AddHoverable(&out, item.name, pb::HoverRef::ITEM,
                   base::NumberToString(item.id));
    }
  };
  if (ext.bags) {
    items(ext.bags->items);
  }
  if (ext.character) {
    items(ext.character->equipped);
  }
  if (ext.quest_log) {
    for (const pb::QuestLog::Entry& e : ext.quest_log->quests) {
      if (e.objective && e.objective->quest) {
        AddHoverable(&out, e.objective->quest->quest, pb::HoverRef::QUEST,
                     base::NumberToString(e.objective->quest->quest_id));
      }
      items(e.rewards);
      items(e.choices);
    }
  }
  return out;
}

}  // namespace

std::string_view PanelName(pb::Panel panel) {
  switch (panel) {
    case pb::BAGS:
      return "bags";
    case pb::CHARACTER:
      return "character";
    case pb::QUEST_LOG:
      return "quest log";
    default:
      return "";
  }
}

pb::Panel PanelFromName(std::string_view name) {
  for (const pb::Panel panel : {pb::BAGS, pb::CHARACTER, pb::QUEST_LOG}) {
    if (PanelName(panel) == name) {
      return panel;
    }
  }
  return pb::PANEL_UNSPECIFIED;
}

std::vector<std::string> PanelLines(const pb::Snapshot& snapshot,
                                    pb::Panel panel) {
  const pb::Extensions& ext = snapshot.ext;
  switch (panel) {
    case pb::BAGS:
      return ext.bags ? BagsLines(*ext.bags) : std::vector<std::string>{};
    case pb::CHARACTER:
      return ext.character ? CharacterLines(*ext.character)
                           : std::vector<std::string>{};
    case pb::QUEST_LOG:
      return ext.quest_log ? QuestLogLines(*ext.quest_log)
                           : std::vector<std::string>{};
    default:
      return {};
  }
}

std::string_view OptionKindName(OptionKind kind) {
  switch (kind) {
    case OptionKind::kAttack:
      return "attack";
    case OptionKind::kTalk:
      return "talk";
    case OptionKind::kLoot:
      return "loot";
    case OptionKind::kGather:
      return "gather";
    case OptionKind::kUse:
      return "use";
    case OptionKind::kKeep:
      return "keep doing what I'm doing";
    case OptionKind::kExplore:
      return "explore";
    case OptionKind::kRest:
      return "rest";
    case OptionKind::kTown:
      return "go to town";
    case OptionKind::kInvite:
      return "invite";
    case OptionKind::kAcceptInvite:
      return "accept invite";
    case OptionKind::kDeclineInvite:
      return "decline invite";
    case OptionKind::kLeaveGroup:
      return "leave group";
  }
  return "?";
}

Screen RenderScreen(const pb::Snapshot& snapshot) {
  Screen screen;
  screen.has_module_options = !snapshot.options.empty();
  std::flat_map<std::string, int, std::less<>> guid_to_option;
  std::vector<bool> placed;
  if (screen.has_module_options) {
    placed.assign(snapshot.options.size(), false);
    for (size_t i = 0; i < snapshot.options.size(); ++i) {
      ScreenOption parsed = FromModuleOption(snapshot.options[i]);
      screen.options.push_back(parsed.kind);
      screen.details.push_back(parsed);
      if (!parsed.guid.empty()) {
        guid_to_option.try_emplace(parsed.guid, static_cast<int>(i + 1));
      }
    }
  }
  const auto lookup_module_option = [&](std::string_view guid) {
    if (guid.empty()) {
      return 0;
    }
    const auto it = guid_to_option.find(guid);
    return it == guid_to_option.end() ? 0 : it->second;
  };

  const pb::Base& base = snapshot.base;
  const bool self_is_leader = IsSelfPartyLeader(snapshot);
  std::vector<std::string> lines;
  if (base.self != pb::Self{}) {
    lines.push_back("me: " + SelfLine(base.self, self_is_leader));
  }
  if (base.where != pb::Where{}) {
    lines.push_back("where: " + WhereLine(base.where));
  }
  const pb::Nearby& nearby = base.nearby;
  // Units written under "around me": the frames below don't repeat them.
  std::flat_set<std::string> around_me;
  for (const pb::Unit& u : nearby.units) {
    if (!u.name.empty() && !u.guid.empty()) {
      around_me.insert(u.guid);
    }
  }
  // The frames a player watches first (snapshot.md §4): target, focus, combat.
  const pb::Extensions& ext = snapshot.ext;
  std::string_view threat_on_target;
  if (ext.target) {
    if (ext.target->target) {
      const pb::Unit& t = *ext.target->target;
      lines.push_back(base::StrCat(
          {"target: ", TargetLine(*ext.target, t, IsAroundMe(t, around_me),
                                  snapshot.bot)}));
      if (Finite(ext.target->my_threat_pct) > 0) {
        threat_on_target = t.name;
      }
    }
    if (ext.target->focus) {
      const pb::Unit& f = *ext.target->focus;
      lines.push_back(
          base::StrCat({"focus: ", FrameLine(f, IsAroundMe(f, around_me))}));
    }
  }
  if (ext.combat) {
    if (std::string combat =
            CombatLine(*ext.combat, snapshot.bot, threat_on_target);
        !combat.empty()) {
      lines.push_back(base::StrCat({"combat: ", combat}));
    }
  }
  std::vector<Thing> named;
  std::vector<const pb::Unit*> nameless;
  for (const pb::Unit& u : nearby.units) {
    if (u.name.empty()) {
      nameless.push_back(&u);
    } else {
      const int mod_opt = lookup_module_option(u.guid);
      const std::optional<OptionKind> opt =
          mod_opt > 0 ? std::optional(screen.options[mod_opt - 1])
                      : UnitOption(u);
      named.push_back({u.guid, u.name, UnitLook(u), u.rel, opt, mod_opt});
    }
  }
  if (!named.empty()) {
    lines.push_back("around me:");
    for (const std::string& line :
         ThingLines(named, screen.has_module_options, &screen.options,
                    &screen.details, &placed)) {
      lines.push_back("  " + line);
    }
  }
  if (!nameless.empty()) {
    lines.push_back("far away: " + FarSummary(nameless));
  }
  if (!nearby.objects.empty()) {
    std::vector<Thing> objects;
    for (const pb::GameObject& go : nearby.objects) {
      const int mod_opt = lookup_module_option(go.guid);
      const std::optional<OptionKind> opt =
          mod_opt > 0 ? std::optional(screen.options[mod_opt - 1])
                      : ObjectOption(go);
      objects.push_back(
          {go.guid, go.name, ObjectLook(go), go.rel, opt, mod_opt});
    }
    lines.push_back("objects:");
    for (const std::string& line :
         ThingLines(objects, screen.has_module_options, &screen.options,
                    &screen.details, &placed)) {
      lines.push_back("  " + line);
    }
  }
  if (!nearby.areas.empty()) {
    std::vector<const pb::Area*> sorted_areas;
    sorted_areas.reserve(nearby.areas.size());
    for (const pb::Area& a : nearby.areas) {
      if (!a.name.empty() || (a.spell && !a.spell->name.empty())) {
        sorted_areas.push_back(&a);
      }
    }
    std::stable_sort(
        sorted_areas.begin(), sorted_areas.end(),
        [](const pb::Area* a, const pb::Area* b) {
          const float da = (a->rel && std::isfinite(a->rel->dist_yd))
                               ? a->rel->dist_yd
                               : std::numeric_limits<float>::infinity();
          const float db = (b->rel && std::isfinite(b->rel->dist_yd))
                               ? b->rel->dist_yd
                               : std::numeric_limits<float>::infinity();
          return da < db;
        });
    if (!sorted_areas.empty()) {
      lines.push_back("areas:");
      const size_t limit = std::min<size_t>(sorted_areas.size(), 6);
      for (size_t i = 0; i < limit; ++i) {
        const pb::Area& a = *sorted_areas[i];
        const std::string& aname = !a.name.empty() ? a.name : a.spell->name;
        std::vector<std::string> parts;
        parts.push_back(a.hostile ? "hostile" : "friendly");
        if (a.rel) {
          const std::string where = Where(*a.rel);
          if (!where.empty()) {
            parts.push_back(where);
          }
        }
        if (a.lands_in_s && std::isfinite(*a.lands_in_s) &&
            *a.lands_in_s > 0.0f) {
          parts.push_back(base::StringPrintf("lands in %.1fs", *a.lands_in_s));
        }
        if (a.inside_me) {
          parts.push_back("inside");
        }
        lines.push_back("  " + aname + " (" + base::JoinString(parts, ", ") +
                        ")");
      }
    }
  }
  if (ext.party) {
    const std::string_view my_name =
        !snapshot.bot.empty() ? snapshot.bot : base.self.unit.name;
    for (std::string& line :
         PartyLines(*ext.party, around_me, my_name, base.self.unit.group,
                    self_is_leader, nearby.units, base.self.in_combat)) {
      lines.push_back(std::move(line));
    }
  }
  for (std::string& line : QuestLines(ext.quest_tracker)) {
    lines.push_back(std::move(line));
  }
  // Open panels and the tooltip of what the bot points at
  // (docs/specs/options.md): last, after every option ("also:"), so
  // long panels never push an option out of the model's sight.
  std::vector<std::string> looked_at;
  for (const pb::Panel panel : base.ui.open_panels) {
    for (std::string& line : PanelLines(snapshot, panel)) {
      looked_at.push_back(std::move(line));
    }
  }
  for (const pb::Tooltipped& t : base.tooltips) {
    looked_at.push_back(TooltipLine(t));
  }
  screen.hoverables = Hoverables(snapshot);
  screen.open_panels = snapshot.base.ui.open_panels;
  for (const pb::Tooltipped& t : snapshot.base.tooltips) {
    if (t.ref) {
      screen.shown_tooltips.push_back(t.ref->name);
    }
  }
  if (screen.has_module_options) {
    std::vector<std::string> extra;
    for (size_t i = 0; i < snapshot.options.size(); ++i) {
      if (!placed[i]) {
        extra.push_back(base::StrCat({"  ", base::NumberToString(i + 1), ". ",
                                      snapshot.options[i].text}));
      }
    }
    if (!extra.empty()) {
      lines.push_back("also:");
      lines.insert(lines.end(), extra.begin(), extra.end());
    }
  } else {
    lines.push_back("also:");
    const struct {
      OptionKind kind;
      const char* text;
    } always[] = {{OptionKind::kKeep, "Keep doing what I'm doing"},
                  {OptionKind::kExplore, "Explore somewhere new"},
                  {OptionKind::kRest, "Rest (eat and drink)"},
                  {OptionKind::kTown, "Go to the nearest town"}};
    for (const auto& option : always) {
      screen.options.push_back(option.kind);
      screen.details.push_back({option.kind, "", ""});
      lines.push_back(
          base::StrCat({"  ", base::NumberToString(screen.options.size()), ". ",
                        option.text}));
    }
  }
  lines.insert(lines.end(), std::make_move_iterator(looked_at.begin()),
               std::make_move_iterator(looked_at.end()));
  // Every line is one line: a name or subtitle from the game with a line
  // break in it must not write a fake line (an option the bot doesn't have).
  for (std::string& line : lines) {
    for (char& c : line) {
      if (c == '\n' || c == '\r') {
        c = ' ';
      }
    }
  }
  screen.text = base::JoinString(lines, "\n");
  return screen;
}

}  // namespace harness
