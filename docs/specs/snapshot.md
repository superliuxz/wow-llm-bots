# Bot perception: the snapshot spec

Status: **agreed** (2026-11-14). Everything a bot's brain decides is based on this.

## 1. Principles

1. **Parity with a real client.** A bot perceives what a human player would get through the
   3.3.5a client plus the addons that bot "has installed". Nothing hidden, nothing extra.
2. **A base view plus extensions**, like the real UI: a base screen everyone has, and panels
   or addons that appear only in certain situations (quest log, party frames, raid grid,
   threat meter, boss timers...). **Conditional** extensions attach when their situation
   applies and disappear when it ends; **always-on** extensions (`map`, `world`) model
   things a player keeps glancing at. A snapshot only carries what matters right now.
3. **Built from the client's own information**, not server internals:
   - **Events** come from the packets the server sends the bot's session (AzerothCore's
     `ServerScript::OnPacketSent`, which fires for client-less sessions): chat, red error
     text, pop-up prompts, combat log.
   - **State** is read from the game once per second on the world thread, filtered to what
     the client knows: the bot's own state, and objects in the client's visibility set
     (`Player::m_clientGUIDs`).
4. **Describe, don't decide.** The snapshot reports what's on screen. The only "help" is the
   numbered **options**, which mirror what a player can click or do next.
