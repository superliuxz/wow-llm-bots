// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <flat_map>
#include <flat_set>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "proto/snapshot.pb.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

// Facing and "behind it" only matter within melee range.
constexpr float kMeleeYd = 10;

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
std::string NameOf(Enum value, std::string_view (*name_fn)(Enum)) {
  return value == static_cast<Enum>(0) ? "" : std::string(name_fn(value));
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
                bool shown,
                float pct,
                const char* key) {
  if (shown && pct < 100) {
    bits->push_back(base::StringPrintf("%s %.0f%%", key, pct));
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
  const std::string dir = NameOf(rel.dir(), pb::Direction_Name);
  std::vector<std::string> parts;
  if (dir == "HERE" || (rel.dist_yd() == 0 && dir.empty())) {
    parts.push_back("here");
  } else if (rel.dist_yd() != 0) {
    parts.push_back(
        dir.empty() ? base::StringPrintf("%.0f yd", Finite(rel.dist_yd()))
                    : base::StringPrintf("%.0f yd %s", Finite(rel.dist_yd()),
                                         dir.c_str()));
  } else {
    parts.push_back(dir);
  }
  if (rel.height() == pb::Relative::ABOVE ||
      rel.height() == pb::Relative::BELOW) {
    parts.push_back(Word(pb::Relative_Height_Name(rel.height())));
  }
  if (rel.has_in_los() && !rel.in_los()) {  // Unset: not checked yet.
    parts.push_back("out of sight");
  }
  if (rel.behind_it()) {
    parts.push_back("you're behind it");
  }
  if (rel.facing_me()) {
    parts.push_back("facing you");
  }
  return base::JoinString(parts, ", ");
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
    parts.push_back(base::StrCat({"id ", base::NumberToString(pack.id())}));
  }
  if (pack.size() != 0) {
    parts.push_back(base::StrCat({"size ", base::NumberToString(pack.size())}));
  }
  return base::StrCat({"(", base::JoinString(parts, ", "), ")"});
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
    who.push_back(base::ToLowerASCII(u.player().class_()));
  }
  if (u.has_level() || !who.empty()) {
    std::vector<std::string> parts;
    if (u.has_level()) {
      parts.push_back(
          base::StrCat({"level ", base::NumberToString(u.level())}));
    }
    parts.insert(parts.end(), who.begin(), who.end());
    bits.push_back(base::JoinString(parts, " "));
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
  AddPercent(&bits, u.has_health_pct(), u.health_pct(), "health");
  AddPercent(&bits, u.has_power_pct(), u.power_pct(), "power");
  const std::string state =
      NameOf(u.activity().state(), pb::Activity_State_Name);
  if (!state.empty() && !IsDefaultName(state)) {
    bits.push_back(Word(state) +
                   (u.activity().with().empty()
                        ? ""
                        : base::StrCat({" ", u.activity().with()})));
  }
  // `dead` is already in the activity.
  AddFlag(&bits, u.lootable(), "lootable");
  AddFlag(&bits, u.skinnable(), "skinnable");
  AddFlag(&bits, u.tapped_by_other(), "tapped by other");
  AddFlag(&bits, u.targeting_me(), "targeting me");
  if (!u.target().empty()) {
    bits.push_back(base::StrCat({"target ", u.target()}));
  }
  AddEnum(&bits, NameOf(u.threat_status(), pb::Unit_ThreatStatus_Name),
          "threat status");
  AddEnum(&bits, NameOf(u.quest_marker(), pb::Unit_QuestMarker_Name),
          "quest marker");
  AddFlag(&bits, u.pvp_flagged(), "pvp flagged");
  AddEnum(&bits, NameOf(u.raid_marker(), pb::RaidMarker_Name), "raid marker");
  if (u.has_pack() && (u.pack().id() != 0 || u.pack().size() != 0)) {
    bits.push_back(base::StrCat({"pack ", PackText(u.pack())}));
  }
  if (u.visual_state_size() > 0) {
    std::vector<std::string> states;
    for (int i = 0; i < u.visual_state_size(); ++i) {
      states.push_back(Word(pb::Unit_VisualState_Name(u.visual_state(i))));
    }
    bits.push_back(
        base::StrCat({"visual state [", base::JoinString(states, "; "), "]"}));
  }
  if (!u.quest_relevant().empty()) {
    bits.push_back(base::StrCat({"quest relevant ", u.quest_relevant()}));
  }
  if (!u.subtitle().empty()) {
    bits.push_back(base::StrCat({"subtitle ", u.subtitle()}));
  }
  return base::JoinString(bits, ", ");
}

