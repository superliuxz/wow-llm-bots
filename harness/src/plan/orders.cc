// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/orders.h"

#include <algorithm>
#include <cstddef>
#include <flat_set>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "grammar/orders.wlb.h"
#include "grammar/wlb_runtime.h"
#include "snapshot/snapshot.h"

namespace harness {

std::optional<std::vector<Act>> ParseActs(
    std::string_view text,
    const std::vector<std::string>& names) {
  // The steps of an order are only a type inside `Order`: parse them as one.
  std::optional<Order> order = grammar::ParseOrder(
      base::StrCat({"always -> ", text}),
      grammar::rt::Env{.names = {{"order_names", names}}, .counts = {}});
  if (!order) {
    return std::nullopt;
  }
  return std::move(order->steps);
}

std::string OrderGrammar(const std::vector<std::string>& names,
                         const std::vector<std::string>& pingers) {
  // No names, no orders: that is how `order:` lines stay off until the module
  // can carry acts out (docs/specs/triggers.md, step (b)). The generated
  // grammar alone would still allow orders that name nothing ("always ->
  // jump").
  if (std::none_of(names.begin(), names.end(), [](const std::string& name) {
        return grammar::rt::IsName(name);
      })) {
    return "";
  }
  return grammar::OrdersGrammar(
      "Order",
      grammar::rt::Env{.names = {{"order_names", names}, {"pingers", pingers}},
                       .counts = {}});
}

namespace {

namespace pb = wlb::snapshot;

bool IsOrderKeyword(std::string_view s) {
  static constexpr std::string_view kKeywords[] = {
      "me",    "tank",   "offtank", "healer", "leader",    "anyone", "boss",
      "it",    "ally",   "player",  "enemy",  "my target", "group",  "allies",
      "focus", "always", "wake",    "jump",   "ping",
  };
  return std::ranges::find(kKeywords, s) != std::end(kKeywords);
}

bool IsCommonSentenceWord(std::string_view w) {
  static constexpr std::string_view kStop[] = {
      "The",    "A",      "An",     "When",        "If",      "In",   "On",
      "At",     "To",     "From",   "Run",         "Move",    "Use",  "Cast",
      "Stop",   "Target", "Stay",   "Avoid",       "Kite",    "Do",   "Dont",
      "Don't",  "Never",  "Always", "Immediately", "Phase",   "Boss", "Tank",
      "Healer", "Raid",   "Party",  "Player",      "Players",
  };
  return std::ranges::find(kStop, w) != std::end(kStop);
}

bool IsNonGameNoteHeading(std::string_view s) {
  if (IsCommonSentenceWord(s)) {
    return true;
  }
  static constexpr std::string_view kHeadings[] = {
      "Note",
      "Notes",
      "Tip",
      "Tips",
      "Summary",
      "Overview",
      "Mechanics",
      "Strategy",
      "Reminder",
      "Warning",
      "Important",
      "Role",
      "Roles",
      "DPS",
      "Melee",
      "Ranged",
      "Adds",
      "Intermission",
      "Draft",
      "Suggested standing orders",
      "Standing order",
      "Standing orders",
      "Ground hazard",
      "Boss cast AoE",
      "Priority add",
      "Priority interrupt target",
      "Encounter positioning",
  };
  for (std::string_view h : kHeadings) {
    if (base::EqualsCaseInsensitiveASCII(s, h)) {
      return true;
    }
  }
  // Reject non-game names with digits directly attached to a preceding letter
  // (e.g. "Kinetic Bomb1-4"), while preserving hyphenated/spaced real names
  // like "VX-001" or "XT-002 Deconstructor".
  for (size_t i = 1; i < s.size(); ++i) {
    if (base::IsAsciiDigit(s[i]) && base::IsAsciiAlpha(s[i - 1])) {
      return true;
    }
  }
  // Reject structural headings like "Phase 1", "Stage 2", "Step 3", "Group 1".
  static constexpr std::string_view kNumberedPrefixes[] = {
      "Phase ", "Stage ", "Step ", "Part ", "Group ", "Rule ",
  };
  for (std::string_view p : kNumberedPrefixes) {
    if (s.size() > p.size() &&
        base::EqualsCaseInsensitiveASCII(s.substr(0, p.size()), p)) {
      std::string_view rest = s.substr(p.size());
      if (std::all_of(rest.begin(), rest.end(), [](char c) {
            return base::IsAsciiDigit(c) || c == ' ';
          })) {
        return true;
      }
    }
  }
  return false;
}

void ExtractNamesFromNotes(std::string_view notes, const auto& add_name) {
  auto clean_word = [](std::string_view w) {
    return base::TrimString(w, ".,;:!?()[]\"'*`", base::TRIM_ALL);
  };
  auto ends_phrase = [&](std::string_view raw_word, std::string_view cleaned) {
    std::string_view stripped =
        base::TrimString(raw_word, "\"'*`)]", base::TRIM_TRAILING);
    if (stripped.empty()) {
      return true;
    }
    const char last = stripped.back();
    if (last == '.' || last == ',' || last == ';' || last == '!' ||
        last == '?') {
      return true;
    }
    if (last == ':' && cleaned != "Word") {
      return true;
    }
    return false;
  };

  bool in_code_block = false;
  for (std::string_view raw_line : base::SplitStringPiece(
           notes, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (raw_line.starts_with("```")) {
      in_code_block = !in_code_block;
      continue;
    }
    if (in_code_block || raw_line.starts_with("<!--")) {
      continue;
    }
    std::string_view line =
        base::TrimString(raw_line, "-*# \t", base::TRIM_LEADING);
    // 1. Headings before `:` ("The Lich King, Defile: black pool...").
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      for (std::string_view part : base::SplitStringPiece(
               line.substr(0, colon), ",", base::TRIM_WHITESPACE,
               base::SPLIT_WANT_NONEMPTY)) {
        std::string_view cleaned =
            base::TrimString(part, "\"'*`", base::TRIM_ALL);
        if (!IsNonGameNoteHeading(cleaned)) {
          add_name(cleaned);
        }
      }
    }
    // 2. Title-Case multi-word or single capitalized names in the body
    // (e.g. "Ice Block", "Power Word: Shield", "Val'kyr Shadowguard").
    std::vector<std::string_view> words = base::SplitStringPiece(
        line, " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    for (size_t i = 0; i < words.size();) {
      std::string_view w0 = clean_word(words[i]);
      if (w0.empty() || !base::IsAsciiUpper(w0.front())) {
        ++i;
        continue;
      }
      if (w0 == "The" &&
          (ends_phrase(words[i], w0) || i + 1 >= words.size() ||
           clean_word(words[i + 1]).empty() ||
           !base::IsAsciiUpper(clean_word(words[i + 1]).front()))) {
        ++i;
        continue;
      }
      size_t end = i + 1;
      if (!ends_phrase(words[i], w0)) {
        while (end < words.size()) {
          std::string_view wn = clean_word(words[end]);
          if (!wn.empty() && base::IsAsciiUpper(wn.front())) {
            const bool stop = ends_phrase(words[end], wn);
            ++end;
            if (stop) {
              break;
            }
          } else if ((wn == "of" || wn == "the" || wn == "on" || wn == "in" ||
                      wn == "and") &&
                     !ends_phrase(words[end], wn) && end + 1 < words.size()) {
            // Allow 1 or 2 lowercase connectors ("Mark of the Fallen",
            // "Lay on Hands").
            size_t next_cap = end + 1;
            if (next_cap + 1 < words.size() &&
                clean_word(words[next_cap]) == "the" &&
                !ends_phrase(words[next_cap], "the")) {
              ++next_cap;
            }
            std::string_view wc = clean_word(words[next_cap]);
            if (!wc.empty() && base::IsAsciiUpper(wc.front())) {
              const bool stop = ends_phrase(words[next_cap], wc);
              end = next_cap + 1;
              if (stop) {
                break;
              }
            } else {
              break;
            }
          } else {
            break;
          }
        }
      }
      std::string phrase;
      for (size_t k = i; k < end; ++k) {
        std::string_view piece =
            (k + 1 == end)
                ? clean_word(words[k])
                : base::TrimString(words[k], "(,;[.\"'*`", base::TRIM_LEADING);
        piece = base::TrimString(piece, ",;.\"'*`)]", base::TRIM_TRAILING);
        if (!phrase.empty()) {
          phrase.push_back(' ');
        }
        phrase.append(piece);
      }
      if ((end - i >= 2 || !IsCommonSentenceWord(phrase)) &&
          !IsNonGameNoteHeading(phrase)) {
        add_name(phrase);
      }
      i = end;
    }
  }
}

}  // namespace

std::vector<std::string> CollectOrderNames(const pb::Snapshot& snapshot,
                                           std::string_view notes) {
  std::vector<std::string> out;
  std::flat_set<std::string> seen;
  auto add = [&](std::string_view raw) {
    std::string_view trimmed = base::TrimWhitespaceASCII(raw, base::TRIM_ALL);
    if (trimmed.empty() || trimmed.size() > 60 || IsOrderKeyword(trimmed) ||
        !grammar::rt::IsName(trimmed)) {
      return;
    }
    std::string s(trimmed);
    if (seen.insert(s).second) {
      out.push_back(std::move(s));
    }
  };

  // A dead unit's name is no order target, except a party or raid member's
  // (a body to resurrect: `use Resurrection on <name>`).
  auto add_unit = [&](const pb::Unit& u, bool include_unit_name,
                      bool even_dead = false) {
    if (include_unit_name && (even_dead || !u.dead)) {
      add(u.name);
    }
    for (const pb::Aura& a : u.auras) {
      add(a.name);
    }
    if (u.activity.cast && u.activity.cast->spell) {
      add(u.activity.cast->spell->name);
    }
    if (u.pet) {
      for (const pb::Spell& sp : u.pet->abilities) {
        add(sp.name);
      }
    }
    if (u.vehicle) {
      for (const pb::Spell& sp : u.vehicle->abilities) {
        add(sp.name);
      }
    }
  };

  if (!notes.empty()) {
    ExtractNamesFromNotes(notes, add);
  }

  add_unit(snapshot.base.self.unit, /*include_unit_name=*/false);
  if (snapshot.base.self.casting && snapshot.base.self.casting->spell) {
    add(snapshot.base.self.casting->spell->name);
  }
  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if (!u.dead) {
      add_unit(u, /*include_unit_name=*/true);
    }
  }
  for (const pb::GameObject& o : snapshot.base.nearby.objects) {
    add(o.name);
  }
  for (const pb::Area& a : snapshot.base.nearby.areas) {
    add(a.name);
    if (a.spell) {
      add(a.spell->name);
    }
  }
  if (snapshot.ext.encounter) {
    if (snapshot.ext.encounter->boss) {
      add_unit(*snapshot.ext.encounter->boss, /*include_unit_name=*/true);
    }
    for (const pb::Encounter::Timer& t : snapshot.ext.encounter->dbm) {
      add(t.name);
    }
  }
  if (snapshot.ext.target) {
    if (snapshot.ext.target->target) {
      add_unit(*snapshot.ext.target->target, /*include_unit_name=*/true);
    }
    if (snapshot.ext.target->focus) {
      add_unit(*snapshot.ext.target->focus, /*include_unit_name=*/true);
    }
  }
  if (snapshot.ext.party) {
    for (const pb::Unit& m : snapshot.ext.party->members) {
      add_unit(m, /*include_unit_name=*/true, /*even_dead=*/true);
    }
  }
  if (snapshot.ext.raid) {
    for (const pb::Unit& m : snapshot.ext.raid->members) {
      add_unit(m, /*include_unit_name=*/true, /*even_dead=*/true);
    }
  }
  if (snapshot.ext.combat) {
    for (const pb::Unit& a : snapshot.ext.combat->attackers) {
      add_unit(a, /*include_unit_name=*/true);
    }
  }
  if (snapshot.ext.action_bars) {
    for (const pb::Spell& sp : snapshot.ext.action_bars->buttons) {
      add(sp.name);
    }
    for (const pb::Spell& sp : snapshot.ext.action_bars->stance_bar) {
      add(sp.name);
    }
    for (const pb::Spell& sp : snapshot.ext.action_bars->totem_bar) {
      add(sp.name);
    }
  }
  if (snapshot.ext.spellbook) {
    for (const pb::Spell& sp : snapshot.ext.spellbook->spells) {
      add(sp.name);
    }
    for (const pb::Spell& sp : snapshot.ext.spellbook->pet_spells) {
      add(sp.name);
    }
  }
  if (snapshot.ext.bags) {
    for (const pb::Item& it : snapshot.ext.bags->items) {
      add(it.name);
    }
  }
  if (snapshot.ext.character) {
    for (const pb::Item& it : snapshot.ext.character->equipped) {
      add(it.name);
    }
  }
  if (snapshot.ext.vehicle) {
    add_unit(*snapshot.ext.vehicle, /*include_unit_name=*/true);
  }
  if (snapshot.ext.pet && snapshot.ext.pet->pet) {
    add_unit(*snapshot.ext.pet->pet, /*include_unit_name=*/true);
  }
  for (const pb::Option& opt : snapshot.options) {
    if (opt.kind == pb::Option::USE || opt.kind == pb::Option::ATTACK ||
        opt.kind == pb::Option::TALK || opt.kind == pb::Option::GATHER) {
      add(opt.name);
    }
  }
  return out;
}

}  // namespace harness
