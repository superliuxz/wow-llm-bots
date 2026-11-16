# Triggers: what wakes the brain

The brain doesn't look at every snapshot. It answers with a **plan**, C++ carries the plan out at
game speed, and the brain is woken by a **trigger**: something the plan didn't account for, or
something the brain asked to be told about. Measured: one bot's think takes 0.4-1 s, a 50-bot
batch 8-10 s (`docs/phase2-results.md`), so the brain can't be in every step, and doesn't need
to be: neither is a human.

C++ never judges here. It carries out the brain's plan, checks the conditions the brain set,
and applies defaults taken from the game client (what the client itself alerts a player about).

## The plan (the brain's answer)

**Built today vs. designed.** The harness's plan (`harness/src/plan.cc`, `content/RULES.md`)
has `goal`, `do` (option numbers, with counts), `expect`, `watch` (only `hp<N`, `mana<N`, both
percent of maximum, `enemy_player<YARDS`, `attacked`, `whisper`), `look` and `say`. The fields
`until`, `ignore` and `react` and the path-based watches below are the full design, not built
yet; standing orders (below) are proposed.

| Field | What it is |
|---|---|
| `goal` | what the bot is working towards, in its own words ("10 scorpid tails, then turn in") |
| `steps` | what to do, in order: options and commands (`snapshot.md` §9), each optionally `until` a condition ("until 10 tails") |
| `expect` | what the plan intends to deal with, as filters ("Scorpid Worker" mobs): these don't count as surprises |
| `watch` | extra conditions to wake on (below) |
| `ignore` | default triggers not to wake on for this plan ("whispers", "combat feedback from expected mobs") |
| `orders` | proposed: standing orders, `<watch> -> <action>`, run by C++ without waking the brain (below) |
| `react` | the first reaction to a surprise while the brain thinks ("run to the Razor Hill guards"); default: the body's stock reaction (playerbots: fight back if attacked, otherwise keep going) |
| `look_again_s` | wake anyway after this long |

Nothing in a plan is fixed by C++: a careful bot watches a lot, a reckless one very little.

**The brain can only do what a player can.** `steps` and `expect` are numbers from the bot's
own `options` (`snapshot.md` §9), `watch` uses only the vocabulary below, and `look_again_s`
is a number; free text only in a short `goal` and an optional chat line. The harness enforces
this with a grammar built for each call from the bot's current options, so a plan is always
valid and short (~36 tokens). Benched: it tripled the brain's capacity and made every plan
valid (`docs/phase2-results.md`).

## Where triggers come from (complete)

Every event the 3.3.5a client can fire is classified in `client-events.tsv`
(`coverage-client-events.md`): each one is either an Event in the snapshot, or a change in a
snapshot field, or out of scope. So two sources cover everything a player can be told:

1. **Events:** every Event kind in `events` (chat, prompts, errors, died, displaced, ...). The
   module sends each one **immediately** (`ImmediateEvent`), not with the next snapshot.
2. **Changes:** any field of the snapshot. The harness compares each snapshot with the previous
   one (every second; more often in combat).

Plus the plan's own status:

3. **Plan:** done (all steps finished); failed (a step got an `error` or `path_failed`); a
   step's `until` met.
4. **Time:** `look_again_s` passed.

## Watches (the brain's conditions)

A closed vocabulary over the snapshot. Paths are proto field paths, so it grows with the
snapshot and the checker validates them.

| Watch | Fires when | Example |
|---|---|---|
| `event {kind, filter}` | an event of that kind arrives (matching the filter) | a whisper; chat that mentions my name; a raid warning |
| `changed {path}` | the field changes | `base.self.in_combat`; `base.self.control`; `ext.party.members` |
| `crossed {path, below/above, value}` | a number crosses the line | `base.self.unit.health_pct` below 40; `ext.target.target.health_pct` below 20; `base.self.unit.power_pct` below 30 |
| `appears {list, filter}` / `gone {list, filter}` | an entry matching the filter enters or leaves a list | a hostile player in `nearby.units` within 40 yd; a new entry in `combat.attackers`; an aura on me (`self.unit.auras`); a friend online |
| `elapsed {s}` | time passes | |