5. **Testable.** Each scene in §7 is a check: "in scene X the snapshot must contain Y".

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
 "ext":{"quests":{...},"party":{...}},
 "events":[...],
 "options":[...]}
{"type":"event","bot":"Dezlu","seq":1235,"t":123456790,"event":{...}}
```
- `seq`: per-bot counter (gaps reveal drops); `t`: game time in ms.
- Each snapshot is **complete** (not a diff). `ext` holds only the active extensions.
  `events` repeats every event since the previous snapshot (they were also sent live).
- Size budget ~1-3 KB typical, hard cap 16 KB; under pressure the harness/module drops the
  lowest-priority extensions and truncates lists by salience (§6).

## 3. Base view (always present)

What's on screen with nothing open.

| Block | Fields | Client equivalent |
|---|---|---|
| `self` | name, race, class, gender, level; `xp`, `xp_next`, `rested_xp`; `health`/`health_max`; `power_type`, `power`/`power_max`; `state` (alive / dead at corpse / ghost); `in_combat`, `resting`, `mounted`, `swimming`; `casting {spell, remaining_ms}`; `auras` on self `{name, remaining_s, stacks, harmful}`; `money` (copper) | player unit frame, XP bar, buff bar, cast bar |
| `where` | `map`, `zone`, `subzone`, `x,y,z`, `facing`, `indoors`, `in_instance` | minimap, zone text |
| `doing` | `{activity, since_s, detail}` (do_quest + title, grind + area, rest, travel...) | the player's own intent |
| `nearby` | units and objects in the client's visibility set, sorted by salience, capped (~20 units, ~10 objects). Units: `name, kind (npc/player/pet), level, level_color, reaction, classification (normal/elite/rare/boss), health_pct, dist_yd, dir (8-way + above/below), in_los, targeting_me, tapped_by_other, quest_marker (!, ?, gray !), services (vendor, class trainer, repair, innkeeper, flight master, banker, auctioneer, spirit healer...), dead, lootable`. Players add `class, in_my_group`. Objects: `name, kind (quest object, herb, ore, chest, mailbox, door...), dist_yd, dir, usable` | nameplates, tooltips, highlighted objects |

Everything else is an extension.

## 4. Extensions

Each extension has an **activation rule**, a **content** list, a **source**, and the
**addon** it mirrors (if any). An extension that mirrors an addon (or the addon-provided
part of one) is only present if the bot has that addon in its **loadout** (§5).

### Always-on

| Extension | Content | Mirrors |
|---|---|---|
| `map` | The spatial overview a player gets from glancing at the map: current zone and the bot's position in it; **quest objective areas** for tracked quests (`{quest, area_name, dir, dist_yd}`; native quest POI blobs since patch 3.3); nearby **places**: towns/camps, flight masters (known or not), innkeeper, graveyard, dungeon entrances, with dir/dist; party members and corpse markers on the map; zones bordering this one and their level ranges (from the client's map data). With **Questie**: quest givers with available quests and mob/item spawn areas for objectives | world map + minimap (+ **Questie** map pins) |
| `world` | `game_time` (day/night), `session_minutes`, zone PvP status | clock, session |

### Conditional

| Extension | Active when | Content | Mirrors |
|---|---|---|---|
| `quests` ("missions") | quest log not empty | each quest: `title, level, objectives [{text, have, need, done}], complete, failed`; `nearest_objective {dir, dist_yd, area}` (where it is on the map: see `map`) | quest log + tracker |
| `target` | has a target (and `focus` if set) | `name, level, reaction, classification, health_pct, casting {spell, remaining_ms, interruptible}, auras (mine on it), its target, raid marker` | target / focus frames |
| `combat` | in combat | attackers (who, `targeting_me`), my threat on each (**Omen**), my usable abilities + cooldowns + range, resources (combo points, runes...), recent combat summary (hits taken/dealt, incoming casts) | action bars, cast bars, **Omen**, scrolling combat text |
| `party` | in a party (not raid) | leader; members `name, class, level, role, health_pct, power_pct, dist_yd, in_los, dead, offline`; per member `auras` incl. **dispellable_by_me** debuffs, missing buffs I can give; loot method; raid markers | party frames (+ **Decursive**-like dispel highlights) |
| `raid` ("grid") | in a raid | compact grid by subgroup: `name, class, role, health_deficit, dead, in_range/in_los, aggro, dispellable_by_me`; raid markers; main tanks; raid warnings | **Grid / VuhDo** raid frames |
| `encounter` | engaged with a boss | boss `health_pct`, `phase`, **DBM** timers and warnings (`{name, remaining_s}`); boss emotes/yells | **DBM** |
| `death` | dead or ghost | `corpse {dir, dist_yd}`, `can_resurrect_in_s`, `spirit_healer {dir, dist_yd}`, pending resurrect offers | death UI, corpse arrow |
| `interaction` | a window is open (talking to an NPC/object) | merchant (items, prices, buyback), trainer (spells I can learn, cost), quest giver dialog (quest text, rewards), flight map (known destinations, cost), bank, mail, auction house | the open window |
| `inventory` | bags nearly full, just looted, low durability, or in town | `free_slots/total`, junk items + value, consumables (food/water/potions), quest items, `durability_pct` + worst item, upgrades found (**GearScore** delta), hearthstone ready + bind location | bags, character pane, **GearScore** |
| `trade_loot` | trade window, loot window or loot roll open | trade contents both sides; lootable items; roll `{item, roll_id, time_left}` | trade / loot / roll frames |
| `travel` | travelling or about to (no quest target nearby) | known flight paths, nearest flight master, hearthstone, mount available | world map, flight map |
| `pet` | has a pet / totems / minion | pet `name, health_pct, happiness (hunter), abilities on autocast`; shaman totems active + timers | pet frame, totem bar |
| `social` | idle in a city/town, or a social event | friends online, guild online count + recent guild chat, LFG channel listings ("LF2M Deadmines") | friends list, guild panel, chat channels |
| `environment` | breath, fatigue or falling active | `breath_s`, `fatigue_s`, `falling {height_yd}` | mirror timers |

### Role and class flavors
Same extensions, different focus, like a player's UI layout:
- **Healer:** `party` / `raid` list health deficits first and mark `dispellable_by_me`
  using the bot's class (priest: magic + disease; paladin: magic + poison + disease;
  shaman: poison + disease, curse with Cleanse Spirit; druid: curse + poison; mage: curse).
- **Tank:** `combat` shows threat on every nearby enemy, loose mobs, who they target.
- **DPS:** `target` and `combat` cooldowns first; marker priority (skull, cross).
- Class extras: rogue/druid combo points, DK runes, hunter pet happiness and ammo,
  warlock soul shards, shaman totems.

## 5. Addon loadout (per bot)

Each bot has an addon list in its memory (`var/bots/<name>/addons.md`), chosen by its
personality and experience, like real players: a veteran raider runs DBM, Omen, Grid and
GearScore; a new player has the default UI plus maybe Questie. Extensions that mirror an
addon (Questie, Omen, Grid/VuhDo, Decursive, DBM, GearScore, Skada) are only produced if the
bot has it. Bots can install addons later ("someone told me to get DBM"), which is part of
learning. Base view and non-addon extensions (quest log, party frames, target frame...) are
the default UI and always available.

## 6. Salience and budget

Priority when the snapshot must shrink (lowest dropped first):
`world` < `social` < `travel` < `map` (trimmed to the nearest places first) < `inventory` <
`pet` < `quests` < `interaction` <
`trade_loot` < `party`/`raid` < `target` < `death` < `combat` < `encounter` < base.
Within lists: hostile-and-targeting-me > quest-related > interactable > other; nearest first.

## 7. Events (from packets to the bot's session)

| Event | Source packet(s) |
|---|---|
| `chat {channel, from, text}` (say/yell/whisper/party/raid/guild/general/trade/lfg/emote, monster say/yell) | SMSG_MESSAGECHAT (+ GM variant) |
| `error {text}` (red UI text: "Target not in line of sight", "Inventory is full"...) | SMSG_CAST_FAILED, SMSG_INVENTORY_CHANGE_FAILURE, SMSG_NOTIFICATION, other error packets |
| `prompt {kind, from}` (group invite, trade, duel, guild invite, ready check, resurrect offer, summon) | SMSG_GROUP_INVITE, SMSG_TRADE_STATUS, SMSG_DUEL_REQUESTED, SMSG_GUILD_INVITE, MSG_RAID_READY_CHECK, SMSG_RESURRECT_REQUEST, SMSG_SUMMON_REQUEST |
| `level_up`, `quest_complete`, `quest_failed`, `xp_gained`, `money_changed`, `item_received` | SMSG_LEVELUP_INFO, SMSG_QUESTGIVER_QUEST_COMPLETE, SMSG_QUESTUPDATE_*, SMSG_LOG_XPGAIN, SMSG_ITEM_PUSH_RESULT |
| `died {killer}`, `resurrected` | death / resurrect packets |
| `loot_roll {item, roll_id}` | SMSG_LOOT_START_ROLL |
| `zone_changed`, `discovered_area` | SMSG_EXPLORATION_EXPERIENCE, zone updates |
| `dbm_warning {text}` | our DBM-parity emulation (only with DBM in the loadout) |

## 8. Options

Numbered actions computed in C++ from base + extensions, phrased as player intents; each
extension contributes its own (`quests`: accept / turn in / go to objective; `interaction`:
buy / sell junk / repair / learn spells; `death`: release / run to corpse / spirit healer /
"Stuck"; `trade_loot`: need / greed / pass; prompts: accept / decline). Always includes
`Keep doing what I'm doing`. The brain answers with a number; chat replies are generated
separately.

