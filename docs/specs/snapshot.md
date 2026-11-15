# Bot perception: the snapshot spec

Status: **agreed, revision 4** (2026-11-15). The exact field-level contract is the JSON Schema
[`snapshot.schema.json`](snapshot.schema.json) (types inherit with `allOf`); examples in
[`examples/`](examples/), checked by `tools/validate-snapshots.sh`. This document explains it. Everything a bot's brain decides is based on this.

## 1. Principles

1. **Definition: everything a user could possibly see in the WoW client, nothing more.**
   A bot perceives what a human player could get through the 3.3.5a client plus the addons
   that bot "has installed". The test for any field: *is there a place in the client (or one
   of this bot's addons) where a human could see this?* If yes, it belongs here, in the
   extension for that UI place; if no, it doesn't, however useful it would be. The one
   deliberate stand-in is `surroundings`, which approximates the 3D view itself.
2. **A base view plus extensions**, like the real UI: a base screen everyone has, and panels
   or addons that appear in certain situations. **Always-on** extensions (`map`,
   `objectives`, `inventory`, `character`, `world`) model what a player keeps glancing at;
   **conditional** ones (party frames, raid grid, boss timers, an open vendor window...)
   attach while their situation applies.
3. **Built from the client's own information**, not server internals:
   - **Events** come from the packets the server sends the bot's session (AzerothCore's
     `ServerScript::OnPacketSent`, which fires for client-less sessions).
   - **State** is read from the game once per second on the world thread, filtered to what
     the client knows: the bot's own state and objects in the client's visibility set
     (`Player::m_clientGUIDs`).
4. **One type per concept.** Items, spells, auras and units are always described with the
   same shared types (§3), wherever they appear (bags, vendor, loot, trainer, buffs, enemy
   casts...), including tooltips, cooldowns and GearScore.
5. **Describe, don't decide.** The snapshot reports what's on screen. The only "help" is the
   numbered **options**, which mirror what a player can click or do next.
6. **Testable.** Each scene in §10 is a check: "in scene X the snapshot must contain Y".

## 2. Transport and storage

- The module streams one snapshot per attached bot every `LlmBrain.SnapshotIntervalMs`
  (1 s), plus **events immediately**. Nothing is stored server-side.
- The harness keeps only the **latest snapshot per bot** in memory.
- On disk: the snapshot behind each **decision** (with prompt, reply, choice, latency) in
  `var/bots/<name>/events/<date>.jsonl`; optional full recording to `out/recordings/`
  (debug only; ~8-9 GB/day at 50 bots).

```json
{"type":"snapshot","bot":"Dezlu","seq":1234,"t":123456789,
 "base":{...},
 "ext":{"map":{...},"objectives":{...},"inventory":{...},"character":{...},"world":{...},"party":{...}},
 "events":[...],
 "options":[...]}
{"type":"event","bot":"Dezlu","seq":1235,"t":123456790,"event":{...}}
```
- `seq`: per-bot counter (gaps reveal drops); `t`: game time in ms.
- Each snapshot is **complete** (not a diff). `ext` holds only the active extensions.
  `events` repeats every event since the previous snapshot (they were also sent live).
- Size budget ~2-4 KB typical, hard cap 16 KB. Tooltips are sent in full only where a
  decision needs them (§7); under pressure lists are truncated by salience.

## 3. Types

