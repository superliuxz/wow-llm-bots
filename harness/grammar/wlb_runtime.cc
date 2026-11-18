// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "grammar/wlb_runtime.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"

namespace harness::grammar::rt {
namespace {

// The rest of a sequence still to match: nodes [i, end) of `seq`, then `next`
// (where an optional's body continues after it).
struct Frame {
  const Seq* seq;
  size_t i;
  const Frame* next;
};

bool Run(const Frame* f, std::string_view s, std::vector<Capture>* caps);

// What must come right after a field, to know where it can end: nothing (the
// field runs to the end), a literal (it ends where the literal occurs), or
// anything (every position).
struct Hint {
  enum class Kind { kEnd, kLit, kAny } kind = Hint::Kind::kAny;
  std::string_view lit;
};

Hint NextHint(const Frame* f) {
  while (f != nullptr && f->i == f->seq->size()) {
    f = f->next;
  }
  if (f == nullptr) {
    return {Hint::Kind::kEnd, {}};
  }
  const Node& next = (*f->seq)[f->i];
  if (next.kind == Node::Kind::kLit) {
    return {Hint::Kind::kLit, next.text};
  }
  return {Hint::Kind::kAny, {}};
}

// The ends a field's span may have in `s`, shortest first: only where what
// follows can start (`also`: a second literal that may follow instead, a
// list's separator).
std::vector<size_t> CandidateEnds(std::string_view s,
                                  const Hint& hint,
                                  std::string_view also = {}) {
  std::vector<size_t> ends;
  if (hint.kind == Hint::Kind::kAny) {
    for (size_t end = 1; end <= s.size(); ++end) {
      ends.push_back(end);
    }
    return ends;
  }
  const auto add_occurrences = [&](std::string_view lit) {
    for (size_t at = s.find(lit, 1); at != s.npos; at = s.find(lit, at + 1)) {
      ends.push_back(at);
    }
  };
  if (hint.kind == Hint::Kind::kEnd) {
    if (!s.empty()) {
      ends.push_back(s.size());
    }
  } else {
    add_occurrences(hint.lit);
  }
  if (!also.empty()) {
    add_occurrences(also);
  }
  std::sort(ends.begin(), ends.end());
  ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
  return ends;
}

// Tries `node`'s own match, then the rest. `caps` grows only along the path
// that succeeds: a failed attempt truncates back.
bool RunNode(const Node& node,
             const Frame& rest,
             std::string_view s,
             std::vector<Capture>* caps) {
  const size_t mark = caps->size();
  const auto fail = [&] {
    caps->resize(mark);
    return false;
  };
  switch (node.kind) {
    case Node::Kind::kLit:
      return s.starts_with(node.text) &&
             Run(&rest, s.substr(node.text.size()), caps);
    case Node::Kind::kParseOnly: {
      // Optional spaces around it, as an unconstrained model writes them.
      std::string_view t = base::TrimWhitespaceASCII(s, base::TRIM_LEADING);
      if (t.starts_with(node.text)) {
        std::string_view after = base::TrimWhitespaceASCII(
            t.substr(node.text.size()), base::TRIM_LEADING);
        if (Run(&rest, after, caps)) {
          return true;
        }
        caps->resize(mark);
      }
      return Run(&rest, s, caps) || fail();
    }
    case Node::Kind::kFlag:
      if (s.starts_with(node.text)) {
        caps->push_back({node.id, s.substr(0, node.text.size())});
        if (Run(&rest, s.substr(node.text.size()), caps)) {
          return true;
        }
        caps->resize(mark);
      }
      return Run(&rest, s, caps) || fail();
    case Node::Kind::kOpt: {
      const Frame body{&node.body, 0, &rest};
      if (Run(&body, s, caps)) {
        return true;
      }
      caps->resize(mark);
      return Run(&rest, s, caps) || fail();
    }
    case Node::Kind::kField:
      for (const size_t end : CandidateEnds(s, NextHint(&rest))) {
        const std::string_view span = s.substr(0, end);
        if (!node.valid(span)) {
          continue;
        }
        caps->push_back({node.id, span});
        if (Run(&rest, s.substr(end), caps)) {
          return true;
        }
        caps->resize(mark);
      }
      return false;
    case Node::Kind::kList: {
      // Item, then (separator, item) up to max - 1 times, then the rest.
      const Hint after_list = NextHint(&rest);
      const auto items = [&](auto& self, std::string_view t,
                             int count) -> bool {
        for (const size_t end : CandidateEnds(t, after_list, node.text)) {
          const std::string_view span = t.substr(0, end);
          if (!node.valid(span)) {
            continue;
          }
          const size_t item_mark = caps->size();
          caps->push_back({node.id, span});
          const std::string_view after = t.substr(end);
          if (count + 1 >= node.min && Run(&rest, after, caps)) {
            return true;
          }
          if (count + 1 < node.max && after.starts_with(node.text) &&
              self(self, after.substr(node.text.size()), count + 1)) {
            return true;
          }
          caps->resize(item_mark);
        }
        return false;
      };
      if (node.min == 0 && Run(&rest, s, caps)) {
        return true;
      }
      caps->resize(mark);
      return items(items, s, 0) || fail();
    }
  }
  return false;
}

bool Run(const Frame* f, std::string_view s, std::vector<Capture>* caps) {
  while (f != nullptr && f->i == f->seq->size()) {
    f = f->next;
  }
  if (f == nullptr) {
    return s.empty();
  }
  const Frame rest{f->seq, f->i + 1, f->next};
  return RunNode((*f->seq)[f->i], rest, s, caps);
}

// GBNF ----------------------------------------------------------------------

const Rule* FindRule(const std::vector<const Rule*>& rules,
                     std::string_view name) {
  for (const Rule* rule : rules) {
    if (rule->name == name) {
      return rule;
    }
  }
  return nullptr;
}

const std::vector<std::string>* FindEnv(const Env& env, std::string_view name) {
  for (const auto& [key, names] : env) {
    if (key == name) {
      return &names;
    }
  }
  return nullptr;
}

std::vector<std::string> UsableNames(const std::vector<std::string>* names) {
  std::vector<std::string> out;
  if (names == nullptr) {
    return out;
  }
  for (const std::string& name : *names) {
    if (IsName(name) && name.size() <= 60) {
      out.push_back(name);
    }
  }
  return out;
}

class Emitter {
 public:
  Emitter(const std::vector<const Rule*>& rules, const Env& env)
      : rules_(rules), env_(env) {}