## 9. Scene checklist (each becomes an e2e/eval check)

| Scene | Must be present |
|---|---|
| Kill quest | `quests` with counts and `nearest_objective`; mobs with `tapped_by_other` |
| Combat | `combat` (attackers, cooldowns, threat if Omen), `target` incl. its cast |
| Low health after a fight | `self.health`, `inventory.consumables`, option `Rest` |
| Death | `death` (corpse dir/dist, spirit healer, timer) + death options |
| Town | NPC `services` in `nearby`; `inventory` (free slots, junk, durability); vendor/repair options |
| Talking to a trainer | `interaction.trainer` with learnable spells and costs |
| Social prompts | `prompt` events + accept/decline options |
| Channels | `chat` events for general/trade/lfg/guild |
| Red error text | `error` events |
| Party (5-man) | `party` with health/power, dispellable debuffs for a healer, markers |
| Raid | `raid` grid, `encounter` with DBM timers only if DBM is in the loadout |
| Loot roll | `trade_loot` roll + need/greed/pass options |
| Travel | `travel` (flight paths, hearthstone); `map` places with flight masters and bordering zones |
| Orientation (any time) | `map` with the bot's position, objective areas for tracked quests, nearest town and graveyard |
| Underwater | `environment.breath_s` |
| Events | `level_up`, `quest_complete`, `item_received` |

## 10. Decisions

1. **Nameplates through walls:** included, flagged `in_los: false` (the real client shows
   nameplates through walls within range).
2. **Combat log:** summarized per snapshot in `combat`, not streamed hit by hit.
3. **Coordinates:** both raw `x,y,z` (for C++ geometry and evals) and relative `dir/dist_yd`
   (for prompts).
4. **Knowledge gating:** the snapshot shows visible text as the client does ("Defile"); the
   knowledge gate is applied when rendering the prompt and in the reflexes.
5. **Activation vs prompt:** the module attaches every extension whose rule holds; the
   harness decides what goes into the prompt (tunable without server rebuilds).
6. **No version tiers:** the spec describes the complete perception; each phase plan picks
   which extensions to build first.
