# Brief: classify playerbots boss actions against the standing-order `<act>` grammar

How `trigger-actions.tsv` is made: all 585 `bool <class>::Execute` bodies in
`src/Ai/Raid/**/*Actions*.cpp` and `src/Ai/Dungeon/**/*Actions*.cpp` are listed with `rg`,
split into six groups, and each group is read by an agent with this brief.

Playerbots source (read only, never edit):
`third_party/mod-playerbots` (pinned commit `ecc4a53`). Snapshot proto:
`server_module/proto/snapshot.proto`. Spec for context: `docs/specs/triggers.md`, section
"Standing orders".

## Your input

A TSV (path given in your task) with one row per action: `file<TAB>line<TAB>class`, where `line`
is the line of `bool <class>::Execute(...)` in the file (relative to `third_party/mod-playerbots`).
Read each body. When the action is hidden in a helper (a `*Helpers.cpp`, a base class, a
`helper.` call, `MoveTo(...)`, `Attack(...)`, etc.), **follow it** until you know what the action
actually does.

## The question for each action

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

## Output

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
- `snapshot_gap`: what `snapshot.proto` lacks for the bot/executor to do this, or `-`.
- `note`: one short line (no tabs or newlines): what the action actually does in plain words.

Be literal and conservative: if you're unsure whether it fits, say `unsayable` with `other` and explain.