Filters compare an entry's fields: `{kind: PLAYER, reaction: HOSTILE, level >= 70,
rel.dist_yd < 40}`, `{name: "Hogger"}`, `{dispel_type: MAGIC}`. Units coming into view have no
client event (a player just sees them), which is why `appears` exists.

## Standing orders (proposed 2026-11-16, not built)

**The test for any behaviour is how fast it has to happen.** Under about a second it can't be a
brain call: one bot alone gets a plan in 0.21 s, but an urgent wake cost 0.8 s at the lightest
load measured (2 surprises per bot per minute, 50 bots, 79% GPU) and 2-3 s with a busy GPU
(`docs/phase2-results.md`, "Urgent wakes"; the cost at low load isn't measured). So it is a **reflex** (the playerbots engine:
rotation, interrupts, fighting back, human-paced) or a **standing order**. Anything slower is a
brain decision.

| Layer | Who | Speed | Examples |
|---|---|---|---|
| Reflex | C++ (playerbots engine, our module) | within a tick, plus a human-like delay | rotation, interrupts, fighting back |
| Standing order | the brain writes it, C++ carries it out | written ahead of time, runs within a tick | "hp<30: use option 7", "Defile on me: run to the edge" |
| Decision | the brain | 0.5-2 s | what to do next, who to group with, chat, what to do after a wipe |

An **order** is a small plan started by a watch instead of by the brain:
`<when> -> <act>, then <act>` (grammar below): the verbs act on things the plan's `steps` already
name, and a step may end `until` a condition or a timeout, as in a plan; it generalises today's
single `react` line to a list.

**About ten verbs, and the number must not grow per boss; what grows is what they point at**
(provisional: see "What a pass over playerbots' boss triggers found" below):
`to`, `away`, `target`, `use`, `stop`, `face`, `jump`, `cancel`, `avoid`, `wake`.

| Verb | Covers |
|---|---|
| `to` | stack, run to the tank, hide behind an ice tomb, enter a portal |
| `away` | Defile, spread out, get out of a cloud |
| `target` | switch to adds, a Bone Spike, a Val'kyr |
| `use` | a cooldown, potion, dispel, taunt, interrupt |
| `stop` | stop casting or attacking (Unchained Magic) |
| `avoid` | don't melee this, don't dispel that |
| `face`, `jump`, `cancel` | turn to a unit; jump; cancel an aura or a cast |
| `wake` | no rule fits: ask the brain (what every watch does today) |

Targets and places reuse the filter vocabulary the watches already have (`{name: ...}`, an
aura, a raid marker on a unit, a distance).

**What a first pass over playerbots' boss triggers found** (2026-11-16, against the first
grammar; its table was not kept, and the re-run with the revised grammar below replaces its numbers). Six readers wrote each of the 594 boss-trigger bodies in the
pinned playerbots commit as a `when` clause of the first grammar here, or marked why it couldn't be
said:

| Result | Triggers |
|---|---|
| Fits the first grammar | 188 (32%) |
| No condition at all: "this role does this for the whole fight" | 120 (20%) |
| Can't be said | 286 (48%) |

The verbs were not the weak part; the `when` side was. Why the 286 failed (a trigger can have
several reasons; counts are from a keyword sort of the notes): "not" (an aura is absent, the boss
isn't casting) 145; assignments computed in code and shared between bots 49; a state that isn't
an aura (flying, submerged, untargetable, a phase flag) 48; a fixed location, height or room 45;
a distance other than "within N of me" 28; health above a line or a range 24; vehicle, mount, pet
or gear state 23; "or" of two conditions about 20; a raid marker 15; a repeating or stored timer
15; a count of units 14. Of the 120 whole-fight triggers about 52 are positions and about 24 are
kill priorities: standing orders with no trigger, so the grammar needs `always`.

**The `when` side, revised** (proposed; the verb side is as above):

```
order   ::= when " -> " act (", then " act){0,2}
when    ::= "always" | cond (" & " cond){0,2}
cond    ::= "not "? test
test    ::= mech " on " who | mech " stacks" cmp cnt " on " who
          | mech " cast" | mech " cast on " who | mech " appears" | mech " count>" cnt
          | thing (" within " | " beyond ") num | thing " targets " who
          | thing " hp" cmp pct | "mana" cmp pct | thing " is " state
          | "me " mystate | "phase " digit | "after " num "s" | "every " num "s"