  bool Live(std::string_view name) {
    for (const auto& [rule, live] : memo_) {
      if (rule == name) {
        return live;
      }
    }
    memo_.push_back({name, false});  // A cycle counts as unusable (none yet).
    const Rule* rule = FindRule(rules_, name);
    bool live = false;
    if (rule != nullptr) {
      if (!rule->raw.empty()) {
        live = true;
      } else if (!rule->env.empty()) {
        live = !UsableNames(FindEnv(env_, rule->env)).empty();
      } else {
        for (const Seq& alt : rule->alts) {
          live = live || SeqLive(alt);
        }
      }
    }
    for (auto& [r, l] : memo_) {
      if (r == name) {
        l = live;
      }
    }
    return live;
  }

  bool SeqLive(const Seq& seq) {
    for (const Node& node : seq) {
      if ((node.kind == Node::Kind::kField && !Live(node.rule)) ||
          (node.kind == Node::Kind::kList && node.min > 0 &&
           !Live(node.rule))) {
        return false;
      }
    }
    return true;
  }

  std::string SeqText(const Seq& seq) {
    std::vector<std::string> parts;
    for (const Node& node : seq) {
      switch (node.kind) {
        case Node::Kind::kLit:
          parts.push_back(GbnfLiteral(node.text));
          break;
        case Node::Kind::kParseOnly:
          break;
        case Node::Kind::kFlag:
          parts.push_back(base::StrCat({GbnfLiteral(node.text), "?"}));
          break;
        case Node::Kind::kOpt:
          if (SeqLive(node.body)) {
            parts.push_back(base::StrCat({"(", SeqText(node.body), ")?"}));
          }
          break;
        case Node::Kind::kField:
          parts.push_back(std::string(node.rule));
          Use(node.rule);
          break;
        case Node::Kind::kList: {
          if (!Live(node.rule)) {
            break;  // min == 0, else the sequence wasn't live.
          }
          Use(node.rule);
          std::string list =
              base::StrCat({std::string(node.rule), " (",
                            GbnfLiteral(node.text), " ", std::string(node.rule),
                            "){0,", base::NumberToString(node.max - 1), "}"});
          parts.push_back(node.min == 0 ? base::StrCat({"(", list, ")?"})
                                        : list);
          break;
        }
      }
    }
    return parts.empty() ? "\"\"" : base::JoinString(parts, " ");
  }