// "herb, state active": the usual state of an object (usable, ready) is left
// out.
std::string ObjectLook(const pb::GameObject& go) {
  std::vector<std::string> bits;
  const std::string kind = NameOf(go.kind(), pb::GameObject_Kind_Name);
  if (!kind.empty() && !IsDefaultName(kind)) {
    bits.push_back(Word(kind));
  }
  AddPercent(&bits, go.has_health_pct(), go.health_pct(), "health");
  // A usable object in the ready state is the normal case; written only when
  // it is the first thing said (the Python prototype's string replace left
  // those in).
  if (go.usable() && bits.empty()) {
    bits.push_back("usable yes");
  }
  if (!go.state().empty() && !(go.state() == "ready" && !bits.empty())) {
    bits.push_back(base::StrCat({"state ", go.state()}));
  }
  AddFlag(&bits, go.highlighted(), "highlighted");
  if (!go.owner().empty()) {
    bits.push_back(base::StrCat({"owner ", go.owner()}));
  }
  if (go.capture_progress_pct() != 0) {
    bits.push_back(base::StringPrintf("capture progress pct %g",
                                      go.capture_progress_pct()));
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
      return "";
  }
  return "";
}

ScreenOption ParseModuleOption(const pb::Option& option) {
  const std::string_view text = base::TrimWhitespaceASCII(option.text());
  const struct {
    std::string_view prefix;
    OptionKind kind;
  } targeted[] = {
      {"Attack ", OptionKind::kAttack}, {"Talk to ", OptionKind::kTalk},
      {"Loot ", OptionKind::kLoot},     {"Gather ", OptionKind::kGather},
      {"Use ", OptionKind::kUse},
  };
  for (const auto& entry : targeted) {
    if (const auto rest = base::RemovePrefix(text, entry.prefix)) {
      return {entry.kind, std::string(option.guid()),
              std::string(base::TrimWhitespaceASCII(*rest))};
    }
  }
  if (base::StartsWith(text, "Explore", base::CompareCase::INSENSITIVE_ASCII)) {
    return {OptionKind::kExplore, std::string(option.guid()), ""};
  }
  if (base::StartsWith(text, "Rest", base::CompareCase::INSENSITIVE_ASCII)) {
    return {OptionKind::kRest, std::string(option.guid()), ""};
  }
  if (base::StartsWith(text, "Go to", base::CompareCase::INSENSITIVE_ASCII)) {
    return {OptionKind::kTown, std::string(option.guid()), ""};
  }
  return {OptionKind::kKeep, std::string(option.guid()), ""};
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
      dists.push_back(Finite(t->rel.dist_yd()));
      const std::string dir = NameOf(t->rel.dir(), pb::Direction_Name);
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
    groups[look].push_back(&u->rel());
  }
  std::vector<std::string> parts;
  for (const std::string& look : order) {
    std::vector<float> dists;
    std::flat_set<std::string> dirs;
    for (const pb::Relative* rel : groups[look]) {
      dists.push_back(Finite(rel->dist_yd()));
      const std::string dir = NameOf(rel->dir(), pb::Direction_Name);
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

std::string SelfLine(const pb::Self& s) {
  std::vector<std::string> who;
  who.push_back(
      s.unit().level() != 0
          ? base::StrCat({"level ", base::NumberToString(s.unit().level())})
          : "level ?");
  const std::string gender = NameOf(s.gender(), pb::Self_Gender_Name);
  if (!gender.empty()) {
    who.push_back(Word(gender));
  }
  if (!s.unit().player().class_().empty()) {
    who.push_back(base::ToLowerASCII(s.unit().player().class_()));
  }
  std::vector<std::string> bits;
  bits.push_back(base::StrCat({s.unit().name().empty() ? "?" : s.unit().name(),
                               ", ", base::JoinString(who, " ")}));
  bits.push_back(base::StrCat({"health ", base::NumberToString(s.health()), "/",
                               base::NumberToString(s.health_max())}));
  if (s.power_max() != 0) {
    const std::string power_type =
        NameOf(s.power_type(), pb::Self_PowerType_Name);
    bits.push_back(
        base::StrCat({power_type.empty() ? "power" : Word(power_type), " ",
                      base::NumberToString(s.power()), "/",
                      base::NumberToString(s.power_max())}));
  }
  if (s.xp_next() != 0) {
    bits.push_back(base::StrCat(
        {"xp ", base::NumberToString(s.xp()), "/",
         base::NumberToString(s.xp_next()),
         s.rested_xp() != 0
             ? base::StrCat(
                   {" (rested ", base::NumberToString(s.rested_xp()), ")"})
             : ""}));
  }
  if (s.money() != 0) {
    bits.push_back(
        base::StrCat({"money ", base::NumberToString(s.money()), "c"}));
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
  return base::JoinString(bits, "; ");
}

std::string WhereLine(const pb::Where& w) {
  std::vector<std::string> places;
  if (!w.subzone().empty()) {
    places.push_back(std::string(w.subzone()));
  }
  if (!w.zone().empty()) {
    places.push_back(std::string(w.zone()));
  }
  std::string out = base::JoinString(places, ", ");
  if (!w.map().empty()) {
    base::StrAppend(&out, {" (", w.map(), ")"});
  }
  if (w.indoors()) {
    base::StrAppend(&out, {"; indoors"});
  }
  if (w.in_instance()) {
    base::StrAppend(&out, {"; in an instance"});
  }
  return out;
}

}  // namespace

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
  }
  return "?";
}