Every concept is one type, reused everywhere it appears (object-oriented: children inherit
their parent's fields). Full definitions in the schema's `$defs`.

```
Entity {guid, name}
├─ Located = Entity + Relative {dist_yd, dir, height, in_los, position, relative_position {behind_it, facing_me}}
│  ├─ Unit {kind, level, level_color, reaction, classification, health_pct, power_pct, dead, lootable,
│  │        skinnable, tapped_by_other, targeting_me, target, threat_status, quest_marker, services,
│  │        pvp_flagged, activity (Activity), auras [Aura], raid_marker}
│  │  ├─ Player {class, race, guild, in_my_group, is_friend, gearscore (GearScore addon)}
│  │  │  ├─ Self: the bot (xp, health/power, state, combat/resting/camping/mounted/flight/transport,
│  │  │  │        control {stunned, feared, ...}, casting (Cast), money, new_mail)
│  │  │  └─ GroupMember {role, online, health_deficit, in_range, aggro, subgroup}
│  │  ├─ Boss {phase}
│  │  ├─ Pet {happiness, autocast [Spell]}
│  │  └─ Vehicle {seats, abilities [Ability]}
│  ├─ GameObject {kind (quest object, herb, ore, chest, mailbox, door, campfire, bobber...), usable,
│  │              state ("splashing", "open"...)}
│  ├─ Area: a ground effect {spell, radius_yd, inside_me, hostile, remaining_s}
│  └─ Place {kind (town, camp, flight master, graveyard, dungeon, zone exit, AH...), known, level_range}
├─ Item {id, quality, count, kind, item_level, required_level, bound, unique, vendor_value, usable,
│        cooldown_s, charges, tooltip}
│  ├─ Equipment {slot, durability_pct, stats, gearscore, gearscore_delta, upgrade}
│  └─ PricedItem {price}  (merchant, buyback, auction house)
├─ Spell {id, rank, school, cost, cast_time_ms, range_yd, cooldown_s, tooltip}
│  ├─ Ability {cooldown_remaining_s, usable, in_range, known}  (action bars)
│  └─ TrainerSpell {price, required_level, learnable}
└─ Aura {id, harmful, remaining_s, stacks, dispel_type, dispellable_by_me, caster, tooltip}

Objective {title, progress {have, need}, done, where (Place)}
├─ QuestObjective {quest, text, complete, failed, daily, timer_s, escort}
└─ Goal {id, goal_kind (sell, buy, train, repair, learn_profession, reach_level, travel_to, meet, farm), params}

Activity {state (idle, moving, approaching_me, fleeing, fighting, casting, channeling, talking,
          emoting, looting, gathering, sitting, following, fishing, dead), cast (Cast), with, text}
Cast {spell (Spell), remaining_ms, interruptible, target, channeled}
Event {kind, ...}   (§8)
```

## 4. Base view (always present)

What's on screen with nothing open.

| Block | Fields | Client equivalent |
|---|---|---|
| `self` | **Self** | unit frame, buff bar, cast bar, minimap mail icon |
| `where` | `map`, `zone`, `subzone`, `position`, `facing`, `indoors`, `in_instance` | minimap, zone text |
| `doing` | `{activity, since_s, detail}` (do_quest + title, grind + area, rest, travel...) | the player's own intent |
| `nearby` | **units** (Unit/Player, up to 50), **objects** (GameObject, up to 20), **areas** (Area, up to 20), all in the client's visibility set, sorted by salience. `threat_status` is the default UI's threat indicator (Omen adds numbers). Gathering nodes appear as the client shows them (and on the minimap only with tracking, see `map.tracked`) | nameplates, tooltips, highlighted objects, spell effects on the ground |

Everything else is an extension.

## 5. Extensions

An extension that mirrors an addon (or the addon-provided part of one) is only present if
the bot has that addon in its **loadout** (§6).

### Always-on

| Extension | Content | Mirrors |
|---|---|---|
| `map` | The overview from glancing at the map: current `zone {name, level_range}` and position; **objective locations** from `objectives`; nearby **places** (Place: towns, camps, flight masters, innkeeper, graveyard, dungeon entrances, campfires); zones bordering this one with their `level_range`; party members and corpse marker; **`tracked`**: minimap blips from active tracking (Find Herbs, Find Minerals, Track Beasts...). With **Questie**: quest givers with available quests, and mob/item spawn areas for objectives | world map, minimap (+ **Questie** pins) |
| `objectives` | Everything the bot is working towards: `{kind, title, progress, done, where {place, dir, dist_yd}, source}`. **Quests** (`source: quest`): from the quest log, objectives `{text, have, need}`, `complete`, `failed`, `daily`, `timer_s` (timed quests), `escort` (the escorted unit), `where` from quest POI. **Self-set goals** (`source: goal`, set by the brain with `set_goal`, §9): `sell` (items, AH or vendor), `buy`, `train`, `repair`, `learn_profession`, `reach_level`, `travel_to`, `meet`, `farm` (item x count)...; the module fills in progress (bags, level, gold, skill) and `where` (nearest auctioneer / trainer / vendor / place) | quest tracker + the player's to-do list |
| `inventory` | **Full bag contents** (Item list with slots); `free_slots/total`; **equipped gear** (Item per slot, with durability and GearScore); total GearScore (with GearScore); ammo; hearthstone (Item, ready/bind location) | bags, character pane, **GearScore** |
| `character` | **unspent talent points**, talent spec summary (points per tree, key talents); **skills**: professions and secondary skills `{name, skill, max}`, riding, weapon skills; reputations with relevant factions `{faction, standing, value}`; known flight paths count; rested state | talent pane, skills/reputation tabs |
| `world` | `game_time` (day/night), `session_minutes`, `daily_reset_in_s`, zone PvP status, server messages pending (shutdown countdown) | clock, session |

### Conditional

| Extension | Active when | Content | Mirrors |
|---|---|---|---|
| `target` | has a target (and `focus` if set) | Unit (with all its auras and its `activity`), raid marker | target / focus frames |
| `combat` | in combat | attackers (Unit, `targeting_me`), duel opponent, my **threat** on each (**Omen**), **abilities** (Spell list of what's on my bars: cooldowns, usable, in range), resources (combo points, runes), recent combat summary (hits taken/dealt by whom, incoming casts) | action bars, **Omen**, scrolling combat text |
| `party` | in a party (not raid) | leader; members (Unit with `role`, `power_pct`, `target`, `auras` incl. `dispellable_by_me`, offline); buffs missing that I can give; loot method; raid markers | party frames (+ **Decursive** dispel highlights) |
| `raid` ("grid") | in a raid | compact grid by subgroup: members `{name, class, role, health_deficit, dead, in_range, in_los, aggro, dispellable_by_me, target}`; main tanks; raid markers | **Grid / VuhDo** |
| `encounter` | engaged with a boss | boss Unit (`health_pct`, `activity`), `phase`, **DBM** timers and warnings `{name, remaining_s}` | **DBM** |
| `instance` | inside a dungeon or raid | `name, difficulty, size`, bosses `{name, killed}`, lockout, entrance location (for corpse runs from inside) | instance info, raid info |
| `death` | dead or ghost | `corpse {dir, dist_yd}`, `can_resurrect_in_s`, `spirit_healer {dir, dist_yd}`, pending resurrect offers | death UI, corpse arrow |
| `vehicle` | controlling or riding a vehicle | vehicle Unit, its health/power, its abilities (Spell list), seats | vehicle UI |
| `interaction` | a window is open | **merchant** (Item list with prices, buyback), **trainer** (Spell list with cost, learnable), **quest giver** (quest text, objectives, rewards as Items, choices), **flight map** (destinations with cost), **bank**, **mail** (letters, attached Items/money, COD), **auction house** (my auctions, search results / prices for items I'm selling or buying), **tradeskill** (recipes `{name, reagents [{Item, have, need}], skillup_color, can_craft}`), **gossip** (dialog options), **inspect** (another player's equipped Equipment and GearScore), **item_text** (books, letters, notes), **raid_browser** (listings) | the open window |
| `trade_loot` | trade window, loot window or loot roll open | trade contents both sides (Items, money); lootable Items; roll `{Item, roll_id, time_left_s}` | trade / loot / roll frames |
| `travel` | travelling, or no quest target nearby | known flight paths, nearest flight master, hearthstone, mount, transports (zeppelin/boat: where they go, when they leave) | world map, flight map, schedules |
| `pet` | has a pet / totems / minion | pet Unit (`happiness` for hunters, abilities on autocast); shaman totems active + timers | pet frame, totem bar |
| `social` | idle in a city/town, or a social event | friends online; `guild {name, rank, motd, online}`; **`calendar`** (events and my sign-up status: guild raid nights); LFG listings ("LF2M Deadmines") | friends list, guild panel, calendar, chat channels |
| `environment` | breath, fatigue or falling active | `breath_s`, `fatigue_s`, `falling {height_yd}` | mirror timers |
| `battleground` | in a battleground | score, objectives (Place + owner), flag carriers, time remaining | battleground UI |
| `surroundings` | stuck, `path_failed`, or in tight terrain | what a human sees in 3D, computed from the navmesh/vmaps: `walkable` directions, `obstacles` (wall, slope, drop, water, ledge, opening, door... with dir/dist/height), the `goal` direction, a short `description`, and optionally a synthetic top-down `image` (rendered from map data, no game client) for a vision model | the 3D view |

### Role and class flavors
Same extensions, different focus, like a player's UI layout:
- **Healer:** `party`/`raid` list health deficits first and mark `dispellable_by_me` from the
  bot's class (priest: magic + disease; paladin: magic + poison + disease; shaman: poison +
  disease, curse with Cleanse Spirit; druid: curse + poison; mage: curse).
- **Tank:** `combat` threat on every nearby enemy, loose mobs, who they target.
- **DPS:** `target` and `combat` cooldowns first; marker priority (skull, cross).
- Class extras in `combat`/`pet`: combo points, runes, pet happiness, ammo, soul shards, totems.

## 6. Addon loadout (per bot)

Each bot has an addon list in its memory (`var/bots/<name>/addons.md`), chosen by its
personality and experience: a veteran raider runs DBM, Omen, Grid and GearScore; a new player
has the default UI plus maybe Questie. Addon-backed content (Questie pins, Omen threat,
Grid/VuhDo, Decursive highlights, DBM timers, GearScore values, Skada meters) is only
produced for bots that have the addon. Bots can install addons later ("someone told me to
get DBM"). The default UI parts (quest log + native quest POI, party frames, target frame,
tooltips...) are always available.

## 7. Salience and budget

Priority when the snapshot must shrink (lowest trimmed first):
`world` < `social` < `travel` < `map` (nearest places first) < `character` <
`inventory` (notable items first: quest, consumables, upgrades, junk total) < `pet` <
`instance` < `battleground` < `objectives` < `interaction` < `trade_loot` < `vehicle` <
`surroundings` < `party`/`raid` <
`target` < `death` < `combat` < `encounter` < base.
Within lists: hostile-and-targeting-me > casting-at-me > quest-related > interactable >
other; nearest first. Tooltips are dropped before entries are.

## 8. Events (from packets to the bot's session)

| Event | Source packet(s) |
|---|---|
| `chat {channel, from, text, at_me, from_dist_yd}` (say / yell / whisper / party / raid / raid_warning / guild / officer / general / trade / lfg / emote / monster say-yell-emote / system) | SMSG_MESSAGECHAT (+ GM variant), SMSG_TEXT_EMOTE, SMSG_NOTIFICATION, server messages |
| `error {text}` (red UI text) | SMSG_CAST_FAILED, SMSG_INVENTORY_CHANGE_FAILURE, SMSG_NOTIFICATION, other error packets |
| `prompt {kind, from}` (group invite, trade, duel, guild invite, ready check, role check, resurrect offer, summon, LFG/BG proposal) | SMSG_GROUP_INVITE, SMSG_TRADE_STATUS, SMSG_DUEL_REQUESTED, SMSG_GUILD_INVITE, MSG_RAID_READY_CHECK, SMSG_RESURRECT_REQUEST, SMSG_SUMMON_REQUEST, ... |
| `level_up`, `talent_point_gained`, `quest_complete`, `quest_failed`, `xp_gained`, `money_changed`, `item_received`, `skill_up` | SMSG_LEVELUP_INFO, SMSG_QUESTGIVER_QUEST_COMPLETE, SMSG_QUESTUPDATE_*, SMSG_LOG_XPGAIN, SMSG_ITEM_PUSH_RESULT, skill updates |
| `died {killer}`, `resurrected` | death / resurrect packets |
| `loot_roll {Item, roll_id}`, `loot_won {Item, by}` | SMSG_LOOT_START_ROLL, SMSG_LOOT_ROLL_WON |
| `mail_received`, `auction_sold {Item, money}`, `auction_expired` | SMSG_RECEIVED_MAIL, SMSG_AUCTION_OWNER_NOTIFICATION |
| `zone_changed`, `discovered_area` | SMSG_EXPLORATION_EXPERIENCE, zone updates |
| `path_failed {to}` (movement can't reach a destination) | playerbots movement result (feeds stuck detection) |
| `duel_started {from}`, `duel_ended {winner}` | SMSG_DUEL_* |
| `dbm_warning {text}` | our DBM-parity emulation (only with DBM in the loadout) |

## 9. Options and commands

**Options:** numbered actions computed in C++ from base + extensions, phrased as player
intents; each extension contributes its own (`objectives`: accept / turn in / go to an
objective or goal; `interaction`: buy / sell junk / repair / learn spells / craft / post an
auction; `death`: release / run to corpse / spirit healer / "Stuck"; `trade_loot`: need /
greed / pass; `character`: spend talent points; prompts: accept / decline; `vehicle`: its
abilities). Always includes `Keep doing what I'm doing`. The brain answers with a number.

**Commands** (not options): `set_goal {kind, params}` / `clear_goal {id}` (the bot's own
to-do list in `objectives`); `say {channel, text, to}` (chat replies are generated
separately from decisions).

## 10. Scene checklist (each becomes an e2e/eval check)

| Scene | Must be present |
|---|---|
| Kill quest | `objectives` quest with counts and `where`; mobs with `tapped_by_other`, `activity` |
| Use-item quest | quest item in `inventory` with its "Use:" tooltip line and `cooldown_s` |
| Timed / escort quest | `objectives.timer_s`; escorted unit (`activity: following/moving`, health) |
| Combat | `combat` (attackers, abilities with cooldowns, threat if Omen); `target` with its cast in `activity` |
| Caster pack | every caster's `activity.casting` in `nearby` with `interruptible` |
| Ground effect | `nearby.areas` entry with `inside_me` |
| Stunned / feared | `self.control` |
| Low health after a fight | `self.health`, food/water Items in `inventory`, option `Rest` |
| Death (world and in instance) | `death` (corpse, spirit healer, timer); in instance: `instance.entrance` |
| Town | NPC `services`; `inventory` (free slots, junk, durability); vendor / repair options |
| Trainer | `interaction.trainer` Spells with cost; `character.unspent talent points` after level 10 |
| Profession | `character.skills`; `interaction.tradeskill` recipes with reagents have/need; nodes only with skill |
| Auction house | `interaction.auction_house` prices; later `auction_sold` event and `self.new_mail` |
| Gear upgrade | looted Item with `gearscore_delta` and `upgrade` (GearScore loadout) |
| Self-set goal | `set_goal sell` at AH -> `objectives` goal with progress and `where` = auctioneer, also on `map` |
| Social prompts | `prompt` events + accept/decline options |
| Chat | `chat` events incl. `at_me`, `from_dist_yd`, channels, system, raid_warning |
| Red error text | `error` events |
| Party (5-man) | `party` members with `target`, `dispellable_by_me` for a healer, markers |
| Raid | `raid` grid, `encounter` (DBM timers only with DBM), `instance` |
| Loot roll | `trade_loot` roll + need/greed/pass options |
| Vehicle quest | `vehicle` with its abilities |
| Travel | `travel` (flight paths, transports, hearthstone); `self.in_flight` / `on_transport` |
| Orientation (any time) | `map` with position, zone `level_range`, objective locations, nearest town/graveyard |
| Wrong zone level | `map.zone.level_range` and bordering zones' ranges |
| Underwater | `environment.breath_s` |
| Stuck | `path_failed` event; `doing.since_s` without progress; `surroundings` (walkable directions, obstacles, goal) |
| Backstab / frontal cone | `relative_position.behind_it` / `facing_me` on the target |
| Pulling without Omen | `threat_status` on units (default UI indicator) |
| Fishing | the bobber GameObject with `state: splashing` |
| Herbalism | `map.tracked` blips with Find Herbs on |
| LFG "GS 5.5k?" | `gearscore` on players (GearScore); `interaction.inspect` |
| Guild raid night | `social.calendar` with my sign-up status; `social.guild.motd` |
| Daily quest | quest `daily: true`; `world.daily_reset_in_s` |
| Battleground | `battleground` score, objectives, flag carriers |
| Duel | `duel_started` / `duel_ended`; `combat.duel_opponent` |
| Reading a book | `interaction.item_text` |
| Events | `level_up`, `talent_point_gained`, `quest_complete`, `item_received`, `skill_up` |

## 11. Decisions

1. **Nameplates through walls:** included, flagged `in_los: false` (the real client shows
   nameplates through walls within range).
2. **Combat log:** summarized per snapshot in `combat`, not streamed hit by hit.
3. **Coordinates:** both raw `x,y,z` (for C++ geometry and evals) and relative `dir/dist_yd`.
4. **Knowledge gating:** the snapshot shows visible text as the client does ("Defile"); the
   knowledge gate applies when rendering the prompt and in the reflexes.
5. **Activation vs prompt:** the module attaches every extension whose rule holds; the
   harness decides what goes into the prompt.
6. **No version tiers:** the spec describes the complete perception; each phase plan picks
   which extensions to build first.
7. **Unit activity, not just casting:** every unit carries `activity` (what it's doing).
8. **Shared types** for Item / Spell / Aura / Unit / Place, with tooltips, cooldowns and
   GearScore everywhere they appear; defined once in the schema, inherited with `allOf`.
9. **Structured data, not screenshots:** bots have no client to render, images cost many more
   tokens, break prompt caching and get numbers wrong, and are hard to test. Exception: an
   optional synthetic top-down render in `surroundings` for spatial problems.
