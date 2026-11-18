// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The hand-written half of the grammar IDL (docs/plans/grammar-idl.md): what
// the code generated from a `.wlb` file (tools/grammar/wlb_gen.py) calls. A
// generated sequence is a static table of `Node`s; this file matches a text
// against it (backtracking over where each field can end) and turns the tables
// into GBNF for llama-server, dropping what an empty name list makes unusable.

#ifndef HARNESS_GRAMMAR_WLB_RUNTIME_H_
#define HARNESS_GRAMMAR_WLB_RUNTIME_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace harness::grammar::rt {

struct Env;

// Whether a whole span is a value of a type (the generated Parse<T> with no
// output). `env`, when not null, is the call's: the names it offered.
using ValidFn = bool (*)(std::string_view, const Env* env);

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
  // kField: the env list of names this field prefers. Where a span could end
  // at more than one place, ends that make it one of the call's names are
  // tried first ("use Fire on the ground on me").
  std::string_view names;
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
inline Node Field(ValidFn valid,
                  std::string_view rule,
                  int id,
                  std::string_view names = {}) {
  Node n;
  n.kind = Node::Kind::kField;
  n.valid = valid;
  n.rule = rule;
  n.id = id;
  n.names = names;
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
// whose rest also matches, an optional tries "present" before "absent"; with
// `env`, a field with `names` tries the call's names first. On success,
// `captures` holds the matched path in order.
bool MatchSeq(const Seq& seq,
              std::string_view s,
              std::vector<Capture>* captures,
              const Env* env = nullptr);

// GBNF ----------------------------------------------------------------------

// One rule: alternatives of sequences, or a fixed GBNF body (a number), or the
// names of an env list.
struct Rule {
  std::string_view name;
  std::vector<Seq> alts;
  std::string_view raw;     // When set, the rule's whole GBNF body.
  std::string_view env;     // When set, the rule is the names of this env list,
  ValidFn valid = nullptr;  // those of them `valid` accepts (default IsName).
  std::string_view count;   // When set, the numbers `count_from`..this count.
  int count_from = 1;
  std::string_view needs;  // When set, unusable while this env list is empty.
};

// What one call passes in: name lists and counts, by name.
struct Env {
  std::vector<std::pair<std::string_view, std::vector<std::string>>> names;
  std::vector<std::pair<std::string_view, int>> counts;
};

// An env count by name (0 if absent).
int CountOf(const Env& env, std::string_view name);

// Whether `value` may be read as a name of env list `list`: with the call's
// env (`env` not null) holding names in that list, only one of them (the
// GBNF allowed only those, and a name the call never offered is a misreading:
// "away Area 52" is not "Area" 52 yd when "Area 52" was offered); otherwise
// any name.
bool IsOffered(const Env* env, std::string_view list, std::string_view value);

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

// Lines of a reply ----------------------------------------------------------

std::string_view Trim(std::string_view s);
// At most `max_bytes` of trimmed `s`, never splitting a UTF-8 character.
std::string Cut(std::string_view s, size_t max_bytes);
// The lines of a reply (empty ones too).
std::vector<std::string_view> SplitLines(std::string_view text);
// "Key: value" -> the trimmed key and value; false without a ':'.
bool SplitLine(std::string_view line,
               std::string_view* key,
               std::string_view* value);
// "tell Gatda" with `word` "tell" -> true and `field` "Gatda" (any case).
bool KeyWithField(std::string_view key,
                  std::string_view word,
                  std::string_view* field);
// A line's list, split at `separator` with the items trimmed ("3, 4" or "3,4").
std::vector<std::string_view> SplitItems(std::string_view value,
                                         std::string_view separator);
// Moves a parsed value into `out`; false if there is none.
template <typename T>
bool Assign(std::optional<T> value, T* out) {
  if (!value) {
    return false;
  }
  *out = std::move(*value);
  return true;
}

// Tests ---------------------------------------------------------------------

// Where the generated Random<T> functions draw from: a small seeded generator
// (the same values on every run) and the names a `name` field may take.
class RandomSource {
 public:
  RandomSource(uint64_t seed, std::vector<std::string> names);
  RandomSource(const RandomSource&) = delete;
  RandomSource& operator=(const RandomSource&) = delete;
  ~RandomSource();

  // In [0, n); n > 0.
  int Below(int n);
  bool Coin() { return Below(2) == 1; }
  const std::string& Name();
  // A number in [lo, hi] with at most `decimals` places (0 or 1 for now).
  double Num(double lo, double hi, int decimals);

 private:
  uint64_t state_;
  std::vector<std::string> names_;
};

}  // namespace harness::grammar::rt

#endif  // HARNESS_GRAMMAR_WLB_RUNTIME_H_
