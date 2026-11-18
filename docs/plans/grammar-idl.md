# Plan: one grammar file, everything else generated (2026-11-18, agreed; not built yet)

## Goal

Today each of our two reply grammars (the plan, `harness/src/plan.{h,cc}`, and standing orders,
`harness/src/orders.{h,cc}`) is written five times by hand, and the five must agree:

1. the C++ types,
2. the parser,
3. the printer (`FormatPlan`, `FormatOrder`),
4. the per-call GBNF that constrains llama-server (`PlanGrammar`, `OrderGrammar`),
5. the reply form in `content/RULES.md`.

When they drift, the bot breaks silently: `3a28d9d` fixed a GBNF that put `hover:` before
`goal:` while RULES.md taught the opposite order, so once Gemma wrote `goal:` it could never
`hover:`.

**This plan:** each grammar is written once, in a small `.wlb` file, and a generator writes all
five from it. Nothing else changes: the same language, the same C++ API for callers, the same
GBNF the model sees.

**Not in this plan** (each is its own decision, when its executor is built): new verbs or
conditions, sending orders over the socket (decision 1 of `docs/specs/triggers.md`: only the
act goes to the module), option blanks, more panels or chat channels, merging `watch:` into
`when`.

## The language

Eight constructs. Everything is a sequence of literals and typed fields; a field's type says how
it is parsed, printed and constrained.

