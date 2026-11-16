// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen_renderer.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "proto/snapshot.pb.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

// Facing and "behind it" only matter within melee range.
constexpr float kMeleeYd = 10;

// "THREAT_AGGRO" -> "threat aggro".
std::string Word(absl::string_view name) {
  std::string out(name);
  for (char& c : out) {
    c = c == '_' ? ' ' : absl::ascii_tolower(c);
  }
  return out;
}

// The enum value's name, or "" when it is unset (zero).
template <typename Enum>
std::string NameOf(Enum value, absl::string_view (*name_fn)(Enum)) {
  return value == static_cast<Enum>(0) ? "" : std::string(name_fn(value));
}

// Values that are the normal case and so left out of the text.
bool IsDefaultName(absl::string_view name) {
  return name == "IDLE" || name == "LEVEL" || name == "MEDIUM" ||
         name == "THREAT_NONE" || name == "NORMAL" || name == "NAMEPLATE";
}

// "key yes" for a set flag.
void AddFlag(std::vector<std::string>* bits, bool value, const char* key) {
  if (value) {
    bits->push_back(absl::StrCat(key, " yes"));
  }
}

// "key name" for a set, non-default enum value.
void AddEnum(std::vector<std::string>* bits,
             const std::string& name,
             const char* key) {
  if (!name.empty() && !IsDefaultName(name)) {
    bits->push_back(absl::StrCat(key, " ", Word(name)));
  }
}

// "health 24%" for a unit below full health (0 means not shown).
void AddPercent(std::vector<std::string>* bits, float pct, const char* key) {
  if (pct != 0 && pct < 100) {
    bits->push_back(absl::StrFormat("%s %.0f%%", key, pct));
  }
}

// Where something is: "28 yd SW, below, out of sight, you're behind it".
std::string Where(const pb::Relative& rel) {
  const std::string dir = NameOf(rel.dir(), pb::Direction_Name);
  std::vector<std::string> parts;
  if (dir == "HERE" || (rel.dist_yd() == 0 && dir.empty())) {
    parts.push_back("here");
  } else if (rel.dist_yd() != 0) {
    parts.push_back(absl::StrFormat("%.0f yd %s", rel.dist_yd(), dir));
  } else {
    parts.push_back(dir);
  }
  if (rel.height() == pb::Relative::ABOVE ||
      rel.height() == pb::Relative::BELOW) {
    parts.push_back(Word(pb::Relative_Height_Name(rel.height())));
  }
  if (!rel.in_los()) {
    parts.push_back("out of sight");
  }
  if (rel.behind_it()) {
    parts.push_back("you're behind it");
  }
  if (rel.facing_me()) {
    parts.push_back("facing you");
  }
  return absl::StrJoin(parts, ", ");
}

std::string TightWhere(const pb::Relative& rel_in) {
  pb::Relative rel = rel_in;
  if (rel.dist_yd() > kMeleeYd) {
    rel.clear_behind_it();
    rel.clear_facing_me();
  }
  return Where(rel);
}

// A unit's pack: "(id 1234, size 3)".
std::string PackText(const pb::Unit::Pack& pack) {
  std::vector<std::string> parts;
  if (pack.id() != 0) {
    parts.push_back(absl::StrCat("id ", pack.id()));
  }
  if (pack.size() != 0) {
    parts.push_back(absl::StrCat("size ", pack.size()));
  }
  return absl::StrCat("(", absl::StrJoin(parts, ", "), ")");
}

