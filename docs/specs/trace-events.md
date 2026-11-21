# Behavior trace schema (`TraceSample` & `TraceEvent`)

Unified observer-parity JSONL traces written by `server_module/src/snapshot/behavior_trace.{h,cc}`
under `<LlmBrain.TraceDir>/<role>/<name>/<YYYY-MM-DD>.jsonl` (`role` is `"human"` or `"bot"`).
Files rotate at 64 MiB (up to 8 archives: `.1.jsonl`..`.8.jsonl`). Periodic sampling cadence is
controlled by `LlmBrain.TraceSampleIntervalMs` (default `200` ms = 5 Hz, plus immediate edge samples
on combat and jump transitions).

## `kind: "sample"` (`TraceSample`)

| Field | Type | Meaning |
|---|---|---|
| `t_ms` | int64 | Wall-clock Unix timestamp (ms). |
| `mono_us` | int64 | Monotonic clock (µs) for exact inter-sample deltas. |
| `role`, `name` | string | `"human"` or `"bot"`, and character name. |
| `map`, `inst`, `zone` | uint32 / uint32 / string | Map ID, dungeon instance ID (`0` in open world), and zone display name. |
| `in_town`, `in_group`, `in_dungeon` | bool | Resting/town flag, in party/raid, and inside a dungeon map. |
| `x`, `y`, `z`, `o` | float | World position and facing orientation (radians). |
| `moving`, `jumping`, `mounted`, `swimming`, `sitting`, `on_taxi` | bool | Movement and posture flags (`jumping` = `MOVEMENTFLAG_FALLING`). |
| `in_combat`, `dead`, `cc_mask` | bool / bool / uint32 | Combat state, dead state, and crowd-control bitmask (`ControlMask`). |
| `bag_free_slots`, `pet_hp_pct` | uint32 / float | Free general bag slots; pet HP % (`-1` if no live pet). |
| `level`, `xp`, `hp_pct`, `mana_pct` | uint32 / float | Character level, current XP, HP %, mana %. |
| `casting` | string | Active preparing or channeled spell name (empty if none). |
| `target`, `target_dist`, `target_x`, `target_y` | string / float | Selected target name; if hostile & alive, distance (yd) and impact coordinates (`Spell::m_targets.GetDstPos()` for ground-targeted spells, else caster position). |
| `target_hp_pct`, `target_level`, `target_casting`, `target_cast_radius` | float / uint32 / string / float | Hostile target HP %, level, active cast name, and DBC max effect radius (`CalcRadius()`, `-1` if none). |
| `target_moving`, `target_hostile`, `target_targeting_me`, `target_in_combat` | bool | Hostile target state flags. |
| `attackers` | int | Number of units currently attacking the character. |

## `kind: "event"` (`TraceEvent`)

All events carry `t_ms`, `mono_us`, `role`, `name`, `event`, `detail`, and `value`.
Peer player names are redacted (empty `detail`) on PvP, heal, whisper, and group events.

| `event` | `detail` | `value` | Parity allow-list? |
|---|---|---|---|
| `combat_start` / `combat_end` | `""` | attacker count | Yes |
| `cast_start` | spell display name | spell ID | Yes |
| `damage_dealt` | NPC name (`""` if player; excludes self/env) | damage | Yes |
| `damage_taken` | NPC name, `"environment"`, or `""` (player) | damage | Yes |
| `heal_done` | `""` (ally name redacted) | healing | Yes |
| `mob_kill` | creature name (credited within 100 yd, incl. null-killer loot recipient) | creature level | Yes |
| `died` | killer creature name or `"environment"` | killer level (`0` for env) | Yes |
| `pvp_kill` / `died_pvp` | `""` (never fired on suicide/env) | opponent level | Yes |
| `release_spirit` / `resurrect` | `""` | `0` / restore % | Yes |
| `loot` | item name or `"money"` | count or copper | Yes |
| `loot_roll_win` | item name | `count * 100 + vote_type` | Yes |
| `quest_accept` / `quest_turnin` | quest title | quest ID | Yes |
| `quest_reward` / `item_gain` / `item_open` / `item_use` | item name | count or item entry | Yes (except `item_use` hook) |
| `level_up` / `xp_gain` / `money_change` / `reputation` | `""` / source / `""` / faction | level / XP / copper delta / rep delta | Yes |
| `group_join` / `group_invited` / `group_leave` | `""` | member count / `0` / remove method | Yes |
| `zone_change` / `area_change` / `map_change` | area/zone name or `""` | zone/area/map ID | Yes |
| `chat_say` / `chat_yell` / `chat_party` / `chat_raid` / `chat_channel` / `chat_whisper_out` / `chat_whisper_in` | message text (peer name redacted) | message length | Yes |
| `act_sent` | standing-order act dispatched by harness (bot-only diagnostic) | `0` | **No** (excluded by `PARITY_ALLOWED_EVENTS`) |
| `item_unequip` / `quest_abandon` | item name / `""` | entry / quest ID | **No** (asymmetric between humans and playerbots) |
