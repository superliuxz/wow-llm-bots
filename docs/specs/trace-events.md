# Behavior trace schema

Unified observer-parity JSONL traces written by `server_module/src/snapshot/behavior_trace.{h,cc}`
under `<LlmBrain.TraceDir>/<role>/<name>/<YYYY-MM-DD>.jsonl` (`role` is `"human"` or `"bot"`).
Each line is one JSON object with a `kind` of `"sample"`, `"event"`, or `"chat"`.

- **Cadence:** periodic samples every `LlmBrain.TraceSampleIntervalMs` (default `200` ms = 5 Hz),
  plus an immediate sample on every combat or jump transition. `trace_parity.py` rejects traces
  whose measured median source rate is below 3.5 Hz; world-tick rounding adds up to one tick
  (~50 ms) on top of `TraceSampleIntervalMs`, and the module warns at runtime when the measured
  rate in the trace drops below 3.5 Hz (keep the default `200` ms whenever traces are scored).
- **Volume cap:** each day file rotates at `LlmBrain.TraceMaxFileMb` (default `64`) and keeps at
  most `LlmBrain.TraceMaxRotatedFiles` archives (default `8`: `.1.jsonl` .. `.8.jsonl`), so the cap
  per character per day is `TraceMaxFileMb * (TraceMaxRotatedFiles + 1)`; older archives are
  overwritten.

This file is checked against the code by `tools/bench/trace_events_spec_test.py` (run by
`tools/wlb check`): every sample field, every event name the C++ emits, and the "Parity" column
(`trace_parity.PARITY_ALLOWED_EVENTS`) must match exactly.

## `kind: "sample"`

| Field | Meaning |
|---|---|
| `kind` | Always `"sample"`. |
| `t_ms` | Wall-clock Unix time (ms). |
| `mono_us` | Monotonic clock (µs), for exact inter-sample deltas. |
| `role` | `"human"` or `"bot"`. |
| `name` | Character name. |
| `map` | Map ID. |
| `inst` | Dungeon instance ID (`0` in the open world). |
| `zone` | Zone display name. |
| `in_town` | Resting flag (inn / city). |
| `in_group` | In a party or raid. |
| `in_dungeon` | On a dungeon map. |
| `x` | World X. |
| `y` | World Y. |
| `z` | World Z. |
| `o` | Facing (radians). |
| `moving` | Moving. |
| `jumping` | `MOVEMENTFLAG_FALLING` set. |
| `mounted` | Mounted. |
| `swimming` | In water. |
| `sitting` | Stand state is not standing. |
| `on_taxi` | On a flight path. |
| `in_combat` | In combat. |
| `dead` | Dead or ghost. |
| `cc_mask` | Loss-of-control bitmask (`ControlMask`). |
| `bag_free_slots` | Free slots in general-purpose bags. |
| `pet_hp_pct` | Pet HP % (`-1` if no live pet). |
| `level` | Character level. |
| `xp` | Current XP. |
| `hp_pct` | HP %. |
| `mana_pct` | Mana % (`0` for non-mana classes). |
| `casting` | Own preparing or channeled spell name (empty if none). |
| `target` | Selected unit name (empty if none). |
| `target_dist` | Distance to a hostile, alive target (yd; `-1` otherwise). |
| `target_x` | Hostile target X, or the cast's destination X for ground-targeted spells. |
| `target_y` | Hostile target Y, or the cast's destination Y for ground-targeted spells. |
| `target_hp_pct` | Hostile target HP % (`-1` otherwise). |
| `target_level` | Hostile target level (`0` otherwise). |
| `target_casting` | Hostile target's preparing or channeled spell name. |
| `target_cast_radius` | Largest DBC effect radius of that spell (`CalcRadius`, yd; `-1` if none). |
| `target_moving` | Hostile target is moving. |
| `target_hostile` | Selected target is hostile and alive. |
| `target_targeting_me` | Hostile target's target or victim is this character. |
| `target_in_combat` | Hostile target is in combat. |
| `attackers` | Number of units attacking this character. |

## `kind: "event"`

Every event row carries `t_ms`, `mono_us`, `role`, `name`, `event`, `detail` and `value`. Other
players' names are never written (`detail` is `""` where the other party would be a player).
"Parity" = listed in `trace_parity.PARITY_ALLOWED_EVENTS` (kept in blind human-vs-bot clips);
events marked "No" are dropped from parity clips because humans and bots don't reach them through
the same server path.