// Everything about a unit except its name and position, in the order of the
// Python prototype: who it is, how it looks, how hurt, what it does, then the
// rest of its fields by field number.
std::string UnitLook(const pb::Unit& u) {
  std::vector<std::string> bits;
  std::vector<std::string> who;
  if (!u.looks_like().empty()) {
    who.push_back(std::string(u.looks_like()));
  }
  if (!u.player().class_().empty()) {
    who.push_back(absl::AsciiStrToLower(u.player().class_()));
  }
  if (u.level() != 0 || !who.empty()) {
    std::vector<std::string> parts;
    if (u.level() != 0) {
      parts.push_back(absl::StrCat("level ", u.level()));
    }
    parts.insert(parts.end(), who.begin(), who.end());
    bits.push_back(absl::StrJoin(parts, " "));
  }
  for (const std::string& name :
       {NameOf(u.level_color(), pb::Unit_LevelColor_Name),
        NameOf(u.kind(), pb::Unit_Kind_Name),
        NameOf(u.reaction(), pb::Unit_Reaction_Name),
        NameOf(u.classification(), pb::Unit_Classification_Name),
        NameOf(u.size(), pb::Unit_Size_Name)}) {
    if (!name.empty() && !IsDefaultName(name)) {
      bits.push_back(Word(name));
    }
  }
  AddPercent(&bits, u.health_pct(), "health");
  AddPercent(&bits, u.power_pct(), "power");
  const std::string state =
      NameOf(u.activity().state(), pb::Activity_State_Name);
  if (!state.empty() && !IsDefaultName(state)) {
    bits.push_back(Word(state) +
                   (u.activity().with().empty()
                        ? ""
                        : absl::StrCat(" ", u.activity().with())));
  }
  // `dead` is already in the activity.
  AddFlag(&bits, u.lootable(), "lootable");
  AddFlag(&bits, u.skinnable(), "skinnable");
  AddFlag(&bits, u.tapped_by_other(), "tapped by other");
  AddFlag(&bits, u.targeting_me(), "targeting me");
  if (!u.target().empty()) {
    bits.push_back(absl::StrCat("target ", u.target()));
  }
  AddEnum(&bits, NameOf(u.threat_status(), pb::Unit_ThreatStatus_Name),
          "threat status");
  AddEnum(&bits, NameOf(u.quest_marker(), pb::Unit_QuestMarker_Name),
          "quest marker");
  AddFlag(&bits, u.pvp_flagged(), "pvp flagged");
  AddEnum(&bits, NameOf(u.raid_marker(), pb::RaidMarker_Name), "raid marker");
  if (u.has_pack() && (u.pack().id() != 0 || u.pack().size() != 0)) {
    bits.push_back(absl::StrCat("pack ", PackText(u.pack())));
  }
  if (u.visual_state_size() > 0) {
    std::vector<std::string> states;
    for (int i = 0; i < u.visual_state_size(); ++i) {
      states.push_back(Word(pb::Unit_VisualState_Name(u.visual_state(i))));
    }
    bits.push_back(
        absl::StrCat("visual state [", absl::StrJoin(states, "; "), "]"));
  }
  if (!u.quest_relevant().empty()) {
    bits.push_back(absl::StrCat("quest relevant ", u.quest_relevant()));
  }
  if (!u.subtitle().empty()) {
    bits.push_back(absl::StrCat("subtitle ", u.subtitle()));
  }
  return absl::StrJoin(bits, ", ");
}

// "herb, state active": the usual state of an object (usable, ready) is left
// out.
std::string ObjectLook(const pb::GameObject& go) {
  std::vector<std::string> bits;
  const std::string kind = NameOf(go.kind(), pb::GameObject_Kind_Name);
  if (!kind.empty() && !IsDefaultName(kind)) {
    bits.push_back(Word(kind));
  }
  AddPercent(&bits, go.health_pct(), "health");
  // A usable object in the ready state is the normal case; written only when
  // it is the first thing said (the Python prototype's string replace left
  // those in).
  if (go.usable() && bits.empty()) {
    bits.push_back("usable yes");
  }
  if (!go.state().empty() && !(go.state() == "ready" && !bits.empty())) {
    bits.push_back(absl::StrCat("state ", go.state()));
  }
  AddFlag(&bits, go.highlighted(), "highlighted");
  if (!go.owner().empty()) {
    bits.push_back(absl::StrCat("owner ", go.owner()));
  }
  if (go.capture_progress_pct() != 0) {
    bits.push_back(
        absl::StrFormat("capture progress pct %g", go.capture_progress_pct()));
  }
  return absl::StrJoin(bits, ", ");
}

// What the bot can do with a unit or object.
struct Thing {
  std::string name;
  std::string look;
  pb::Relative rel;
  std::optional<OptionKind> option;
};

