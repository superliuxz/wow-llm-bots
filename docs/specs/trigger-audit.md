# How the trigger audit tables were made

The briefs given to the readers (LLM agents) that classified playerbots' boss triggers and
actions against the standing-order grammar (`triggers.md`): conditions into
`trigger-conditions.tsv`, actions into `trigger-actions.tsv`. Each was its own file until
2026-11-18.

## Conditions: boss triggers (`trigger-conditions.tsv`)
How `trigger-conditions.tsv` was made (2026-11-16): the 595 `bool <class>::IsActive` bodies in
`src/Ai/Raid/*/*Triggers.cpp` and `src/Ai/Dungeon/*/*Triggers.cpp` were listed with grep, split
into six groups, and each group given to a reader (an LLM agent) with this brief. Afterwards
the five `*Cheat*` triggers (they only fire with playerbots' raid cheat on) got their own
verdict, `cheat`, and the `when` cells were normalised until the grammar checker (now the C++ parser: `OrdersTest.*Audit*`)
passed.

Playerbots source (read only, never edit):
third_party/mod-playerbots (pinned commit ecc4a53). Snapshot structs:
snapshot/snapshot.h (originally audited against snapshot.proto). Spec for context: docs/specs/triggers.md, section
"Standing orders".

### Your input

A TSV (path given in your task) with one row per trigger: `file<TAB>line<TAB>class`, where `line`
is the line of `bool <class>::IsActive...()` in the file (relative to third_party/mod-playerbots).
Read each body. When the condition is hidden in a helper (a `*Helpers.cpp`, a base class, a
`helper.` call, a value like `AI_VALUE(...)`), **follow it** until you know what the trigger
actually checks. Also read the matching `*Actions.cpp` only if you need it to know what the
trigger is for.

### The question for each trigger

Could a player bot's brain write this trigger's condition as the `when` side of a standing order
in this grammar, with the facts the snapshot provides? (The action side is not the question.)

```
order   ::= when " -> " act (", then " act){0,2}
when    ::= "always" | cond (" & " cond){0,2}
cond    ::= "not "? test
test    ::= mech " on " who | mech " stacks" cmp cnt " on " who
          | mech " cast" | mech " appears" | mech " count>" cnt
          | thing (" within " | " beyond ") num | thing " targets " who
          | thing " hp" cmp pct | "mana" cmp pct | thing " is " state
          | "me " mystate | "phase " digit | "after " num "s" | "every " num "s"
cmp     ::= "<" | ">"
who     ::= "me" | "tank" | "healer" | "anyone" | "boss"
thing   ::= "it" | "boss" | "tank" | mech | "marker " mark
state   ::= "flying" | "grounded" | "untargetable" | "submerged"
mystate ::= "in vehicle" | "mounted" | "swimming" | "nearest to it"
act verbs: to, away, target, use, stop, face, jump, cancel, avoid, wake
```

`mech` = a named spell, aura or unit (by name, as a player sees it). `phase N` comes from DBM
(BossPart.phase), so it is allowed. "Or" is allowed as two orders with the same action (say so
in the note). Roles ("I am a tank/healer/ranged/melee") are known to the bot: a role check alone
is not a condition, it decides whose orders these are; put the role in the `role` column.
The boss being engaged / alive / in combat is `boss is ...`-free context: treat "the fight with
boss X is on" as given (the order is written for that fight).

### Output

Write a TSV with a header row to the output path given in your task, exactly one row per input
row, same order, columns:

`file  line  class  boss  role  verdict  when  missing  snapshot_gap  note`

- `boss`: the encounter (e.g. "Lich King", "Sindragosa"); `-` if generic.
- `role`: tank, healer, ranged, melee, any, or a specific class/spec, or `-`.
- `verdict`, one of:
  - `fits`: sayable in the grammar; write it in `when`.
  - `always`: no real condition beyond the fight (and role): a standing position, kill priority,
    or "this role does this the whole fight". `when` = `always`.
  - `unsayable`: can't be said in the grammar; `when` = `-`.
  - `bugnet`: guards against bot bugs, not a mechanic (fell through the floor, out of bounds,
    stuck, resetting state); `when` = `-`.
  - `plumbing`: not a mechanic at all (a base class, a helper update that always returns true,
    a commented or empty body); `when` = `-`.
- `missing` (for `unsayable`, and also for `fits` that needed a stretch): one or more of these,
  comma-separated, else `-`:
  `assignment` (computed in code and shared between bots: who takes which add/cube/beam, a
  rotation of bots, a numbered slot), `fixed_location` (coordinates, a height, a room or
  platform), `nonaura_state` (a unit state not in the `state` list), `distance_other` (a
  distance not between me/a thing, e.g. between two other units), `health_range` (health
  between two lines, or of a unit that isn't a `thing`), `vehicle_gear` (vehicle seat, pet,
  item, gear state), `timer` (a stored or repeating timer, a latched flag, "since X happened"),
  `count` (counting units/players other than `mech count>`), `marker` (a raid marker on the
  ground or as a place), `other` (explain in the note).
- `snapshot_gap`: what the snapshot (`snapshot/snapshot.h`) lacks for a player to see this, or `-`.
  Check the schema when unsure; name the field you would need.
- `note`: one short line: what the trigger checks, in plain words.

Use TABs, no TABs or newlines inside a cell. Be literal and conservative: if you're unsure
whether it fits, say `unsayable` with `other` and explain. Don't edit any repo file. When done,
reply with: rows written, counts per verdict, the top `missing` reasons, and up to 5 rows you
were least sure of.

## Actions: boss actions (`trigger-actions.tsv`)
How `trigger-actions.tsv` is made: all 585 `bool <class>::Execute` bodies in
`src/Ai/Raid/**/*Actions*.cpp` and `src/Ai/Dungeon/**/*Actions*.cpp` are listed with `rg`,
split into six groups, and each group is read by an agent with this brief.

Playerbots source (read only, never edit):
`third_party/mod-playerbots` (pinned commit `ecc4a53`). Snapshot structs:
`snapshot/snapshot.h` (originally audited against `snapshot.proto`). Spec for context: `docs/specs/triggers.md`, section
"Standing orders".

### Your input

A TSV (path given in your task) with one row per action: `file<TAB>line<TAB>class`, where `line`
is the line of `bool <class>::Execute(...)` in the file (relative to `third_party/mod-playerbots`).
Read each body. When the action is hidden in a helper (a `*Helpers.cpp`, a base class, a
`helper.` call, `MoveTo(...)`, `Attack(...)`, etc.), **follow it** until you know what the action
actually does.

### The question for each action

Could a player bot's brain write this action as the `<act>` side of a standing order using the
10 provisional verbs (up to 3 sequential steps `act (", then " act){0,2}`), with targets/places
the snapshot can name?

Provisional `<act>` verbs:
- `to <target/place>`: move to / stack on / run to unit or place
- `away <target/hazard>`: move away from / spread out / get out of hazard
- `target <unit>`: switch target to unit (adds, boss, marker, etc.)
- `use <ability/item> [on <target>]`: cast a spell, use cooldown, potion, dispel, taunt, interrupt, item
- `stop`: stop casting or attacking
- `face <unit>`: turn to face (or face away from) a unit
- `jump`: jump
- `cancel <aura/cast>`: cancel an aura or current cast
- `avoid <unit/hazard/spell>`: don't melee/hit/dispel this, or stay clear of it
- `wake`: escalate to brain

### Output

Return one result per input row, in the exact same order, with fields:
`file  line  class  boss  role  verdict  act  missing  snapshot_gap  note`

- `boss`: encounter name (e.g. "Lich King", "Sindragosa"); `-` if generic.
- `role`: `tank`, `healer`, `ranged`, `melee`, `any`, or a specific class/spec, or `-`.
- `verdict`, one of:
  - `fits`: sayable with the 10 verbs (1–3 steps) and nameable targets/places; write a concise
    example in `act` (e.g. `away`, `to Ice Tomb`, `target Bone Spike`, `use 4 on it`, `stop`).
  - `reflex`: standard combat rotation / healing / dispelling / interrupting / cooldowns that
    belongs in the C++ reflex engine rather than a boss-specific standing order; `act` = `-`.
  - `unsayable`: can't be expressed with the 10 verbs or snapshot-nameable targets/places; `act` = `-`.
  - `bugnet`: guards against bot bugs (unstuck, fell through floor, out of bounds); `act` = `-`.
  - `plumbing`: not an in-game player action (base class, empty/commented body, internal state/timer
    bookkeeping, bot-to-bot coordination/role assignment, raid mark assignment); `act` = `-`.
  - `cheat`: relies on playerbots raid cheat / teleport / GM-like hack; `act` = `-`.
- `missing` (for `unsayable`, and also for `fits` that needed a stretch): one or more of these,
  comma-separated, else `-`:
  `verb` (needs a new verb: e.g. `interact`/click gameobject, `mount`/`dismount`, `follow`, `chat`/say,
  `emote`, `equip`, `loot`, `pet`, `mark`, `kite`), `fixed_location` (hardcoded `(x,y,z)` or room
  spot), `relative_spot` (behind boss, max range, N yd from X, opposite side, spread/triangle),
  `assignment` (shared bot-to-bot slot/cube/beam/group/master), `target_filter` (lowest/highest hp,
  specific aura/role, nearest/furthest, threat/aggro), `vehicle_gear` (vehicle ability/seat, pet,
  equip/bag item), `multi_step` (>3 steps, branching if/else, continuous steering), `other` (explain in note).
- `snapshot_gap`: what `snapshot/snapshot.h` lacks for the bot/executor to do this, or `-`.
- `note`: one short line (no tabs or newlines): what the action actually does in plain words.

Be literal and conservative: if you're unsure whether it fits, say `unsayable` with `other` and explain.
