# Triggers & Standing Orders

The brain does not inspect every snapshot. It emits a **plan** (and optional **standing orders**);
C++ executes the plan at game speed and wakes the brain only when a trigger fires.

## Plan Shape

Grammar source of truth: [harness/grammar/plan.wlb](../../harness/grammar/plan.wlb)
(`docs/specs/grammar-idl.md`) and `content/RULES.md` (checked by `wlb check rules`).

- `do:` — ordered steps referencing numbered `options` (with optional `xN` repeats), dispatched
  as soon as the first line streams in (`FirstLineCallback`).
- `expect:` — option numbers the plan accounts for (suppresses surprise wakes for those targets).
- `watch:` — edge-latched conditions (`hp<N`, `mana<N`, `enemy_player<N`, `attacked`, `whisper`).
- `order:` / `orders: none` — standing orders (`<when> -> <act>`) evaluated locally in C++ on
  every combat snapshot (`harness/src/plan/order_evaluator.{h,cc}`).
- `open:` / `close:` / `hover:` — inspect panels or tooltips on the next wake.
- `look:` — wake timer in seconds (enforced `>= 10` s to prevent polling loops).
- `goal:` / `remember:` / `note:` — optional persistent goal, memory note, or player relationship note.
- `say:` / `tell <Name>:` — optional chat output.

## Wake Sources & Scheduler Priorities

`TriggerEvaluator` (`harness/src/runtime/triggers.{h,cc}`) evaluates each incoming snapshot and
`ImmediateEvent` against the active plan:

| Priority | Trigger class | Examples |
|---|---|---|
| **P0 (urgent)** | Immediate combat/survival & `-> wake` orders | Died, entered combat unexpectedly, lost control, new unexpected attacker, boss standing-order `wake`. Can preempt lower-tier in-flight LLM calls. |
| **P1** | Plan failure / step error | Action refused (`ErrorEvent`), path failed, walk stuck, target gone mid-step. |
| **P2** | Normal plan boundary & popups | Plan finished, `watch:` edge crossed, timed popups (`PopupWatch`: ready check, loot roll, LFG role/proposal, shared quest, rez offer), `look:` timer elapsed. |
| **P3** | Chat & social | Whispers, `at_me` mentions, party/raid chat (rate-limited per sender; whispers/chat never cancel an active boss dodge). |

`Scheduler` (`harness/src/runtime/scheduler.{h,cc}`) bounds in-flight calls to llama-server's
slot count, applies aging so lower priorities cannot starve indefinitely under load, and prefers
soft idle-slot affinity.

## Standing Orders (`<when> -> <act>`)

Standing orders let the LLM encode sub-second boss/combat tactics once (typically on pull or
from `content/notes/<Dungeon>/notes.md`) and have C++ execute them with ~0 ms LLM latency:

- **Conditions (`<when>`):** Casting (`<who> casts <spell>`), aura/debuff/stack thresholds
  (`me has <aura> >= N`), ground hazard (`hazard <spell>` / `in <spell>`), unit appearance or
  HP threshold (`<mob> appears`, `<who> hp < N%`), periodic timer (`every Ns`, minimum `1s` —
  `every 0s` is rejected by grammar & parser), and boolean combinations (`&`, `not`).
- **Acts (`<act>`):** `out of <hazard>`, `away [N yd] [from <who>]`, `to <place> <who>`, `back`,
  `target <who>`, `use <spell/item> [on <who>]`, `stop [casting|attacking|moving]`, `avoid <who>`,
  `face` / `face away`, `jump`, `cancel <aura>`, `mark <who> <icon>`, `interact <obj>`,
  `board` / `leave`, `equip <item>`, `pet <cmd>`, `kite`, `wake`.
- **Execution & safety:** `Command::Act` runs through the unified validate-before-cancel
  `ExecuteAction` path (`server_module/src/options/option_executor.cc` → `act_executor.cc`), so
  an invalid or unresolvable act never cancels an in-flight boss dodge.
- **Grammar regression datasets:** Every `fits` row in
  [docs/specs/trigger-conditions.tsv](trigger-conditions.tsv) and
  [docs/specs/trigger-actions.tsv](trigger-actions.tsv) is parsed and round-tripped by
  `OrdersTest.*Audit*` (`wlb check triggers`).