| Event | `detail` | `value` | Parity |
|---|---|---|---|
| `login` | `""` | level | Yes |
| `logout` | `""` | level | Yes |
| `combat_start` | `""` | attacker count | Yes |
| `combat_end` | `""` | attacker count | Yes |
| `spell_cast` | spell name (fires when `Spell::cast` executes/completes) | spell ID | Yes |
| `damage_dealt` | NPC victim name (`""` for a player victim; never self/environment) | damage | Yes |
| `damage_taken` | NPC attacker name, `"environment"` (no attacker or self), or `""` (player attacker) | damage | Yes |
| `heal_done` | `""` | amount healed | Yes |
| `mob_kill` | creature name (credited to group members within 100 yd; loot recipient when there is no killer) | creature level | Yes |
| `died` | killer creature name, or `"environment"` (fall, lava, drowning, self) | killer level (`0` for environment) | Yes |
| `pvp_kill` | `""` | victim level | No |
| `died_pvp` | `""` | killer level | No |
| `release_spirit` | `""` | `0` | Yes |
| `resurrect` | `""` | restore percent | Yes |
| `xp_gain` | killed unit name (`""` for non-kill XP) | XP amount | Yes |
| `level_up` | `""` | new level | Yes |
| `money_changed` | `""` | copper delta | No |
| `rep_change` | faction ID | new standing | No |
| `quest_accept` | quest title | quest ID | Yes |
| `quest_turnin` | quest title | quest ID | Yes |
| `quest_reward` | item name | count | Yes |
| `quest_abandon` | `""` | quest ID | No |
| `loot` | item name, or `"money"` | count, or copper | Yes |
| `loot_roll_win` | item name | `count * 100 + vote_type` | No |
| `item_gain` | item name | count | Yes |
| `item_equip` | item name | item entry | Yes |
| `item_unequip` | item name | item entry | No |
| `item_remove` | item name | count | Yes |
| `item_craft` | item name | count | Yes |
| `item_use` | item name | item entry | Yes |
| `item_open` | item name | item entry | No |
| `mail_take_item` | item name | count | No |
| `vendor_repair` | `""` | `0` | No |
| `spell_learn` | spell name | spell ID | Yes |
| `talent_learn` | talent ID | rank | Yes |
| `skill_up` | skill ID | new skill value | Yes |
| `taxi_learn` | `""` | taxi node ID | No |
| `zone_change` | zone name | zone ID | Yes |
| `area_change` | area name | area ID | Yes |
| `map_change` | `""` | map ID | Yes |
| `group_join` | `""` | member count | Yes |
| `group_invited` | `""` | `0` | No |
| `group_leave` | `""` | remove method | Yes |
| `go_interact` | game object name | loot state | Yes |
| `emote` | `""` | text emote ID | No |
| `duel_request` | `""` | `0` | No |
| `duel_win` | `""` | `0` | No |
| `duel_lose` | `""` | `0` | No |
| `act_sent` | `<VERB> [<name>] [[err: <reason>]]` (bot-only harness diagnostic) | act distance `act.yd` (truncated to int) | No |

## `kind: "chat"`

Chat is its own row kind, not an event. `trace_parity.py` turns each chat row into a synthetic
`chat_in` / `chat_out` clip event (from `dir`) carrying only the text length. Human traces record
only whispers, with `peer` and `text` empty (timing and length only). Bot traces record `peer` for
both directions, the text only for `out`, and only the length for `in`.

| Field | Meaning |
|---|---|
| `kind` | Always `"chat"`. |
| `t_ms` | Wall-clock Unix time (ms). |
| `mono_us` | Monotonic clock (µs). |
| `role` | `"human"` or `"bot"` (whose trace file this is). |
| `name` | Trace owner's character name. |
| `dir` | `"in"` (received) or `"out"` (sent). |
| `channel` | `"whisper"`, `"say"`, `"party"`, ... |
| `peer` | Sender (`in`) or recipient (`out`); empty in human traces. |
| `text` | Message text; empty in human traces and for bot `in` rows. |
| `text_len` | Byte length of the message (set even when `text` is empty). |
