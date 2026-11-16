# Triggers: what wakes the brain

The brain doesn't look at every snapshot. It answers with a **plan**, C++ carries the plan out at
game speed, and the brain is woken by a **trigger**: something the plan didn't account for, or
something the brain asked to be told about. Measured: one bot's think takes 0.4-1 s, a 50-bot
batch 8-10 s (`docs/phase2-results.md`), so the brain can't be in every step, and doesn't need
to be: neither is a human.

C++ never judges here. It carries out the brain's plan, checks the conditions the brain set,
and applies defaults taken from the game client (what the client itself alerts a player about).

## The plan (the brain's answer)

| Field | What it is |
|---|---|
| `goal` | what the bot is working towards, in its own words ("10 scorpid tails, then turn in") |
| `steps` | what to do, in order: options and commands (`snapshot.md` §9), each optionally `until` a condition ("until 10 tails") |
| `expect` | what the plan intends to deal with, as filters ("Scorpid Worker" mobs): these don't count as surprises |
| `watch` | extra conditions to wake on (below) |
| `ignore` | default triggers not to wake on for this plan ("whispers", "combat feedback from expected mobs") |
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
| `changed {path}` | the field changes | `self.in_combat`; `self.control`; `party.members` |
| `crossed {path, below/above, value}` | a number crosses the line | `self.health` below 40%; `target.health_pct` below 20%; power below 30% |
| `appears {list, filter}` / `gone {list, filter}` | an entry matching the filter enters or leaves a list | a hostile player in `nearby.units` within 40 yd; a new entry in `combat.attackers`; an aura on me (`self.unit.auras`); a friend online |
| `elapsed {s}` | time passes | |

Filters compare an entry's fields: `{kind: PLAYER, reaction: HOSTILE, level >= 70,
rel.dist_yd < 40}`, `{name: "Hogger"}`, `{dispel_type: MAGIC}`. Units coming into view have no
client event (a player just sees them), which is why `appears` exists.

## Defaults (from the client)

When a plan says nothing, a trigger wakes the brain if **the client would get the player's
attention**. The list is derived from the client's own UI code (`client_alert` in
`client-events.tsv`), not chosen by us:

| Default wake | Why (the client's alert) |
|---|---|
| every **prompt** | a popup waits for an answer (and often has a timer) |
| events the client alerts on: whisper (sound), raid warning (raid notice + sound), Dungeon Finder proposal (sound), achievement (alert frame), died (popup), resurrect offer (popup + sound), ... | the event's `client_alert` has a sound, popup, flash, raid notice, alert frame or error line |
| **entered combat** (`self.in_combat` changed to true) when the plan `expect`s no fight | the red screen-edge flash (`PLAYER_REGEN_DISABLED`) |
| a new entry in `combat.attackers` that the plan doesn't `expect` | combat feedback on my portrait (`UNIT_COMBAT`): how a player notices being hit; the 3.3.5a UI plays no aggro sound |
| **lost control** (`self.control` changed: stunned, feared, charmed, possessed) | the action bars stop responding and the camera or character moves on its own |
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
   `crossed {self.health, below, 30%}` to reconsider.

## Tests

- **Unit (harness):** the trigger evaluator on recorded snapshot sequences: each watch fires once
  per crossing, `expect` and `ignore` suppress, the defaults match `client-events.tsv`.
- **Checker:** watch paths and filters name real proto fields; the default list is derived from
  `client-events.tsv`, never hand-written.
- **Played scenarios (e2e, test fixtures):** gank mid-quest, a stun, a whisper, a group invite,
  a ready check, a knockback, health under a watched line, a hostile player coming into view with
  a watch set, death, a Dungeon Finder proposal: each checks the brain was woken, with what, and
  how fast.