cmp     ::= "<" | ">"
who     ::= "me" | "tank" | "healer" | "anyone" | "boss"
thing   ::= "it" | "boss" | "tank" | mech | "marker " mark
state   ::= "flying" | "grounded" | "untargetable" | "submerged"
mystate ::= "in vehicle" | "mounted" | "swimming" | "nearest to it"
```

Examples the first grammar couldn't say: `always -> to behind boss`; `always -> target marker
skull`; `not Fire Bomb cast & boss hp>20 -> target boss`; `Sindragosa is flying -> to Ice Tomb`;
`Tainted Core appears & me nearest to it -> use 4 on it`.

- **"Or" needs no syntax:** write two orders with the same action.
- **Coverage, measured (2026-11-16):** every one of the 595 `IsActive` bodies in
  `src/Ai/Raid` and `src/Ai/Dungeon` of the pinned playerbots commit (`ecc4a53`; 482 raid, 113
  dungeon) was read, following helpers, and written in the revised grammar or marked why it
  can't be: `docs/specs/trigger-conditions.tsv`, one row each (how it was made:
  `trigger-conditions-brief.md`; six readers in parallel, then 12 random rows re-read against
  the source). `tools/check-trigger-conditions.py` parses every `when` cell against the grammar
  (all pass), so the table is the grammar's regression set. The script is a second copy of the
  grammar: once orders are parsed in `harness/src/plan.cc`, a unit test runs the real parser over
  the table and the Python copy is retired. Leaving out 25 `plumbing` rows (base
  classes, shared timer keepers), 18 `bugnet` rows (resetting state, stuck bots) and 5 `cheat`
  rows (the `*Cheat*` triggers, which only fire with playerbots' raid cheat on), the 547
  mechanics are:

  | Verdict | Triggers |
  |---|---|
  | `fits` (a `when` clause in the revised grammar) | 339 (62%) |
  | `always` (no condition beyond the fight and the bot's role) | 124 (23%) |
  | `unsayable` | 84 (15%) |

  So **85% can be said if every stretch is allowed, 69% without any** (256 clean `fits` plus 124
  `always`). 83 `fits` rows needed a stretch (noted in `missing`); 28 of those assume the bot
  knows its assignment. Separately, 44 `fits` rows use `phase N`, whose numbers assume DBM's
  numbering (playerbots computes its own phases, e.g. Illidan, Kael'thas; not checked against
  DBM). Why the 84 can't be said (a row can have several): `other` 43, `assignment` 21,
  `fixed_location` 17, `timer` 13, `nonaura_state` 10, `vehicle_gear` 8, `count` 4,
  `distance_other` 4, `health_range` 3, `marker` 2.
- **Grammar gaps the re-run found** (each cheap; they cover most of the 43 `other` rows): `me` is not a `thing`, so "my health < 40%"
  can't be said though the snapshot has it; no "master" or assist-tank `who`; no line-of-sight,
  "behind it" or "my current target" test (the snapshot has `in_los` and `behind_it`); no game
  objects as `mech` (traps, Crystal Spike); a named unit among several bosses (council fights)
  can't be told apart by `boss`; some triggers need 4 conditions (cap 3) or 5-7 orders (cap 4),
  e.g. one interrupt order per spell; no aura's time left (`Aura.remaining_s` exists), no
  "anyone within N" for spread checks, no absolute mana amount.
- **Snapshot gaps it found:** a flying/grounded, untargetable and submerged state on `Unit`
  (the most common gap: Razorscale, Sapphiron, Onyxia, Sindragosa, Nightbane, Thaddius, Kael's
  advisors, Lurker); an assist-tank and assist-healer index in `GroupPart`; where a cast will land
  (Arcane Orb, Flame Strike, Malleable Goo); the liquid under the bot (lava).
- **Found in playerbots:** `TooFarFromPlayerWithAuraTrigger::TooFarFromPlayerWithAura`
  (`src/Ai/Base/Trigger/RangeTriggers.cpp:275`) returns before its distance loop, so the range is
  never checked (Yogg-Saron Brain Link); `NotBehindIngvarTrigger` returns true when the bot *is*
  behind its target. Several ICC triggers also change auras as a side effect (strip
  "Experienced", give heroic cheat buffs), and 5 are `*Cheat*` triggers: not comparable with a
  player.
- **`phase N` comes from DBM (decided 2026-11-16).** The snapshot already has it, gated on the
  DBM addon like a player's own: `BossPart.phase`, the encounter's DBM timers and the
  `dbm_warning` event.
- **`marker` as a place** only works on a unit carrying the icon: 3.3.5a has no markers on the ground.
- **Outside the grammar** (first pass: about 84 triggers; re-run: 85, mainly assignments and
  fixed locations). Most of the answer is already in the snapshot; six gaps are
  left (checked against `snapshot.proto`: `main_tank`, `assistant`, `subgroup`, raid markers,
  `LANDMARK`, `swimming` and `visual_state` exist; `lands_in_s` does not):
  1. **Named places in a boss room** (the hard one): playerbots says `(x, y, z)`, a player says "on
     the dais". Cheapest first: a player or unit ("ranged stack on Loada" is `to Loada`, covers
     most cases, nothing new); relative spots (behind the boss, max range, the door you came in
     by, N yards apart: computed from what is sent); room landmarks (no server data known, would
     be per-room scripting, so left until a fight needs it).
  2. **Unit states that aren't auras** (flying, submerged, untargetable): add to `Unit.visual_state`.
  3. **Impact spots before they land** (a cast aimed at a point, with the time left): a flag and
     `lands_in_s` on `Area`.
  4. **Liquid type under the bot** (lava, slime): next to `swimming`.
  5. **Main assist:** the proto has `main_tank` and `assistant`, but no main assist.
  6. **DBM data per boss** (phases, timers, warnings): the fields exist; filling them is where
     per-boss knowledge legitimately lives. DBM's own boss mods are the obvious source to
     convert from (not looked at yet).
  Also: assignments that are settled in raid chat before the pull can only be used if they are in
  the bot's prompt when it writes its orders: the raid chat where it was given, or a line in its
  memory ("I take the left cube"). Playerbots' "the closest free bot does it" (5) is computable in
  the harness from the positions already sent. About 9 triggers are bug nets (fell through the
  floor): not the brain's job, a C++ safety net or the "Stuck" option.

**A sequence is allowed if it stays short and has no branches:** two or three steps, each with
an `until` or a timeout, ending in "resume the plan" or `wake`. Anything of the form "if that
fails, then..." is a brain decision, so a failed step wakes the brain. One order runs at a
time; the brain lists them in priority order and a higher one interrupts a lower one.
Example: `Defile on me -> away, then resume`.

Rules that keep the split clean:

1. **C++ never chooses for the bot.** It carries out an order the brain wrote, or what the
   playerbots reflexes do anyway. If no order covers an event, it wakes the brain (the defaults
   above) and does not guess.
2. **The brain never sits in a timing loop.** If something needs an answer in under about a
   second, it must be an order given in advance.
3. **Knowing unlocks the reaction.** An order for a boss mechanic only exists for a bot that has
   learned that mechanic (`design.md`, known-mechanics gate): a bot that hasn't learned Defile
   has no order for it and reacts late, like a player.
4. **An order covers its event.** A matching order replaces the default wake, and the event goes
   into the next look's "since your last look". An order can also say `wake` to do both: do the
   thing, then ask the brain.
5. **Priority is the order the brain wrote them in** (see above); a new plan replaces all of the
   old plan's orders. At most 4 orders per plan, like today's 4 watches.

Why: it cuts urgent wakes. Every expected surprise ("an add: target it", "health under 30%:
potion") is handled without a brain call, and the measured limit is about 7 wake-ups a second,
shared by every bot. The cost: a longer reply (about 6 tokens per order) and an executor in the
module for each action.

**How the brain learns orders for a mechanic** (two separate things: the verbs, and what a boss does):

- **The verbs:** a short section in `content/RULES.md` with one or two examples; the per-call
  grammar enforces them, as it does for options.
- **What the boss does:** per-bot notes, from the four sources `design.md` names (dying to it,
  being told in chat, looking it up in the local database tool, watching others), for example
  "Lich King, Defile: black pool under whoever is targeted, grows when people stand in it".
- **Gating:** the `when` part of the grammar is built from the bot's own notes, so a bot can only
  write an order about Defile if Defile is in its notes. That enforces "knowing unlocks the
  reaction" in code.
- **The loop:** before a pull, one wake with that boss's notes and the verbs, and the answer is
  the order list; the harness checks that each order names a real spell or aura and a legal
  verb; after a wipe, one wake with what killed the bot, and it updates its notes and orders.
- **Testing:** offline evals: given these notes, does the brain write a sensible order, and
  does a bot without the note write none.
- This keeps "no per-case scripts": C++ knows the verbs, never the bosses.

Still open: a bench of a raid-wide burst (`SURPRISES` wakes bots at independent random times;
an ICC mechanic wakes 25 bots in the same instant: a knob for "N bots at once every M seconds");
the grammar and snapshot gaps above (then re-run the table, the grammar's
regression set); the six snapshot gaps above; how orders are tested live. **Testing, in five steps:** (1) coverage on paper (done:
`trigger-conditions.tsv`, 595 rows, the grammar's regression set); (2) unit tests in the harness for `not`, `&`, `always`
and order priority, on recorded snapshot sequences; (3) offline with the model: boss notes in,
orders out, scored with `harness decide`, and a bot without the note writes none; (4) live
without the model: hand-written orders on a fixture bot, cause the mechanic from the console,
measure event to first action, one scenario per verb and one per `test` form (about 25); (5)
live with the model, boss by boss: orders fired, reaction time, deaths per mechanic, wakes during
the fight, wake to plan for bots outside, against playerbots' stock strategy on the same boss.
Steps 4 and 5 need the `choose` command and level-80 fixtures.

## Defaults (from the client)

When a plan says nothing, a trigger wakes the brain if **the client would get the player's
attention**. The list is derived from the client's own UI code (`client_alert` in
`client-events.tsv`), not chosen by us:

| Default wake | Why (the client's alert) |
|---|---|
| every **prompt** | a popup waits for an answer (and often has a timer) |
| events the client alerts on: whisper (sound), raid warning (raid notice + sound), Dungeon Finder proposal (sound), achievement (alert frame), died (popup), resurrect offer (popup + sound), ... | the event's `client_alert` has a sound, popup, flash, raid notice, alert frame or error line |
| **entered combat** (`base.self.in_combat` changed to true) when the plan `expect`s no fight | the red screen-edge flash (`PLAYER_REGEN_DISABLED`) |
| a new entry in `combat.attackers` that the plan doesn't `expect` | combat feedback on my portrait (`UNIT_COMBAT`): how a player notices being hit; the 3.3.5a UI plays no aggro sound |
| **lost control** (`base.self.control` changed: stunned, feared, charmed, possessed) | the action bars stop responding and the camera or character moves on its own |
| **displaced** (knocked back, pulled, teleported) | the screen jumps (no client event: from the movement opcodes) |
| plan **done** or **failed** | the player would stop and decide what's next |

Everything else (party chat, loot lines, health going down in an expected fight, a mob walking
past) is **noted, not woken for**: it goes into the next look's "since your last look".

The defaults never check health on their own: the 3.3.5a client has no low-health alert (its
screen flash is for entering combat), so "health under X%" is the brain's own watch.

## Where it runs, and how fast

- **Module:** every Event is sent at once; snapshots every `LlmBrain.SnapshotIntervalMs` (1 s; it
  can drop to 250 ms in combat, ~55 µs per build).
- **Harness:** checks triggers on each event and snapshot (microseconds, no LLM). A woken bot
  goes into the brain's queue by urgency: prompts with timers and combat first, then the rest.
- **Latency:** event to woken < 10 ms; a field change within one snapshot interval; then one
  bot's think, 0.4-1 s.
- **While a thought is in flight**, the bot does its `react`, and further triggers are added to
  the same look (never lost); the brain's answer replaces the plan.

## The gank, step by step

The plan: kill Scorpid Workers south of the valley until 10 tails, loot each, then turn in at
Gornek (`expect`: Scorpid Workers).

1. An Alliance rogue opens on the bot. `combat.attackers` gets a player the plan doesn't expect:
   **woken** (default). If the brain had watched `appears {nearby.units, {kind: PLAYER,
   reaction: HOSTILE}}`, it would have been woken earlier, when the rogue came into view.
2. Within a second the bot does its `react` (default: fights back).
3. The brain sees the rogue, its level, its health, the bot's health and what's nearby, and
   answers with a new plan: fight, flee to the guards, or ignore it and die. The new plan
   `expect`s the rogue, so its hits don't wake the brain again; it might watch
   `crossed {base.self.unit.health_pct, below, 30}` to reconsider.

## Tests

- **Unit (harness):** the trigger evaluator on recorded snapshot sequences: each watch fires once
  per crossing, `expect` and `ignore` suppress, the defaults match `client-events.tsv`.
- **Checker:** watch paths and filters name real proto fields; the default list is derived from
  `client-events.tsv`, never hand-written.
- **Played scenarios (e2e, test fixtures):** gank mid-quest, a stun, a whisper, a group invite,
  a ready check, a knockback, health under a watched line, a hostile player coming into view with
  a watch set, death, a Dungeon Finder proposal: each checks the brain was woken, with what, and
  how fast.
