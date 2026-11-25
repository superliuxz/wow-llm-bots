# Snapshot Wire Contract (Overview)

**Source of truth:** [snapshot/snapshot.h](../../snapshot/snapshot.h) (plain C++20 structs) and
[snapshot/snapshot_wire.cc](../../snapshot/snapshot_wire.cc) (Glaze BEVE binary wire format +
strict JSON for fixtures and `snapshot_check`).

## Framing & Messages

`mod-llm-brain` and `harness` communicate over a local Unix domain socket using 4-byte
big-endian length-prefixed BEVE frames (`common/net/frame_codec.h`, max payload 1 MiB,
`kWireVersion` checked on every message).

- **Handshake (`hello`):** the module's first frame on every connection is a `ModuleMessage`
  carrying only `hello` (`wire_version`, `schema_hash` = `SchemaHash()`, a hash of
  `kWireVersion` and every `ModuleMessage`/`Command` field path, and `role: "module"`). A client
  may answer with a `Command` carrying only `hello` (the harness does, `role: "harness"`;
  `wlb snap fake` sends `"wlb snap fake"`). A side whose peer's hello names another version or
  schema refuses that connection with one log line saying what to rebuild: the harness ends its
  run (exit 2); the module drops the client's commands, stops building snapshots for it and
  hands the bots back to playerbots until it disconnects. A client that sends no hello (a
  passive capture tool, raw-socket e2e cases) is still served; `wire.decode_module_message`
  checks and consumes the module's hello so Python readers only see snapshots and events.
- **Module → Harness (`ModuleMessage`):**
  - `Snapshot`: periodic picture of what one bot sees (`1 s` out of combat, `250 ms` in combat,
    staggered across bots, latest-wins per bot if the socket backs up; sooner on a combat, life
    or control change, a new cast or aura on the bot, its target or attackers (`>= 50 ms` apart),
    or in combat a new hostile cast or spell area within 40 yd (`>= 100 ms` apart)). Carries
    `seq`, `time_ms`, `monotonic_us`, `done_command_id`, `done_act_command_id`, `base`, `ext`,
    `events`, and `options`.
  - `ImmediateEvent`: alerting events (chat, errors, info, prompts, loot rolls) pushed at once,
    never replaced or reordered.
- **Harness → Module (`Command`):**
  - `choose`: pick 1-based `option` from snapshot `seq` (resolved through `OptionHistory`).
  - `act`: standing-order combat/movement verb (`to`, `away`, `out of`, `back`, `target`, `use`,
    `stop`, `wake`, `avoid`, `face`, `jump`, `cancel`, `mark`, `interact`, `board`, `leave`,
    `equip`, `pet`, `kite`).
  - `say`: speak in `SAY`/`PARTY`/`RAID`/`WHISPER`/etc.
  - `open` / `close` / `hover`: toggle UI panels (`BAGS`, `MAP`, `CHARACTER`, `QUEST_LOG`,
    `SPELLBOOK`) or inspect one hover target on the next snapshot.

## Visibility Rules (Client Parity)

Enforced mechanically on every fixture and live capture by `ValidateSnapshotVisibility`
(`snapshot/snapshot_wire.cc`) and `wlb check snapshots`:

1. **`base` (always visible at a glance):** `self` (HP/power/state/cast/auras/indicators),
   `where` (zone/subzone/position/facing), `nearby` (units, usable game objects, ground `areas`
   within client visibility & LoS rules; invisible server triggers excluded), `ui` (open panels
   and active hover refs), and `tooltips`.
2. **`ext` (contextual & panel-gated):**
   - Contextual (`minimap`, `quest_tracker`, `action_bars`, `world`, `target`, `combat`, `party`,
     `raid`, `encounter`, `instance`, `death`, `vehicle`, `interaction`, `pet`, `possessed`)
     populated only when relevant in the client. `action_bars.buttons` is every snapshot's
     castable spellbook from pure `Spell.dbc` metadata (active spec, non-passive, displayed,
     non-tradeskill, highest known rank, each name once, with cooldown left), rendered as one
     `spells:` screen line capped at 600 B (`+N more`). `minimap.ping` is the last party
     member's minimap ping (`MSG_MINIMAP_PING`: who, ground position relative to the bot, age),
     kept 30 s while the pinger is still in the group on the bot's map/instance, rendered as a
     core `ping: <Name> pinged 18 yd NE` line (`(Ns ago)` once 5 s old).
   - Panel-gated (`bags`, `map`, `character`, `quest_log`, `spellbook`, tagged `visibility: OPEN`):
     present only when the corresponding `Panel` is in `base.ui.open_panels`.
3. **Hover-only fields (`visibility: HOVER`):** 26 fields (e.g., `PlayerPart::race`,
   `Item::vendor_value`, `Unit::services`) may appear **only** inside `base.tooltips` for a
   currently hovered `HoverRef`. `wlb check snapshots` checks that `kHoverFields[]` matches
   every `visibility: HOVER` tag in `snapshot/snapshot.h`.

## Offline Fixture Facts

`OptionFacts` (`snapshot/snapshot.h`) stores the server-side facts `AddOptions` reads (NPC flags,
bag food/drink, gathering skill, travel spots/places, usable/equippable items, NPC/group actions)
so `fixture_options` generates the exact same numbered `options` offline as the live module.
