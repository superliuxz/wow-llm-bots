// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan.h"

#include <algorithm>
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

namespace harness {
namespace {

std::string_view Trim(std::string_view text) {
  return base::TrimWhitespaceASCII(text);
}

// An integer in [low, high], digits only.
std::optional<int> ParseInt(std::string_view text, int low, int high) {
  int value = 0;
  if (text.empty() || !base::StringToInt(text, &value) || value < low ||
      value > high || text.find_first_not_of("0123456789") != text.npos) {
    return std::nullopt;
  }
  return value;
}

// Brackets and parentheses a model sometimes wraps numbers in.
std::string StripBrackets(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (c != '[' && c != ']' && c != '(' && c != ')') {
      out.push_back(c);
    }
  }
  return out;
}

// "3", "3x4", "3 x 4" or "3×4": option 3, four times.
std::optional<PlanStep> ParseStep(std::string_view token, int option_count) {
  token = Trim(token);
  // The multiplication sign is two bytes in UTF-8.
  size_t cut = token.find('x');
  size_t skip = 1;
  if (cut == token.npos) {
    cut = token.find("\xC3\x97");
    skip = 2;
  }
  PlanStep step;
  const std::optional<int> option =
      ParseInt(Trim(token.substr(0, cut)), 1, option_count);
  if (!option) {
    return std::nullopt;
  }
  step.option = *option;
  if (cut != token.npos) {
    const std::optional<int> times =
        ParseInt(Trim(token.substr(cut + skip)), 2, kMaxRepeat);
    if (!times) {
      return std::nullopt;
    }
    step.times = *times;
  }
  return step;
}

// "hp<40", "hp40", "hp<40%", "enemy_player<40yd", "attacked", "whisper".
std::optional<Watch> ParseWatch(std::string_view token) {
  const std::string text = base::ToLowerASCII(Trim(token));
  if (text == "attacked") {
    return Watch{Watch::Kind::kAttacked, 0};
  }
  if (text == "whisper") {
    return Watch{Watch::Kind::kWhisper, 0};
  }
  const std::pair<std::string_view, Watch::Kind> kinds[] = {
      {"hp", Watch::Kind::kHealthBelow},
      {"mana", Watch::Kind::kManaBelow},
      {"enemy_player", Watch::Kind::kEnemyPlayerWithin}};
  for (const auto& [prefix, kind] : kinds) {
    const std::optional<std::string_view> after_prefix =
        base::RemovePrefix(text, prefix);
    if (!after_prefix) {
      continue;
    }
    std::string_view rest = Trim(*after_prefix);
    if (const auto after_lt = base::RemovePrefix(rest, "<")) {
      rest = Trim(*after_lt);
    }
    if (const auto no_pct = base::RemoveSuffix(rest, "%")) {
      rest = *no_pct;
    } else if (const auto no_yd = base::RemoveSuffix(rest, "yd")) {
      rest = *no_yd;
    }
    const int max = kind == Watch::Kind::kEnemyPlayerWithin ? 999 : 99;
    if (const std::optional<int> value = ParseInt(Trim(rest), 1, max)) {
      return Watch{kind, *value};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

// A comma-separated list of 1..max items, each parsed by `parse`; nullopt if
// any item is invalid.
template <typename T, typename Parse>
std::optional<std::vector<T>> ParseList(std::string_view text,
                                        size_t max,
                                        Parse parse) {
  std::vector<T> items;
  for (std::string_view token : base::SplitStringPiece(
           text, ",", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    std::optional<T> item = parse(token);
    if (!item || items.size() >= max) {
      return std::nullopt;
    }
    items.push_back(std::move(*item));
  }
  if (items.empty()) {
    return std::nullopt;
  }
  return items;
}

// At most `max_bytes` of `text`, never splitting a UTF-8 character.
// A character name, capitalized as the game writes it ("gatda" -> "Gatda"),
// or nullopt if it isn't one (letters only, kMinNameChars..kMaxNameChars).
std::optional<std::string> ParseName(std::string_view text) {
  text = Trim(text);
  if (text.size() < static_cast<size_t>(kMinNameChars) ||
      text.size() > static_cast<size_t>(kMaxNameChars)) {
    return std::nullopt;
  }
  std::string name;
  for (char c : text) {
    if (!base::IsAsciiAlpha(c)) {
      return std::nullopt;
    }
    name.push_back(name.empty() ? base::ToUpperASCII(c)
                                : base::ToLowerASCII(c));
  }
  return name;
}

std::string Cut(std::string_view text, size_t max_bytes) {
  text = Trim(text);
  if (text.size() <= max_bytes) {
    return std::string(text);
  }
  size_t end = max_bytes;
  // Back up over continuation bytes (10xxxxxx) to a character's first byte.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return std::string(Trim(text.substr(0, end)));
}

}  // namespace

std::string PlanGrammar(int option_count,
                        const std::vector<std::string>& tell_names) {
  // A screen always has at least "Keep doing what I'm doing"; an empty
  // alternative would be an invalid grammar (llama-server rejects it).
  option_count = std::max(option_count, 1);
  std::vector<std::string> options;
  for (int i = 1; i <= option_count; ++i) {
    options.push_back(base::StrCat({"\"", base::NumberToString(i), "\""}));
  }
  // Every count 2..kMaxRepeat spelled out, so the grammar follows kMaxRepeat
  // whatever its value.
  std::vector<std::string> counts;
  for (int n = 2; n <= kMaxRepeat; ++n) {
    counts.push_back(base::StrCat({"\"", base::NumberToString(n), "\""}));
  }
  std::vector<std::string> names;
  for (const std::string& name : tell_names) {
    const bool plain = name.size() >= static_cast<size_t>(kMinNameChars) &&
                       name.size() <= static_cast<size_t>(kMaxNameChars) &&
                       std::all_of(name.begin(), name.end(), [](char c) {
                         return base::IsAsciiAlpha(c);
                       });
    if (plain) {
      names.push_back(base::StrCat({"\"", name, "\""}));
    }
  }
  std::string tell_rule;
  if (!names.empty()) {
    tell_rule = base::StrCat(
        {"tell ::= \"\\ntell \" (", base::JoinString(names, " | "),
         ") \": \" [^\\n|]{1,", base::NumberToString(kMaxSayChars), "}\n"});
  }
  return base::StrCat({
      "root ::= \"do: \" steps \"\\nexpect: \" (\"none\" | ids) \"\\nwatch: \" "
      "watches \"\\nlook: \" looknum goal? say?",
      names.empty() ? "" : " tell?",
      "\n"
      "ids ::= opt (\", \" opt){0,",
      base::NumberToString(kMaxExpected - 1),
      "}\n"
      "steps ::= step (\", \" step){0,",
      base::NumberToString(kMaxSteps - 1),
      "}\n"
      "step ::= opt (\"x\" cnt)?\n"
      "cnt ::= ",
      base::JoinString(counts, " | "),
      "\n"
      "opt ::= ",
      base::JoinString(options, " | "),
      "\n"
      "watches ::= \"none\" | watch (\", \" watch){0,",
      base::NumberToString(kMaxWatches - 1),
      "}\n"
      "watch ::= \"hp<\" pct | \"mana<\" pct | \"enemy_player<\" num | "
      "\"attacked\" | \"whisper\"\n"
      "pct ::= [1-9] [0-9]?\n"
      "num ::= [1-9] [0-9]{0,2}\n"
      "looknum ::= [1-9] [0-9] [0-9]?\n"
      "goal ::= \"\\ngoal: \" [^\\n]{1,",
      base::NumberToString(kMaxGoalChars),
      "}\n"
      // Chat lines: no '|' (the client's link and color escape; the module
      // drops it too, chat/chat_text.h).
      "say ::= \"\\nsay: \" [^\\n|]{1,",
      base::NumberToString(kMaxSayChars),
      "}\n",
      tell_rule,
  });
}

std::optional<std::vector<PlanStep>> ParsePlanStepsLine(std::string_view line,
                                                        int option_count) {
  line = Trim(line);
  if (line.find('\n') != line.npos) {
    return std::nullopt;
  }
  const size_t colon = line.find(':');
  if (colon == line.npos ||
      base::ToLowerASCII(Trim(line.substr(0, colon))) != "do") {
    return std::nullopt;
  }
  const std::string value = StripBrackets(Trim(line.substr(colon + 1)));
  return ParseList<PlanStep>(value, kMaxSteps, [&](std::string_view t) {
    return ParseStep(t, option_count);
  });
}

std::optional<Plan> ParsePlan(std::string_view text, int option_count) {
  Plan plan;
  bool have_steps = false;
  bool have_look = false;
  for (std::string_view line : base::SplitStringPiece(
           text, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    const size_t colon = line.find(':');
    if (colon == line.npos) {
      continue;
    }
    const std::string key = base::ToLowerASCII(Trim(line.substr(0, colon)));
    // Free text is kept as written; brackets are only stripped from the
    // numbers a model sometimes wraps in them.
    const std::string_view raw = Trim(line.substr(colon + 1));
    const std::string value = StripBrackets(raw);
    if (key == "goal") {
      plan.goal = Cut(raw, kMaxGoalChars);
      if (base::ToLowerASCII(plan.goal) == "none") {
        plan.goal.clear();  // What FormatPlan writes for no goal.
      }
    } else if (key == "say") {
      plan.say = Cut(raw, kMaxSayChars);
    } else if (key.starts_with("tell ")) {
      // "tell Gatda: text". A name that isn't one is dropped with its line.
      if (std::optional<std::string> name = ParseName(key.substr(5))) {
        if (std::string text = Cut(raw, kMaxSayChars); !text.empty()) {
          plan.tell_to = std::move(*name);
          plan.tell = std::move(text);
        }
      }
    } else if (key == "do") {
      auto steps = ParseList<PlanStep>(
          value, kMaxSteps,
          [&](std::string_view t) { return ParseStep(t, option_count); });
      if (!steps) {
        return std::nullopt;
      }
      plan.steps = std::move(*steps);
      have_steps = true;
    } else if (key == "expect") {
      if (base::ToLowerASCII(Trim(value)) == "none") {
        continue;
      }
      auto expect = ParseList<int>(
          value, kMaxExpected, [&](std::string_view t) -> std::optional<int> {
            const auto step = ParseStep(t, option_count);
            return step && step->times == 1 ? std::optional(step->option)
                                            : std::nullopt;
          });
      if (!expect) {
        return std::nullopt;
      }
      plan.expect = std::move(*expect);
    } else if (key == "watch") {
      if (base::ToLowerASCII(Trim(value)) == "none") {
        continue;
      }
      auto watches = ParseList<Watch>(value, kMaxWatches, ParseWatch);
      if (!watches) {
        return std::nullopt;
      }
      plan.watches = std::move(*watches);
    } else if (key == "look") {
      // "60", "60s", "60 seconds", or "2m" / "2 min" / "2 minutes".
      size_t digits = 0;
      while (digits < value.size() && base::IsAsciiDigit(value[digits])) {
        ++digits;
      }
      const std::string unit =
          base::ToLowerASCII(Trim(std::string_view(value).substr(digits)));
      int scale = 0;
      if (unit.empty() || unit == "s" || unit == "sec" || unit == "secs" ||
          unit == "second" || unit == "seconds") {
        scale = 1;
      } else if (unit == "m" || unit == "min" || unit == "mins" ||
                 unit == "minute" || unit == "minutes") {
        scale = 60;
      } else {
        return std::nullopt;  // Something else: not a time we understand.
      }
      const std::optional<int> count = ParseInt(
          std::string_view(value).substr(0, digits), 1, kMaxLookSeconds);
      if (!count || *count * scale < kMinLookSeconds ||
          *count * scale > kMaxLookSeconds) {
        return std::nullopt;
      }
      plan.look_s = *count * scale;
      have_look = true;
    }
  }
  if (!have_steps || !have_look) {
    return std::nullopt;
  }
  return plan;
}

std::string FormatPlan(const Plan& plan) {
  std::vector<std::string> steps;
  for (const PlanStep& step : plan.steps) {
    steps.push_back(step.times > 1
                        ? base::StrCat({base::NumberToString(step.option), "x",
                                        base::NumberToString(step.times)})
                        : base::NumberToString(step.option));
  }
  std::string watches = "none";
  if (!plan.watches.empty()) {
    std::vector<std::string> items;
    for (const Watch& watch : plan.watches) {
      switch (watch.kind) {
        case Watch::Kind::kHealthBelow:
          items.push_back(
              base::StrCat({"hp<", base::NumberToString(watch.value)}));
          break;
        case Watch::Kind::kManaBelow:
          items.push_back(
              base::StrCat({"mana<", base::NumberToString(watch.value)}));
          break;
        case Watch::Kind::kEnemyPlayerWithin:
          items.push_back(base::StrCat(
              {"enemy_player<", base::NumberToString(watch.value)}));
          break;
        case Watch::Kind::kAttacked:
          items.push_back("attacked");
          break;
        case Watch::Kind::kWhisper:
          items.push_back("whisper");
          break;
      }
    }
    watches = base::JoinString(items, ", ");
  }
  std::vector<std::string> expect_strs;
  for (int opt : plan.expect) {
    expect_strs.push_back(base::NumberToString(opt));
  }
  std::string out = base::StrCat({
      "do: ",
      base::JoinString(steps, ", "),
      "\nexpect: ",
      plan.expect.empty() ? "none" : base::JoinString(expect_strs, ", "),
      "\nwatch: ",
      watches,
      "\nlook: ",
      base::NumberToString(plan.look_s),
  });
  if (!plan.goal.empty()) {
    base::StrAppend(&out, {"\ngoal: ", plan.goal});
  }
  if (!plan.say.empty()) {
    base::StrAppend(&out, {"\nsay: ", plan.say});
  }
  if (!plan.tell.empty()) {
    base::StrAppend(&out, {"\ntell ", plan.tell_to, ": ", plan.tell});
  }
  return out;
}

}  // namespace harness