  void Use(std::string_view name) {
    for (std::string_view used : pending_) {
      if (used == name) {
        return;
      }
    }
    pending_.push_back(name);
  }

  std::string Emit(std::string_view root) {
    if (!Live(root)) {
      return "";
    }
    Use(root);
    std::string out;
    for (size_t i = 0; i < pending_.size(); ++i) {
      const Rule* rule = FindRule(rules_, pending_[i]);
      std::string body;
      if (!rule->raw.empty()) {
        body = std::string(rule->raw);
      } else if (!rule->env.empty()) {
        std::vector<std::string> alts;
        for (const std::string& name : UsableNames(FindEnv(env_, rule->env))) {
          alts.push_back(GbnfLiteral(name));
        }
        body = base::JoinString(alts, " | ");
      } else {
        std::vector<std::string> alts;
        for (const Seq& alt : rule->alts) {
          if (SeqLive(alt)) {
            alts.push_back(SeqText(alt));
          }
        }
        body = base::JoinString(alts, " | ");
      }
      base::StrAppend(&out, {rule->name, " ::= ", body, "\n"});
    }
    return out;
  }

 private:
  const std::vector<const Rule*>& rules_;
  const Env& env_;
  std::vector<std::pair<std::string_view, bool>> memo_;
  std::vector<std::string_view> pending_;
};

}  // namespace

bool MatchSeq(const Seq& seq,
              std::string_view s,
              std::vector<Capture>* captures) {
  captures->clear();
  const Frame f{&seq, 0, nullptr};
  return Run(&f, s, captures);
}

std::string EmitGbnf(const std::vector<const Rule*>& rules,
                     std::string_view root,
                     const Env& env) {
  return Emitter(rules, env).Emit(root);
}

std::string GbnfLiteral(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

bool IsName(std::string_view s) {
  if (s.empty() || s.back() == ' ') {
    return false;
  }
  const auto word = [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '\'';
  };
  if (!word(s.front())) {
    return false;
  }
  bool letter = false;
  for (char c : s) {
    if (!word(c) && c != ' ' && c != ':' && c != ',' && c != '-') {
      return false;
    }
    letter = letter || base::IsAsciiAlpha(c);
  }
  return letter;
}

bool ParseIntIn(std::string_view s, int lo, int hi, int* out) {
  int value = 0;
  if (s.empty() || s.size() > 9 ||
      s.find_first_not_of("0123456789") != s.npos ||
      !base::StringToInt(s, &value) || value < lo || value > hi) {
    return false;
  }
  if (out != nullptr) {
    *out = value;
  }
  return true;
}

bool ParseNumIn(std::string_view s,
                double lo,
                double hi,
                int decimals,
                double* out) {
  const size_t dot = s.find('.');
  const std::string_view whole = s.substr(0, dot);
  if (whole.empty() || whole.size() > 9 ||
      whole.find_first_not_of("0123456789") != whole.npos) {
    return false;
  }
  if (dot != s.npos) {
    const std::string_view frac = s.substr(dot + 1);
    if (frac.empty() || static_cast<int>(frac.size()) > decimals ||
        frac.find_first_not_of("0123456789") != frac.npos) {
      return false;
    }
  }
  double value = 0;
  if (!base::StringToDouble(s, &value) || value < lo || value > hi) {
    return false;
  }
  if (out != nullptr) {
    *out = value;
  }
  return true;
}

std::string FormatNum(double value) {
  if (value == static_cast<double>(static_cast<long long>(value))) {
    return base::NumberToString(static_cast<long long>(value));
  }
  return base::StringPrintf("%g", value);
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (base::ToLowerASCII(a[i]) != base::ToLowerASCII(b[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace harness::grammar::rt