std::optional<OptionKind> UnitOption(const pb::Unit& u) {
  if (u.kind() != pb::Unit::NPC && u.kind() != pb::Unit::CRITTER) {
    return std::nullopt;
  }
  if (u.dead()) {
    return u.lootable() ? std::optional(OptionKind::kLoot) : std::nullopt;
  }
  switch (u.reaction()) {
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
  if (!go.usable()) {
    return std::nullopt;
  }
  return go.kind() == pb::GameObject::HERB || go.kind() == pb::GameObject::ORE
             ? OptionKind::kGather
             : OptionKind::kUse;
}

// One line per distinct (name, look): the same thing seen several times is
// one line with a count and the range of distances. Entries the bot can act
// on get the next option number.
std::vector<std::string> ThingLines(const std::vector<Thing>& things,
                                    std::vector<OptionKind>* options) {
  std::vector<std::pair<std::string, std::string>> order;
  std::map<std::pair<std::string, std::string>, std::vector<const Thing*>>
      groups;
  for (const Thing& t : things) {
    const auto key = std::make_pair(t.name.empty() ? "?" : t.name, t.look);
    if (groups.find(key) == groups.end()) {
      order.push_back(key);
    }
    groups[key].push_back(&t);
  }
  std::vector<std::string> lines;
  for (const auto& key : order) {
    const std::vector<const Thing*>& group = groups[key];
    std::string mark = "-";
    if (group[0]->option) {
      options->push_back(*group[0]->option);
      mark = absl::StrCat(options->size(), ".");
    }
    if (group.size() == 1) {
      lines.push_back(absl::StrCat(mark, " ", key.first, ": ", key.second, "; ",
                                   TightWhere(group[0]->rel)));
      continue;
    }
    std::vector<float> dists;
    std::set<std::string> dirs;
    for (const Thing* t : group) {
      dists.push_back(t->rel.dist_yd());
      dirs.insert(NameOf(t->rel.dir(), pb::Direction_Name));
    }
    std::sort(dists.begin(), dists.end());
    lines.push_back(absl::StrFormat(
        "%s %dx %s: %s; %.0f-%.0f yd %s", mark, group.size(), key.first,
        key.second, dists.front(), dists.back(), absl::StrJoin(dirs, "/")));
  }
  return lines;
}

// Nameless far units (a model is all a player sees): counts by look, with the
// range of distances and the directions.
std::string FarSummary(const std::vector<const pb::Unit*>& units) {
  std::vector<std::string> order;
  std::map<std::string, std::vector<const pb::Relative*>> groups;
  for (const pb::Unit* u : units) {
    const std::string look = UnitLook(*u);
    if (groups.find(look) == groups.end()) {
      order.push_back(look);
    }
    groups[look].push_back(&u->rel());
  }
  std::vector<std::string> parts;
  for (const std::string& look : order) {
    std::vector<float> dists;
    std::set<std::string> dirs;
    for (const pb::Relative* rel : groups[look]) {
      dists.push_back(rel->dist_yd());
      const std::string dir = NameOf(rel->dir(), pb::Direction_Name);
      if (!dir.empty()) {
        dirs.insert(dir);
      }
    }
    std::sort(dists.begin(), dists.end());
    const std::string span =
        dists.front() == dists.back()
            ? absl::StrFormat("%.0f", dists.front())
            : absl::StrFormat("%.0f-%.0f", dists.front(), dists.back());
    parts.push_back(absl::StrFormat("%dx %s (%s yd %s)", groups[look].size(),
                                    look, span, absl::StrJoin(dirs, "/")));
  }
  return absl::StrJoin(parts, "; ");
}

std::string SelfLine(const pb::Self& s) {
  std::vector<std::string> who;
  who.push_back(s.unit().level() != 0 ? absl::StrCat("level ", s.unit().level())
                                      : "level ?");
  const std::string gender = NameOf(s.gender(), pb::Self_Gender_Name);
  if (!gender.empty()) {
    who.push_back(Word(gender));
  }
  if (!s.unit().player().class_().empty()) {
    who.push_back(absl::AsciiStrToLower(s.unit().player().class_()));
  }
  std::vector<std::string> bits;
  bits.push_back(absl::StrCat(s.unit().name().empty() ? "?" : s.unit().name(),
                              ", ", absl::StrJoin(who, " ")));
  bits.push_back(absl::StrCat("health ", s.health(), "/", s.health_max()));
  if (s.power_max() != 0) {
    const std::string power_type =
        NameOf(s.power_type(), pb::Self_PowerType_Name);
    bits.push_back(absl::StrCat(power_type.empty() ? "power" : Word(power_type),
                                " ", s.power(), "/", s.power_max()));
  }
  if (s.xp_next() != 0) {
    bits.push_back(absl::StrCat(
        "xp ", s.xp(), "/", s.xp_next(),
        s.rested_xp() != 0 ? absl::StrCat(" (rested ", s.rested_xp(), ")")
                           : ""));
  }
  if (s.money() != 0) {
    bits.push_back(absl::StrCat("money ", s.money(), "c"));
  }
  // The life state first when not alive, then the flags.
  const std::string life = NameOf(s.state(), pb::Self_LifeState_Name);
  if (!life.empty() && life != "ALIVE") {
    bits.push_back(Word(life));
  }
  const struct {
    bool value;
    const char* word;
  } flags[] = {{s.in_combat(), "in combat"}, {s.resting(), "resting"},
               {s.mounted(), "mounted"},     {s.swimming(), "swimming"},
               {s.in_flight(), "in flight"}, {s.camping(), "camping"},
               {s.drunk() != 0, "drunk"}};
  for (const auto& flag : flags) {
    if (flag.value) {
      bits.push_back(flag.word);
    }
  }
  return absl::StrJoin(bits, "; ");
}

std::string WhereLine(const pb::Where& w) {
  std::vector<std::string> places;
  if (!w.subzone().empty()) {
    places.push_back(std::string(w.subzone()));
  }
  if (!w.zone().empty()) {
    places.push_back(std::string(w.zone()));
  }
  std::string out = absl::StrJoin(places, ", ");
  if (!w.map().empty()) {
    absl::StrAppend(&out, " (", w.map(), ")");
  }
  if (w.indoors()) {
    absl::StrAppend(&out, "; indoors");
  }
  if (w.in_instance()) {
    absl::StrAppend(&out, "; in an instance");
  }
  return out;
}

}  // namespace