Screen RenderScreen(const pb::Snapshot& snapshot) {
  Screen screen;
  screen.has_module_options = snapshot.options_size() > 0;
  std::flat_map<std::string, int, std::less<>> guid_to_option;
  std::vector<bool> placed;
  if (screen.has_module_options) {
    placed.assign(static_cast<size_t>(snapshot.options_size()), false);
    for (int i = 0; i < snapshot.options_size(); ++i) {
      ScreenOption parsed = ParseModuleOption(snapshot.options(i));
      screen.options.push_back(parsed.kind);
      screen.details.push_back(parsed);
      if (!parsed.guid.empty()) {
        guid_to_option.try_emplace(parsed.guid, i + 1);
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
      const int mod_opt = lookup_module_option(u.guid());
      const std::optional<OptionKind> opt =
          mod_opt > 0 ? std::optional(screen.options[mod_opt - 1])
                      : UnitOption(u);
      named.push_back({std::string(u.guid()), std::string(u.name()),
                       UnitLook(u), u.rel(), opt, mod_opt});
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
  if (nearby.objects_size() > 0) {
    std::vector<Thing> objects;
    for (const pb::GameObject& go : nearby.objects()) {
      const int mod_opt = lookup_module_option(go.guid());
      const std::optional<OptionKind> opt =
          mod_opt > 0 ? std::optional(screen.options[mod_opt - 1])
                      : ObjectOption(go);
      objects.push_back({std::string(go.guid()), std::string(go.name()),
                         ObjectLook(go), go.rel(), opt, mod_opt});
    }
    lines.push_back("objects:");
    for (const std::string& line :
         ThingLines(objects, screen.has_module_options, &screen.options,
                    &screen.details, &placed)) {
      lines.push_back("  " + line);
    }
  }
  if (screen.has_module_options) {
    std::vector<std::string> extra;
    for (int i = 0; i < snapshot.options_size(); ++i) {
      if (!placed[static_cast<size_t>(i)]) {
        extra.push_back(base::StrCat({"  ", base::NumberToString(i + 1), ". ",
                                      snapshot.options(i).text()}));
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