| Construct | Means | C++ |
|---|---|---|
| `words Who { me, tank, offtank, ... }` | one of these exact words | `enum class Who` |
| `int Look 10..999` | a whole number in range | `int` |
| `num Yards 0..999 1dp` | a number, up to one decimal | `double` |
| `text Goal max 60 no "\n" [from <env>]` | free text (the plan's `goal:` / `say:` / `tell`); the parser UTF-8-truncates over `max` rather than rejecting. With `from <env>` (`hover:`), the parser rejects values over `max` rather than cutting (without checking `<env>` membership) | `std::string` |
| `name Name from <env>` | a name as a player sees it (starts with letter, digit or `'`, then those plus ` :,-`, with at least one ASCII letter so `"SI:7"` fits and `"10"` in `"away 10"` does not: a deliberate narrowing, the hand-written parser accepts `"10"` as a name); in the GBNF, only `<env>` | `std::string` |
| `struct Order = when:When " -> " steps:list<Act, ", then ", 1..3>` | a sequence | `struct Order` |
| `choice Act { To = "to " place:Place; Jump = "jump"; ... }` | one of several forms, tried in order | a struct per arm in `std::variant` |
| `lines Plan { ... }` | a reply: one `key: value` line each, in this order | `struct Plan` |

Inside a sequence: `"literal"`, `field:Type`, `( ... )?` (optional), and
`list<T, "sep", min..max>`. A choice arm without fields is a plain word (`Jump = "jump"`).

**Parse-only spellings, `~`:** `~"<"` in a sequence is a literal the parser accepts if present
and skips if not (trimming optional spaces around it, so `"mana" ~"<" pct:Pct ~"%"` and
`"enemy_player" ~"<" yd:Yards ~"yd"` accept `"mana < 30%"` and `"enemy_player 40yd"`), while the
printer never writes it and the GBNF never offers it; `~"questlog"` after a word in `words` is
another spelling of that word, accepted only when parsing. And `words` match case-insensitively
when parsing (`Bags`). This is how the generated parser keeps today's tolerances for an
unconstrained model (`hp40`, `mana < 30%`, `enemy_player 40yd`, `questlog`) without a
hand-written hook:

```
words Panel { bags, character, "quest log" ~"questlog" }
Hp          = "hp" ~"<" pct:Pct ~"%";
EnemyPlayer = "enemy_player" ~"<" yd:Yards ~"yd";
```

`env` declares what each call passes in: `names` lists (`whisperers`, `hover_names`,
`order_names`) are GBNF-only and the parser doesn't need them, while `count` bounds (`options`)
are passed to the parser too so out-of-range options (`do: 5` when `options` is 4) fail.

**Two escape hatches**, both by name, written in C++ by hand, for what `~` can't say. Every
hook is behaviour the generator can't check, so there are three, each with its own unit test:

- `@normalize(fn)` on a line: run before parsing, for the slips an unconstrained model makes
  (`NormalizeSteps`: strips `[]()` and turns `×` or spaces around `x` into `x`, on `do:` and
  `expect:`).
- `@parse(fn)` / `@print(fn)` on a type or field: a hand-written function instead of the generated
  one (`LookSeconds` for `look: 2m`; `CapitalizedName` for `tell gATDA:`).

## Our grammars in it

`harness/grammar/orders.wlb`: the standing orders of `docs/specs/triggers.md`, as they are.

```
env OrderEnv {
  order_names: names;      // units, objects, auras, spells and items; none: no order lines
}

words Who     { me, tank, offtank, healer, leader, anyone, boss, it }
words Icon    { skull, cross, square, moon, triangle, diamond, circle, star }
words State   { flying, grounded, untargetable, submerged }
words MyState { "in vehicle", mounted, swimming, "nearest to it" }
words Cmp     { "<", ">" }
num   Num   0..999 1dp
int   Count 0..999
int   Digit 0..9
name  Name  from order_names

choice NearestKind { Ally = "ally"; Player = "player"; Enemy = "enemy"; Named = name:Name; }
choice Thing {
  Who      = who:Who;
  MyTarget = "my target";
  Marker   = "marker " icon:Icon;
  Nearest  = "nearest " kind:NearestKind;
  AllyWith = "ally with " mech:Name;
  Group    = "group";
  Allies   = "allies";
  Focus    = "focus";
  Named    = name:Name;
}

choice Check {
  Stacks     = mech:Name " stacks" cmp:Cmp n:Count " on " who:Who;
  Ends       = mech:Name " ends<" s:Num "s on " who:Who;
  CastOn     = mech:Name " cast on " who:Who;
  CastInLast = mech:Name " cast in last " s:Num "s";
  Cast       = mech:Name " cast";
  AppearsIn  = mech:Name " appears in last " s:Num "s";
  Appears    = mech:Name " appears";
  Count      = mech:Name " count" cmp:Cmp n:Count;
  On         = mech:Name " on " who:Who;
  Within     = a:Thing " within " yd:Num (" of " b:Thing)?;
  Beyond     = a:Thing " beyond " yd:Num (" of " b:Thing)?;
  Targets    = a:Thing " targets " b:Thing;
  Hp         = a:Thing " hp" cmp:Cmp pct:Num;
  ManaPoints = "mana_points" cmp:Cmp n:Count;
  Mana       = "mana" cmp:Cmp pct:Num;
  IsMarked   = a:Thing " is marked " icon:Icon;
  Is         = a:Thing " is " state:State;
  InSight    = a:Thing " in sight";
  OutOfSight = a:Thing " out of sight";
  MeBehind   = "me behind " a:Thing;
  MyTargetIs = "my target is " a:Thing;
  MeHas      = "me has " name:Name;
  Me         = "me " state:MyState;
  Phase      = "phase " n:Digit;
  After      = "after " s:Num "s";
  Every      = "every " s:Num "s";
}
struct Condition = (not:"not ")? check:Check;
choice When { Always = "always"; All = conds:list<Condition, " & ", 1..4>; }

choice Place {
  Behind   = "behind " t:Thing;
  InFront  = "in front of " t:Thing;
  MaxRange = "max range of " t:Thing;
  YdFrom   = yd:Num " yd from " t:Thing;
  At       = t:Thing;
}
words Stop { casting, attacking, moving }
words PetCmd { stay, follow, passive, defensive }
choice Act {
  To        = "to " place:Place;
  Away      = "away" (" " t:Thing)? (" " yd:Num)?;
  Avoid     = "avoid " t:Thing;
  Target    = "target " t:Thing;
  Use       = "use " name:Name (" on " t:Thing)?;
  Stop      = "stop" (" " what:Stop)?;
  FaceAway  = "face away " t:Thing;
  Face      = "face " t:Thing;
  Jump      = "jump";
  Cancel    = "cancel " name:Name;
  Mark      = "mark " t:Thing " " icon:Icon;
  Interact  = "interact " t:Thing;
  Board     = "board " t:Thing;
  Leave     = "leave vehicle";
  Equip     = "equip " name:Name;
  PetAttack = "pet attack " t:Thing;
  Pet       = "pet " cmd:PetCmd;
  Kite      = "kite " t:Thing;
  Wake      = "wake";
}
struct Order = when:When " -> " steps:list<Act, ", then ", 1..3>;
```

`harness/grammar/plan.wlb`: the reply, line by line, in the order RULES.md teaches.

```
import "orders.wlb"

env PlanEnv {
  options: count;          // the screen's options: 1..N
  whisperers: names;       // who `tell` may answer
  hover_names: names;      // what `hover:` may name
  order_names: names;      // what `order:` lines may name (none: no order lines)
}

int  Option   1..options
int  Repeat   2..29
int  Look     10..999 @parse(LookSeconds)   // also "2m"
int  Pct      1..99
int  Yards    1..999
text Goal     max 60 no "\n"
text Chat     max 80 no "\n|"
name Player   from whisperers  letters 2..12 @parse(CapitalizedName)
text Hover    max 60 no "\n|" from hover_names
words Panel   { bags, character, "quest log" ~"questlog" }

struct Step  = option:Option ("x" times:Repeat)?;
choice Watch {
  Hp          = "hp" ~"<" pct:Pct ~"%";
  Mana        = "mana" ~"<" pct:Pct ~"%";
  EnemyPlayer = "enemy_player" ~"<" yd:Yards ~"yd";
  Attacked    = "attacked";
  Whisper     = "whisper";
}

lines Plan {
  do:      steps:list<Step, ", ", 1..5>                    @normalize(NormalizeSteps)
           "<option numbers in order; add xN to repeat 2-29 times, e.g. 3x4>"
  expect:  expect:list<Option, ", ", 1..5> default "none"  @normalize(NormalizeSteps)
           "<option numbers of the mobs this plan fights, or none>"
  watch:   watches:list<Watch, ", ", 1..4> default "none"
           "<none, or up to 4 of: hp<N, mana<N, enemy_player<YARDS, attacked, whisper>"
  look:    look_s:Look
           "<integer 60 to 300: a safety net only, since surprises wake you at once>"
  goal?    goal:Goal
           "<optional: what you're working towards, a few words>"
  say?     say:Chat
           "<optional: a line said out loud to the players near you>"
  tell?    "tell " to:Player ": " text:Chat                // a line with its own key shape
           "<optional: a whisper to one player by name; answer a whisper this way>"
  open?    open:list<Panel, ", ", 1..3>
           "<optional: bags, character and/or quest log, to look at them; ...>"
  close?   close:list<Panel, ", ", 1..3>
           "<optional: panels to close>"
  hover?   hover:Hover
           "<optional: the name of an item or quest on your screen, to read its tooltip; ...>"
  order*7  order:Order    // no RULES.md line until step (b) runs orders
}
```

`lines` rules: a required line (`do:`, `look:`) must be there; a `default "none"` line
(`expect:`, `watch:`) is always emitted in GBNF and `FormatPlan`, and defaults to empty if omitted
when parsing; `key?` is optional, `key*7` repeats up to 7 times, and a line with no quoted
description (`order*7` today) is omitted from `RULES.md`. In GBNF the lines come in this order
and only these; the parser accepts any order and any case of the key, treats `"none"` on optional
lines (`goal:`, `open:`, `close:`, `hover:`) as empty, drops an invalid panel item from
`open:`/`close:` or an invalid optional line (the plan stands, as today), and ignores unknown
lines. The quoted text after each line is its RULES.md description.

That is the whole of both grammars: about 150 lines, against about 1,450 of hand-written
parser, printer and GBNF code today (`plan.cc` 505, `orders.cc` 944).

## What is generated

`tools/grammar/wlb_gen.py` (Python stdlib, like `tools/wlb`) writes, into
`out/<variant>/gen/grammar/`:

- **`orders.wlb.h`, `plan.wlb.h`**: the types (a struct per `struct`, a `std::variant` of one
  struct per arm for a `choice`, an `enum class` per `words`), each with `operator==`.
- **`*.wlb.cc`**: `Parse<T>(std::string_view)`, the first-line parser for a `lines` root (backing
  `ParsePlanStepsLine` for early SSE dispatch), `Format<T>(const T&)` for every type, and
  `<Root>Grammar(const Env&)` (the GBNF). The generated code follows our C++ rules (Chromium
  style, `base::` strings, no `<regex>`), is run through clang-format, and passes
  `tools/wlb check includes`.
- **RULES.md's reply form**: the lines between the `"Reply in exactly this form"` paragraph's
  `:\n` and the first `"\n\nExample:"` are generated from `plan.wlb`, leaving the intro sentence's
  semantic groupings as prose (no HTML comments: `prompt.cc` sends RULES.md as the system
  prompt, so markers would reach the model). Lines with no description (`order:` until step (b))
  are left out. `tools/wlb check` fails when the block differs from what `plan.wlb` gives, when
  any documented `?` key is missing from that intro sentence, and **fails just as loudly when
  it can't find the anchors** (someone reworded them), never skips; `tools/wlb check --fix`
  rewrites it. The 3a28d9d class of bug can't happen again.

The harness keeps its own small API on top (`ParsePlan`, `FormatPlan`, `PlanGrammar`, ...), so
callers (`bot_loop.cc`, `decide.cc`, `prompt.cc`) don't change.

## How the parser works

Every rule is matched against a whole span of the line, never a prefix: `Parse<Order>` must
consume the entire text. A sequence with a field followed by a literal (`mech:Name " on "
who:Who`) tries each place the literal occurs in the span and keeps the first one where both
sides parse; a choice tries its arms in the order written. So "Invocation of Blood on me" and
"Fire on the ground on me" both parse, without the hand-written `rfind` rules. The spans are
short (a reply line is under 250 bytes), so this is plain recursive descent, no tables or
memoizing.

## How the GBNF is made

Each type becomes one GBNF rule (prefixed by its file: `orders_Thing`, so the two files never
clash); `words` and `from <env>` types become alternatives of literals, ranges become the same
digit patterns as today. An `env` list that is empty makes its rule unusable, and the generator
drops every alternative and optional that needs it (no `hover:` line without hover names), the
cases that are hand-coded ternaries today. Orders that name nothing (`always -> jump`) stay
usable without names, so "no order names, no `order:` lines" (how orders stay off until step
(b) of `docs/specs/triggers.md`) is an explicit gate in `OrderGrammar`, not pruning.

## Tests

- The existing `PlanTest` and `OrdersTest` suites, against the generated code, plus both audit
  tables (`OrdersTest.*Audit*`). Two mechanical updates for parity:
  `OrdersTest.ParsesTheSpecsExample` reads the new `std::variant` arms instead of flat
  `Check::Kind` / `Act::Verb` fields, and `PlanTest.TheGrammarOnlyAllowsTheBotsOwnOptions` checks
  the generated GBNF without hardcoding the old hand-written rule names.
- Round trips: for random values of every type, `Parse(Format(x)) == x`.
- **The GBNF accepts what the printer writes, required:** once the grammar test no longer pins
  the exact GBNF text, this is the only proof the grammar and the printer agree. Checked with
  llama.cpp's own grammar code linked into a unit test, as llama.cpp's
  `tests/test-grammar-integration.cpp` does (the grammar built with no vocabulary, so no model
  weights, GGML or CUDA, then each character fed through llama.cpp's own accept step), for every
  round-trip value and every audit-table row. The generator can emit any rule as the GBNF's
  root (`Order`, `When`, `Act`, not only the whole plan), and an audit row is checked against a
  grammar built from the names that row uses (parse it, collect its names, build the grammar). Compiling
  `llama-grammar.cpp` as a small test-only static library in `common/CMakeLists.txt` behind a
  thin wrapper function keeps `llama-grammar.h`'s `<map>` and `<regex>` out of
  `clang-include-cleaner` and ensures MSan builds it against our instrumented `libc++`.
  It is llama.cpp's internal API (`src/llama-grammar.h`), not the public one, so a llama.cpp bump
  may break it: the wrapper keeps that in one place. Expect it to need `unicode.cpp` /
  `unicode-data.cpp` and maybe parts of `llama-vocab.cpp` even with no vocabulary. Confirming the
  library links is the **first thing step 1 checks**; if it pulls in more than that, stop and
  decide before going on (`perf brain` and the eval alone are too weak a gate).
- Generator tests: a small `.wlb` with each construct, its output checked in as a golden.
- Worst-case input: an unconstrained model can send 250 bytes of junk (`a on a on a & a on a
  ...`); a unit test parses such lines under a time bound, so trying every split point can't blow
  up (recursion is bounded today: no rule nests `Thing` in `Thing`; the test keeps it so as the
  language grows).

## Steps

1. **The generator and `orders.wlb`:** first, the llama.cpp grammar check links into a unit test
   (see Tests); then generate the order types, parser, printer and GBNF;
   `orders.cc` becomes the thin API over them; `OrdersTest` and the audit tables pass.
2. **`plan.wlb`:** the same for the plan, with the three hand-written hooks (`NormalizeSteps`,
   `LookSeconds`, `CapitalizedName`) and `~` for the other tolerances; `PlanTest`,
   the bot loop tests and the screen goldens pass; `tools/wlb test eval` and `perf brain` show
   no change.
3. **RULES.md generated:** its reply form from `plan.wlb`, checked by `tools/wlb check`.
4. **Delete** the hand-written parser, printer and GBNF code.

Size: the generator about 800-1,200 lines of Python and its runtime helpers about 200 lines of
C++, against the ~1,450 lines they replace; every later grammar change is one edit.

The standing-order evaluator (step (a) of `docs/specs/triggers.md`) waits for step 1, so it is
written against the generated types.

## Progress

- **Step 1 (2026-11-18), done:**
  - `tools/grammar/wlb_gen.py` (Python stdlib) generates `grammar/orders.wlb.{h,cc}` at build
    time (`common/CMakeLists.txt`); the runtime is `harness/grammar/wlb_runtime.{h,cc}`
    (namespace `harness::grammar::rt`: the matcher, GBNF emission with pruning, number and name
    helpers). Choice arms are namespace-scope structs aliased in the choice (`Thing::Who` is
    `ThingWho`), because a nested struct's default member initializers can't be used while the
    outer struct is incomplete. Generated besides the plan: `CollectNames(value, &names)`, the
    names a value uses (what its GBNF needs).
  - The matcher only tries a field's ends where the literal that follows it occurs (or the
    whole span when nothing follows); trying every end made a rejected order cost ~50 ms and the
    junk test never finish. Now 32 forms round-trip in 4 ms, the junk test takes 1 ms.
  - `harness/src/orders.{h,cc}` is the thin API (`using` the generated `ParseOrder`,
    `ParseWhen`, `FormatOrder`; `ParseActs`; `OrderGrammar` with the explicit "no names, no
    orders" gate). The hand-written order parser, printer and GBNF are gone.
  - Tests: `OrdersTest` (incl. "Fire on the ground on me", "away boss 10"), every audit row
    parses, round-trips and is accepted by the generated GBNF (llama.cpp's code), the junk test.
  - Random round trips: the generator also emits `Random<T>(source)` (a seeded
    `rt::RandomSource`, names from a pool the test passes); 2,000 random orders round-trip and
    every tenth is checked against the GBNF.
  - The generator's test: instead of a byte-for-byte golden of the generated C++ (it would break
    with every clang-format version, and CI's differs from ours),
    `harness/grammar/testdata/constructs.wlb` holds one of each construct, including the step-2
    ones (`~` spellings, word aliases, a list that may be empty, flags, nested optionals, two
    name lists), and is compiled into the unit tests (`ConstructsTest`): parsing, printing,
    what the GBNF offers and prunes, random round trips. It found two bugs at once: `~` ate a
    space the next literal needed, and an unused-parameter warning.
  - **Known ambiguity (pinned by `OrdersTest.AUseNameHoldingOnSplitsAtTheFirstOn`):** in
    `use <name> on <thing>`, a name holding " on " ("Fire on the ground") splits at the first
    " on " (the hand-written parser read it the same way). Fix: the parser prefers names the
    call offered (they are known: the GBNF allowed only them), so the parser takes the call's
    names after all; to do with step 2's `env`.

## Decided (2026-11-18)

1. **Files:** `harness/grammar/plan.wlb` and `harness/grammar/orders.wlb` (only the harness
   parses replies; per decision 1 of `docs/specs/triggers.md` only the act crosses the socket,
   so the module never needs the grammar; the generated code may use the harness's C++26).
   Generated into `out/<variant>/gen/grammar/`.
2. **A `choice` is a `std::variant` of one struct per arm:** no unused fields, and `std::visit`
   makes the compiler flag a form that isn't handled (the evaluator handles all of them).
3. **RULES.md's reply form is generated** from `plan.wlb` (line order and each line's
   description, skipping lines with no description) between the `"Reply in exactly this form"`
   `:\n` line and `"\n\nExample:"`; edit `plan.wlb`, not the block; `tools/wlb check` fails when
   it is stale and `--fix` rewrites it.
4. **Parity first:** no language change in this plan.
