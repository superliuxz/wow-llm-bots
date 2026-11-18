// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The hand-written half of the grammar IDL (docs/plans/grammar-idl.md): what
// the code generated from a `.wlb` file (tools/grammar/wlb_gen.py) calls. A
// generated sequence is a static table of `Node`s; this file matches a text
// against it (backtracking over where each field can end) and turns the tables
// into GBNF for llama-server, dropping what an empty name list makes unusable.

#ifndef HARNESS_GRAMMAR_WLB_RUNTIME_H_
#define HARNESS_GRAMMAR_WLB_RUNTIME_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace harness::grammar::rt {

// Whether a whole span is a value of a type (the generated Parse<T> with no
// output).
using ValidFn = bool (*)(std::string_view);

// One element of a sequence.
struct Node {
  enum class Kind {
    kLit,        // `"text"`: must be there.
    kField,      // `name:Type`: a span that `valid` accepts; captured as `id`.
    kFlag,       // `(name:"text")?`: the text, or nothing; captured if there.
    kOpt,        // `( body )?`: the body, or nothing.
    kList,       // `list<Type, "sep", min..max>`: items captured as `id`.
    kParseOnly,  // `~"text"`: accepted if there, never printed or offered.
  };
  Kind kind = Kind::kLit;
  std::string_view text;    // kLit, kFlag, kParseOnly; kList: the separator.
  ValidFn valid = nullptr;  // kField, kList: the item's validator.
  std::string_view rule;    // kField, kList: the item's GBNF rule.
  int id = -1;              // kField, kFlag, kList: capture id.
  std::vector<Node> body;   // kOpt.
  int min = 1;              // kList.
  int max = 1;              // kList.
};
using Seq = std::vector<Node>;

// What the generated tables are written with.
inline Node Lit(std::string_view text) {
  Node n;
  n.kind = Node::Kind::kLit;
  n.text = text;
  return n;
}
inline Node ParseOnly(std::string_view text) {
  Node n;
  n.kind = Node::Kind::kParseOnly;
  n.text = text;
  return n;
}
inline Node Flag(std::string_view text, int id) {
  Node n;
  n.kind = Node::Kind::kFlag;
  n.text = text;
  n.id = id;
  return n;
}
inline Node Field(ValidFn valid, std::string_view rule, int id) {
  Node n;
  n.kind = Node::Kind::kField;
  n.valid = valid;
  n.rule = rule;
  n.id = id;
  return n;
}
inline Node List(ValidFn valid,
                 std::string_view rule,
                 std::string_view separator,
                 int id,
                 int min,
                 int max) {
  Node n;
  n.kind = Node::Kind::kList;
  n.valid = valid;
  n.rule = rule;
  n.text = separator;
  n.id = id;
  n.min = min;
  n.max = max;
  return n;
}
inline Node Opt(std::vector<Node> body) {
  Node n;
  n.kind = Node::Kind::kOpt;
  n.body = std::move(body);
  return n;
}

// A field's (or flag's, or list item's) span on the path that matched.
struct Capture {
  int id;
  std::string_view span;
};

// Matches all of `s` against `seq`: each field tries its shortest span first
// whose rest also matches (so "Fire on the ground on me" splits on the last
// " on " that works), an optional tries "present" before "absent". On success,
// `captures` holds the matched path in order.
bool MatchSeq(const Seq& seq,
              std::string_view s,
              std::vector<Capture>* captures);

// GBNF ----------------------------------------------------------------------

// One rule: alternatives of sequences, or a fixed GBNF body (a number), or the
// names of an env list.
struct Rule {
  std::string_view name;
  std::vector<Seq> alts;
  std::string_view raw;  // When set, the rule's whole GBNF body.
  std::string_view env;  // When set, the rule is the names of this env list.
};

// The env lists one call passes in, by name.
using Env = std::vector<std::pair<std::string_view, std::vector<std::string>>>;

// The GBNF for `root` and every rule it uses. A rule whose env list is empty
// (or whose every alternative needs one) is unusable: alternatives and
// optionals that need it are dropped, and if `root` itself is unusable the
// result is "". Names that aren't names (IsName) are left out.
std::string EmitGbnf(const std::vector<const Rule*>& rules,
                     std::string_view root,
                     const Env& env);

// A GBNF string literal.
std::string GbnfLiteral(std::string_view text);

// Values --------------------------------------------------------------------

// A name as a player sees it: starts with a letter, digit or apostrophe, then
// those, spaces, colons, commas or hyphens; at least one ASCII letter ("10" is
// a number, not a name); no trailing space.
bool IsName(std::string_view s);

// Digits only, in [lo, hi].
bool ParseIntIn(std::string_view s, int lo, int hi, int* out);
// Digits with at most `decimals` places after a '.', in [lo, hi].
bool ParseNumIn(std::string_view s,
                double lo,
                double hi,
                int decimals,
                double* out);
// "12", or "2.5": no trailing zeros, as the reply writes it.
std::string FormatNum(double value);

// Case-insensitive equality (words match in any case when parsing).
bool EqualsIgnoreCase(std::string_view a, std::string_view b);

}  // namespace harness::grammar::rt

#endif  // HARNESS_GRAMMAR_WLB_RUNTIME_H_