Screen RenderScreen(const pb::Snapshot& snapshot) {
  Screen screen;
  const pb::Base& base = snapshot.base();
  std::vector<std::string> lines;
  if (base.has_self()) {
    lines.push_back("me: " + SelfLine(base.self()));
  }
  if (base.has_where()) {
    lines.push_back("where: " + WhereLine(base.where()));
  }
  const pb::Nearby& nearby = base.nearby();
  std::vector<Thing> named;
  std::vector<const pb::Unit*> nameless;
  for (const pb::Unit& u : nearby.units()) {
    if (u.name().empty()) {
      nameless.push_back(&u);
    } else {
      named.push_back(
          {std::string(u.name()), UnitLook(u), u.rel(), UnitOption(u)});
    }
  }
  if (!named.empty()) {
    lines.push_back("around me:");
    for (const std::string& line : ThingLines(named, &screen.options)) {
      lines.push_back("  " + line);
    }
  }
  if (!nameless.empty()) {
    lines.push_back("far away: " + FarSummary(nameless));
  }
  if (nearby.objects_size() > 0) {
    std::vector<Thing> objects;
    for (const pb::GameObject& go : nearby.objects()) {
      objects.push_back(
          {std::string(go.name()), ObjectLook(go), go.rel(), ObjectOption(go)});
    }
    lines.push_back("objects:");
    for (const std::string& line : ThingLines(objects, &screen.options)) {
      lines.push_back("  " + line);
    }
  }
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
    lines.push_back(
        absl::StrCat("  ", screen.options.size(), ". ", option.text));
  }
  screen.text = absl::StrJoin(lines, "\n");
  return screen;
}

}  // namespace harness
