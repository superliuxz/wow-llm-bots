# Grammar IDL spec (`.wlb`)

Each reply grammar is defined once in a `.wlb` file (`harness/grammar/orders.wlb` and
`harness/grammar/plan.wlb`), and `tools/grammar/wlb_gen.py` generates all five synchronized
artifacts from it at build time into `out/<variant>/gen/grammar/`:

1. C++ types (`struct`, `enum class`, `std::variant` choice arms),
2. parser (`Parse<T>(text, env)`),
3. printer (`Format<T>(value)`),
4. per-call GBNF emitter (pruning rules whose `env` lists are empty),
5. `content/RULES.md`'s reply-form block (checked by `tools/wlb check rules`; `--fix` rewrites it).

Only the harness parses replies (only the executed act crosses the socket to `server_module`), so
the module never needs the grammar. Callers use the thin C++ facades in `harness/src/plan/plan.h`
and `harness/src/plan/orders.h`.

## Language constructs

| Construct | Meaning | C++ |
|---|---|---|
| `words Who { me, tank, ... }` | Exact keyword set (case-insensitive on parse) | `enum class Who` |
| `int Look 10..999` | Bounded integer (`1..options` for `env` counts) | `int` |
| `num Yards 0..999 1dp` | Bounded number, up to 1 decimal place | `double` |
| `text Goal max 60 no "\n" [from <env>]` | Free text (truncates UTF-8 over `max`, or rejects when `from <env>`) | `std::string` |
| `name Name from <env>` | Player-visible name (contains an ASCII letter; GBNF restricts to `<env>`) | `std::string` |
| `struct Order = ...` | Sequence of literals, fields, optionals `( ... )?`, and `list<T, "sep", min..max>` | `struct` |
| `choice Act { ... }` | Ordered alternatives; each arm is a namespace-scope struct aliased into the choice | `std::variant` |
| `lines Plan { ... }` | Ordered `key: value` reply lines (`key?`, `key*N`, `key <field>:`, `default`, `lenient`, `@needs`) | `struct` |

- **Parse-only spellings (`~`):** `~"<"` or `~"questlog"` is accepted on parse (with optional
  surrounding spaces trimmed) but never emitted by the printer or offered in GBNF.
- **`env`:** declares per-call inputs (`names` lists like `order_names` / `hover_names` /
  `whisperers`, and `count` bounds like `options`). `count` bounds are checked during parsing;
  `names` lists constrain GBNF and guide name-boundary tie-breaking during parsing.
- **Escape hatches (hand-written in `harness/grammar/plan_hooks.{h,cc}`, unit-tested):**
  - `@normalize(fn)` on a line: pre-parse normalization (`NormalizeSteps`, `NormalizeWatches`).
  - `@parse(fn)` / `@print(fn)` on a type or field (`LookSeconds` for `2m`, `CapitalizedName`).

## Parser, GBNF, and `RULES.md` sync

- **Parser (`harness/grammar/wlb_runtime.{h,cc}`, `harness::grammar::rt`):** matches the full span
  of a line via recursive descent. For a field followed by a literal, it only splits at positions
  where that literal occurs; for `name ... from <list>`, it prefers ends that match an offered name
  in `rt::Env` first (resolving names containing `" on "` like `"Fire on the ground"`).
- **GBNF:** every type becomes a file-prefixed GBNF rule (`orders-Thing`, `plan-Plan`). Empty
  `env` lists prune dependent alternatives and optional lines; `OrderGrammar` and `PlanGrammar`
  keep an explicit gate disabling `order:` lines when `order_names` is empty.
- **`RULES.md` sync:** `wlb_gen.py --rules` generates the reply-form lines between
  `"Reply in exactly this form"`'s `:\n` and `"\n\nExample:"` from `plan.wlb`'s line descriptions.
  `tools/wlb check` fails loudly if the block drifts, if any optional `?` key is missing from the
  intro prose, or if either anchor is missing.

## Tests

- **Semantic & audit suites:** `PlanTest`, `OrdersTest`, `PlanHooksTest`, and `ConstructsTest`
  (`harness/grammar/testdata/constructs.wlb`, covering every IDL construct). `OrdersTest.*Audit*`
  parses and round-trips every row of `docs/specs/trigger-conditions.tsv` and `trigger-actions.tsv`.
- **Random round-trips & real llama.cpp GBNF check:** `wlb_gen.py` emits `Random<T>(source)` and
  `CollectNames(value, &names)`. Unit tests verify `Parse(Format(x)) == x` and feed formatted
  outputs character-by-character into llama.cpp's real `llama-grammar.cpp` (linked test-only behind
  `harness/src/test/gbnf_check.h` with no model weights or CUDA).
- **Worst-case junk & generator tests:** bounded-time parsing on 250-byte adversarial inputs, plus
  `tools/grammar/wlb_gen_test.py` (run by `tools/wlb check rules`).
