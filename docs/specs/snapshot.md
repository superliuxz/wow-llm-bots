# Bot perception: the snapshot spec

Status: **agreed, revision 7** (2026-11-15). The exact field-level contract is
[`server_module/mod-llm-brain/proto/snapshot.proto`](../../server_module/mod-llm-brain/proto/snapshot.proto) (protobuf Edition 2024, lite runtime; the single source of
truth for the module <-> harness protocol; field annotations in
[`snapshot_options.proto`](../../server_module/mod-llm-brain/proto/snapshot_options.proto)); examples in [`examples/`](examples/) use the canonical protobuf JSON
mapping and are checked by `tools/validate-snapshots.sh` (strict parse + visibility rules).
Coverage against the client: [`coverage-ui.md`](coverage-ui.md) (every UI frame) and
[`coverage-opcodes.md`](coverage-opcodes.md) (every server opcode). This document explains it. Everything a bot's brain decides is based on this.

## 1. Principles

1. **Definition: everything a user could possibly see in the WoW client, nothing more.**
   A bot perceives what a human player could get through the 3.3.5a client plus the addons
   that bot "has installed". The test for any field: *is there a place in the client (or one
   of this bot's addons) where a human could see this?* If yes, it belongs here, in the
   extension for that UI place; if no, it doesn't, however useful it would be. The one
   deliberate stand-in is `surroundings`, which approximates the 3D view itself.
2. **Three visibility levels**, like a real player's screen. Every field is tagged
   (the `visibility` field option in the proto; untagged fields are glance):
   - **glance**: on screen without doing anything (unit frames, nameplates, minimap, quest
     tracker, action bars, buff icons, chat, red error text, flashing UI cues, and the frames
     that appear in a situation: target, party, raid grid, boss timers, an open vendor...);
   - **open**: inside a panel the bot must open (bags, world map, quest log, character,
     spellbook, talents, reputation, skills, social, calendar);
   - **hover**: inside a tooltip of something the bot points at (item stats, spell and buff
     descriptions, a player's guild, a zone's level range).
   Glance content is always sent; open panels only while open; tooltips only while hovered
   (§9 `open` / `close` / `hover`). So a bot has to *notice* full bags or *hover* a drop to
   see whether it's an upgrade, like a person; and prompts stay small.
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
- The harness keeps the **latest snapshot per bot** in memory, plus **remembered views**: the
  last-seen contents of panels the bot has since closed, tooltips it hovered, and a rolling
  chat history, each with its age ("bags, seen 40 s ago"). These are rendered into prompts as
  memory, not as live state; the snapshot itself is only what's on screen now.
- On disk: the snapshot behind each **decision** (with prompt, reply, choice, latency) in
  `var/bots/<name>/events/<date>.jsonl`; optional full recording to `out/recordings/`
  (debug only; ~8-9 GB/day at 50 bots).

Messages (`snapshot.proto`): the module sends `ModuleMessage` (a `Snapshot`, or an
`ImmediateEvent` between snapshots); the harness sends `Command` (choose an option,
`set_goal`, `say`, `open`, `hover`, ...). In JSON:
```json
{"snapshot": {"bot":"Dezlu","seq":1234,"time_ms":"1794729600000",
 "base":{...},
 "ext":{"minimap":{...},"quest_tracker":[...],"action_bars":{...},"world":{...},"party":{...},"bags":{...}},
 "events":[{"chat":{...}}, {"xp_gained":{...}}],
 "options":[...]}}
{"event": {"bot":"Dezlu","seq":1235,"time_ms":"1794729600350","event":{"prompt":{...}}}}
```
- `seq`: per-bot counter (gaps reveal drops); `time_ms`: server time.
- Wire format: **binary protobuf, length-prefixed frames** over the Unix socket. Both sides
  use C++ generated from `snapshot.proto` with `optimize_for = LITE_RUNTIME`, as Chromium
  does; the module contains no JSON code.
- JSON (the canonical protobuf mapping, shown above) exists only in tooling: the harness
  converts frames with `util::BinaryToJsonString` and a TypeResolver built from the compiled
  descriptor set when it writes recordings, decision logs and eval cases, and the reverse for
  `harness decide <snapshot.json>`.
- Proto3 JSON omits default values (false, 0, empty), so absence means false / zero.
- Each snapshot is **complete** (not a diff). `ext` holds only the active extensions.
  `events` repeats every event since the previous snapshot (they were also sent live).
- No caps on content: the module sends everything the client could show (§4 `nearby`, §7).
  Typical sizes: a few KB in the open world, up to ~50 KB in a busy city. The only hard limit
  is the bridge's 1 MiB frame (the module drops the least salient `nearby` entries if a
  snapshot would ever exceed it). Choosing what the bot notices is the harness's job.

## 3. Types

Every concept is one message, reused everywhere it appears. Protobuf has no inheritance, so
shared types use **composition**: the common fields live in the base message and the
kind-specific ones in an optional part.

```
Relative {dist_yd, dir, height, in_los, position, behind_it, facing_me}
Unit {guid, name, rel (Relative), kind, level, level_color, reaction, classification,
      health_pct, power_pct, dead, lootable, skinnable, tapped_by_other, targeting_me, target,
      threat_status, quest_marker, pvp_flagged, activity, auras, raid_marker, pack,
      visual_state, quest_relevant (Questie), subtitle, services (hover)}
  + player  PlayerPart {class, title, in_my_group, is_friend, is_ignored, afk, dnd,
                        race / guild / gearscore (hover)}
  + group   GroupPart {role, online, health_deficit, in_range, aggro, subgroup, leader, ready}
  + boss    BossPart {phase (DBM)}
  + pet     PetPart {happiness, abilities, stance, owner}
  + vehicle VehiclePart {seats, abilities}
Self {unit (Unit with player), xp, health/power, alt_mana (druid forms), state, combat /
      resting / camping / mounted / flight / transport, control, casting, money, indicators,
      gcd, auto_attack, in_cinematic, drunk, weapon_enchants, runes, form}
GameObject {guid, name, rel, kind, usable, state, highlighted, required_skill}
Area {guid, name, rel, spell, radius_yd, inside_me, hostile, remaining_s}   (ground effects)
Place {name, rel, kind, known, quest, level_range (hover)}
Item {id, name, quality, count, kind, bag_slot, usable, cooldown_s, charges, starts_quest,
      price / buyout / seller / time_left (where shown); item_level, required_level,
      binding, unique, vendor_value, tooltip (hover)}
  + equipment EquipmentPart {slot, durability_pct; stats, sockets, enchant, type,
                             gearscore (hover)}
Spell {id, name, rank, cooldown_s; school, cost, cast_time_ms, range_yd, tooltip (hover)}
  + ability AbilityState {cooldown_remaining_s, usable, in_range, known, charges, slot}
  + trainer TrainerSpell {price, required_level, learnable, learned, requires}
Aura {id, name, harmful, remaining_s, stacks, dispel_type, dispellable_by_me, caster}
Objective {title, progress {have, need}, done, where}
  + one of: quest QuestPart | goal GoalPart | achievement AchievementPart
Achievement {id, name, description, points, category, earned, criteria, reward}
Activity {state, cast, with, text};  Cast {spell, remaining_ms, interruptible, target, channeled}
Event: one of chat | error | info | prompt | level_up | xp_gained | ... (§8)
```

## 4. Base view (glance, always present)

| Block | Fields | Client equivalent |
|---|---|---|
| `self` | **Self** (incl. `gcd_remaining_ms` from the GCD sweep, `auto_attack` melee/ranged, `in_cinematic`), incl. `indicators`: talent button flashing (unspent points), the damaged-armor figure (damaged/broken slots), calendar icon flashing, mail icon, free bag slots on the backpack | unit frame, buff bar, cast bar, main-screen cues |
| `where` | `map`, `zone`, `subzone`, `position`, `facing`, `indoors`, `in_instance` | zone text |
| `doing` | `{activity, since_s, detail}` | the player's own intent |
| `nearby` | **units** (Unit/Player), **objects** (GameObject), **areas** (Area): everything in the client's visibility set (~100 yd, more in instances), **no caps** (the harness decides what the bot notices, §7). Detail follows the client (`detail`): **within name/nameplate range** (a per-bot UI setting, §6) the nameplate-level fields (name, level, health, hostility, elite marker, quest marker, activity incl. patrolling, threat indicator, raid marker, through walls with `in_los: false`); **beyond it** only what the model shows: `looks_like` ("boar", "troll in plate"), `size`, `activity`, `dead`, `visual_state`, `pack`, raid marker, position; name/level/health only when targeted or hovered. Beyond that range a unit **not in line of sight is left out** (a player can't see it). Plus what a player sees of the model: **`pack`** (mobs standing together: pull one, get the pack), **`visual_state`** (sheep, stun stars, ice block, sleeping, banished...), Questie **`quest_relevant`** icon; guild/race/services etc. on hover. Objects show `highlighted` (quest sparkle); items `starts_quest` | nameplates, unit models, highlighted objects, ground effects |
| `ui` | `open_panels`, `hovering` | what the player has open / points at |
| `tooltips` | one **Tooltipped** per hovered thing: tooltip lines + the full typed object (Item with stats and GearScore, Spell, Aura, Player with guild, Place with level range) + Questie line | tooltips |

## 5. Extensions

An extension that mirrors an addon (or the addon-provided part of one) is only present if
the bot has that addon in its **loadout** (§6).

### Glance: always on screen

| Extension | Content | Mirrors |
|---|---|---|
| `minimap` | zone, subzone, **clock**; places within minimap range; quest **objective areas** (native since 3.3); **tracked** blips (Find Herbs, Track Beasts...); party dots; corpse arrow | minimap |
| `quest_tracker` | watched quests, up to the client's tracking limit (QuestObjective, with the **quest item button** when the quest has a usable item: progress, `where`, `daily`, `timer_s`, `escort`) **plus the bot's own goals** (Goal, set with `set_goal`: sell / buy / train / repair / learn_profession / reach_level / travel_to / meet / farm; progress and `where` filled in by the module). Goals are the bot's own to-do list, always in mind | quest tracker + the player's to-do list |
| `action_bars` | Ability per button: `cooldown_remaining_s`, `usable`, `in_range` (vs target), charges; tooltips on hover | action bars |
| `world` | `session_minutes`, `daily_reset_in_s`, zone PvP status, pending server messages | session, server notices |

### Glance: situational frames (on screen while the situation lasts)

| Extension | Active when | Content | Mirrors |
|---|---|---|---|
| `target` | has a target (and `focus`) | Unit with auras (debuff borders show dispel type) and `activity`, raid marker, **target-of-target** | target / focus / ToT frames |
| `combat` | in combat | attackers, duel opponent, my **threat** per enemy (**Omen**), resources (combo points, runes), recent combat summary | **Omen**, combat text |
| `party` | in a party | leader; GroupMembers (health/power, `target`, debuffs incl. `dispellable_by_me`); missing buffs I can give; loot method; markers | party frames (+ **Decursive**) |
| `raid` ("grid") | in a raid | compact grid: health deficits, aggro, range/LoS, `dispellable_by_me`, targets; main tanks; markers | **Grid / VuhDo** |
| `encounter` | engaged with a boss, or a boss-mod timer is running (DBM pull countdown) | Boss (`health_pct`, `phase`, `activity`), **DBM** timers and warnings | **DBM** |
| `instance` | in a dungeon/raid | name, difficulty, size, bosses killed, lockout, entrance | instance info |
| `death` | dead or ghost | corpse, resurrect timer, spirit healer, resurrect offers | death UI |
| `vehicle` | in a vehicle | Vehicle with its abilities and seats | vehicle UI |
| `interaction` | a window is open | merchant, trainer, quest dialog, gossip, flight map, bank, mail, auction house, tradeskill, inspect, item text, raid browser (Items/Spells inside carry hover-level details only when hovered) | the open window |
| `trade_loot` | trade / loot / roll open | trade both sides, loot, roll | trade / loot / roll frames |
| `travel` | travelling | known flight paths, nearest flight master, mount, transports | flight map, schedules |
| `pet` | has a pet / totems | Pet (happiness, autocast), totems | pet frame, totem bar |
| `environment` | breath / fatigue / falling | timers, fall height | mirror timers |
| `battleground` | in a battleground | score, objectives, flag carriers, time | battleground UI |
| `possessed` | possessing a unit (Eye of Acherus, Mind Control) | the possessed Unit with its abilities; `nearby` is seen from it | possess bar |
| `arena` | in an arena match | gates timer, both teams and ratings, enemy frames (class, health, power, casting, auras) | arena enemy frames |
| `surroundings` | stuck, `path_failed`, tight terrain | walkable directions, obstacles, goal direction, description, optional synthetic top-down image | the 3D view |

### Open panels (only while the bot has them open)

| Panel | Content | Auto-opens with |
|---|---|---|
| `bags` | every Item (bag slot, count, quality, kind, usable, cooldown); free/total slots; hearthstone | merchant, bank, mail, trade, auction house |
| `map` | world map: Places (towns, flight masters, graveyards, dungeons, zone exits), bordering zones, objective areas, **Questie** pins; zone level ranges on hover; **inside an instance: the dungeon map** (floors, areas) | - |
| `character` | equipped Equipment (durability, stats), total GearScore, character stats | - |
| `quest_log` | all quests with description, objectives, rewards | quest giver dialog |
| `spellbook` | all known Abilities | trainer |
| `talents` | unspent points, points per tree, talents | - |
| `skills` | professions, secondary skills, riding, weapon skills | tradeskill window |
| `reputation` | factions and standing | - |
| `social` | friends online, guild (name, rank, MOTD, online), LFG listings | - |
| `calendar` | events and my sign-up status (guild raid nights) | - |
| `achievements` | points, recent, the open category with criteria | - |
| `pvp` | honor, arena points, kills, arena teams | battlemaster |
| `companions` | mounts and vanity pets | - |
| `scoreboard` | battleground / arena score: per player killing blows, kills, deaths, honor, damage, healing, objective columns, rating change | - |
| `dungeon_finder` | random dungeon and its reward (done today?), dungeon list with lock reasons, my roles, queue (wait, tanks/healers/dps still needed) | - |

### Role and class flavors
Same extensions, different focus, like a player's UI layout:
- **Healer:** `party`/`raid` list health deficits first and mark `dispellable_by_me` from the
  bot's class (priest: magic + disease; paladin: magic + poison + disease; shaman: poison +
  disease, curse with Cleanse Spirit; druid: curse + poison; mage: curse).
- **Tank:** `combat` threat on every nearby enemy, loose mobs, who they target.
- **DPS:** `target` and `combat` cooldowns first; marker priority (skull, cross).
- Class extras in `combat`/`pet`: combo points, runes, pet happiness, ammo, soul shards, totems.

## 6. Addon loadout and UI settings (per bot)

Each bot has UI settings, like a player's Interface options: **nameplates** on/off and their
distance (20 yd default, up to 41), and **floating names** (players / NPCs). They decide how far
`nearby` shows names and nameplate fields (§4). A new player often has the defaults; a
veteran turns enemy nameplates on.

Each bot also has an addon list in its memory (`var/bots/<name>/addons.md`), chosen by its
personality and experience: a veteran raider runs DBM, Omen, Grid and GearScore; a new player
has the default UI plus maybe Questie. Addon-backed content (Questie pins, Omen threat,
Grid/VuhDo, Decursive highlights, DBM timers, GearScore values, Skada meters) is only
produced for bots that have the addon. Bots can install addons later ("someone told me to
get DBM"). The default UI parts (quest log + native quest POI, party frames, target frame,
tooltips...) are always available.

## 7. Salience and attention (harness)

The module reports everything the client could show; it does not choose. Choosing what this
bot **notices** is the harness's attention step, part of the variance layers (design.md):
a distracted new player notices less than a veteran. When rendering a prompt the harness
ranks `nearby` by salience: hostile-and-targeting-me > casting-at-me > quest-related >
interactable > other; nearest first; and fits the prompt budget in this order (dropped
first): `world` < open panels (longest first) < `travel` < `minimap` places < `instance` <
`battleground` < `interaction` < `trade_loot` < `vehicle` < `surroundings` < `quest_tracker`
< `party`/`raid` < `target` < `action_bars` < `death` < `combat` < `encounter` < base.

**Line of sight** costs a ray cast against the map geometry, so the module checks it on a
budget: a fixed number of casts per bot per tick, nearest units first, each result cached for
a few seconds. Far units' `in_los` can be a few seconds old.

## 8. Events (from packets to the bot's session)

| Event | Source packet(s) |
|---|---|
| `chat {channel, from, text, at_me, from_dist_yd}` (say / yell / whisper / party / raid / raid_warning / guild / officer / general / trade / lfg / emote / monster say-yell-emote / system) | SMSG_MESSAGECHAT (+ GM variant), SMSG_TEXT_EMOTE, SMSG_NOTIFICATION, server messages |
| `error {text}` (red UI text) | SMSG_CAST_FAILED, SMSG_INVENTORY_CHANGE_FAILURE, SMSG_NOTIFICATION, SMSG_TRANSFER_ABORTED / SMSG_LFG_TELEPORT_DENIED (a teleport failed), other error packets |
| `prompt {kind, from}` (group invite, trade, duel, guild invite, ready check, role check, resurrect offer, summon, quest share, battleground ready, Dungeon Finder proposal / boot vote / "find another player?", bind hearthstone, battleground spirit healer wave, reclaim corpse, chat channel invite / password, name your pet, every confirmation popup) | SMSG_GROUP_INVITE, SMSG_TRADE_STATUS, SMSG_DUEL_REQUESTED, SMSG_GUILD_INVITE, MSG_RAID_READY_CHECK, SMSG_RESURRECT_REQUEST, SMSG_SUMMON_REQUEST, ... |
| `level_up`, `talent_point_gained`, `quest_complete`, `quest_failed`, `xp_gained`, `money_changed`, `item_received`, `skill_up` | SMSG_LEVELUP_INFO, SMSG_QUESTGIVER_QUEST_COMPLETE, SMSG_QUESTUPDATE_*, SMSG_LOG_XPGAIN, SMSG_ITEM_PUSH_RESULT, skill updates |
| `died {killer}`, `resurrected` | death / resurrect packets |
| `loot_roll {Item, roll_id}`, `loot_won {Item, by}` | SMSG_LOOT_START_ROLL, SMSG_LOOT_ROLL_WON |
| `mail_received`, `auction_sold {Item, money}`, `auction_expired` | SMSG_RECEIVED_MAIL, SMSG_AUCTION_OWNER_NOTIFICATION |
| `zone_changed`, `discovered_area`, `instance_entered`, `instance_reset` | SMSG_EXPLORATION_EXPERIENCE, zone updates, instance messages |
| `cinematic_started`, `cinematic_ended` | SMSG_TRIGGER_CINEMATIC / movie packets |
| `path_failed {to}` (movement can't reach a destination) | playerbots movement result (feeds stuck detection) |
| `duel_started {from}`, `duel_ended {winner}` | SMSG_DUEL_* |
| `dbm_warning {text}` | our DBM-parity emulation (only with DBM in the loadout) |
| `minimap_ping {from, where}` | MSG_MINIMAP_PING |
| `calendar_alarm {title, starts_in_s}` | calendar event reminders |
| `displaced {how, by, spell, distance_yd}` (knocked back, pulled, teleported on the same map) | MSG_MOVE_KNOCK_BACK, MSG_MOVE_TELEPORT, forced movement (no client event: the screen jumps) |

Every client event is accounted for in `client-events.tsv` (`coverage-client-events.md`), and
what wakes the brain is in `triggers.md`.

## 9. Options and commands

**Options:** numbered actions computed in C++ from base + extensions, phrased as player
intents; each extension contributes its own (`quest_tracker`: accept / turn in / go to an
objective or goal; `interaction`: buy / sell junk / repair / learn spells / craft / post an
auction; `death`: release / run to corpse / spirit healer / "Stuck"; `trade_loot`: need /
greed / pass; `indicators.talent_points_unspent` / `talents`: spend talent points; prompts: accept / decline; `vehicle`: its
abilities). Always includes `Keep doing what I'm doing`. The brain answers with a number.

**Blanks** (things a player types into the client): an option may contain blanks, e.g.
`Buy a guild charter named ___`, `Post Frozen Orb, buyout ___`, `Mail ___ to ___`. Each
blank has a kind (free text, new name, existing player, money, count), the client's label
and limits. The brain answers with the number plus one fill-in per blank (`4 "Totem Pole"`),
generated like chat. The module checks each fill-in with the client's rules (name rules,
length, bounds, enough money); if one is invalid nothing happens and the bot gets the
client's `error` event, as a player would.

**Commands** (not options):
- `open {panel}` / `close {panel}`: open or close a panel (§5). Panels also auto-open with
  their windows, and close on their own when the window closes, as in the client.
- `hover {kind, id/name}`: point at an item, spell, aura, unit, place or quest; its tooltip
  is in `tooltips` for the next few snapshots.
- `ah_search {query}`: search the auction house (results appear in `interaction`).

**Habits:** routine looking is not a brain decision. Each bot has habits (from its
personality, in `var/bots/<name>/habits.md`) that the harness runs automatically, without an
LLM call, e.g. hover new loot, glance at the map on entering a zone, open bags at a vendor,
close bags when combat starts, check the calendar on login. The brain can still open and
hover deliberately.
- `set_goal {goal_kind, params}` / `clear_goal {id}`: the bot's own to-do list in
  `quest_tracker`.
- `say {channel, text, to}`: chat (generated separately from decisions).

Chat and prompts from players on the bot's ignore list are dropped, as the client does.

The module still uses all game data internally to compute options and goal progress (a
real player knows their junk and bag space from habit), but only sends what's visible.

## 10. Scene checklist (each becomes an e2e/eval check)

| Scene | Must be present |
|---|---|
| Kill quest | `quest_tracker` quest with counts and `where`; mobs with `tapped_by_other`, `activity` |
| Use-item quest | quest item (open `bags`) and, when hovered, its "Use:" tooltip line; `action_bars`/item `cooldown_s` |
| Timed / escort quest | `quest_tracker` `timer_s`; escorted unit (`activity: following/moving`, health) |
| Combat | `combat` (attackers, threat if Omen), `action_bars` cooldowns; `target` with its cast in `activity` |
| Caster pack | every caster's `activity.casting` in `nearby` with `interruptible` |
| Ground effect | `nearby.areas` entry with `inside_me` |
| Stunned / feared | `self.control` |
| Low health after a fight | `self.health`, food/water in `action_bars` or open `bags`, option `Rest` |
| Death (world and in instance) | `death` (corpse, spirit healer, timer); in instance: `instance.entrance` |
| Town | NPC services (hover or the open window); `indicators.bag_free_slots`, `durability_warning`; vendor / repair options; merchant auto-opens `bags` |
| Trainer | `interaction.trainer` Spells with cost; `indicators.talent_points_unspent` after level 10 |
| Profession | `skills` (open); `interaction.tradeskill` recipes with reagents have/need; nodes only with skill |
| Auction house | `interaction.auction_house` prices; later `auction_sold` event and `indicators.new_mail` |
| Gear upgrade | hovering the looted Item gives its tooltip with stats, `gearscore_delta`, `upgrade` (GearScore loadout) |
| Self-set goal | `set_goal sell` at AH -> `quest_tracker` goal with progress and `where` = auctioneer |
| Social prompts | `prompt` events + accept/decline options |
| Chat | `chat` events incl. `at_me`, `from_dist_yd`, channels, system, raid_warning |
| Red error text | `error` events |
| Party (5-man) | `party` members with `target`, `dispellable_by_me` for a healer, markers |
| Raid | `raid` grid, `encounter` (DBM timers only with DBM), `instance` |
| Loot roll | `trade_loot` roll + need/greed/pass options |
| Vehicle quest | `vehicle` with its abilities |
| Travel | `travel` (flight paths, transports, hearthstone); `self.in_flight` / `on_transport` |
| Orientation (any time) | `minimap` (nearby places, objective areas, clock); open `map` for distant places |
| Wrong zone level | open `map`, hover the zone: `level_range`; bordering zones |
| Underwater | `environment.breath_s` |
| Stuck | `path_failed` event; `doing.since_s` without progress; `surroundings` (walkable directions, obstacles, goal) |
| Backstab / frontal cone | `relative_position.behind_it` / `facing_me` on the target |
| Pulling without Omen | `threat_status` on units (default UI indicator) |
| Fishing | the bobber GameObject with `state: splashing` |
| Herbalism | `minimap.tracked` blips with Find Herbs on |
| LFG "GS 5.5k?" | hover a player: `gearscore` (GearScore); `interaction.inspect` |
| Guild raid night | `indicators.calendar_invite_pending`; open `calendar` for sign-up; guild MOTD in `social` |
| Daily quest | quest `daily: true`; `world.daily_reset_in_s` |
| Battleground | `battleground` score, objectives, flag carriers |
| Duel | `duel_started` / `duel_ended`; `combat.duel_opponent` |
| Reading a book | `interaction.item_text` |
| **Combat:** pulling | `pack` on mobs (pull one, get the pack), `activity: patrolling` on patrols, level color, elite marker |
| **Combat:** crowd control | `visual_state` on CC'd mobs (sheep, sap, shackle) without targeting them; don't break it |
| **Combat:** rotation pacing | `gcd_remaining_ms`, `auto_attack`, `action_bars` cooldowns, combo points/rage |
| **Combat:** interrupt | caster's `activity.cast.interruptible` + my interrupt's cooldown and `in_range` |
| **Questing:** find quest mobs | Questie `quest_relevant` icon on mobs; objective areas on the minimap |
| **Questing:** collect objects | `highlighted` GameObjects (sparkle) |
| **Questing:** use quest item | `quest_tracker` item button with cooldown/range |
| **Questing:** drop starts a quest | `starts_quest` Item in loot / bags |
| **Questing:** party shares a quest | `prompt quest_share` + accept/decline |
| **Questing:** cutscene | `in_cinematic` (can't act), `cinematic_*` events |
| **Dungeon:** tanking | threat per mob (Omen) or `threat_status`; mob `target` (who it's hitting); party member targets; target-of-target |
| **Dungeon:** healing | party health/power, debuffs with `dispellable_by_me`, `in_range`, mobs targeting the healer |
| **Dungeon:** navigating | open `map`: dungeon map with floors and areas; `instance` bosses killed |
| **Dungeon:** wipe | `death` + `instance.entrance`; `instance_reset` system message |
| Events | `level_up`, `talent_point_gained`, `quest_complete`, `item_received`, `skill_up` |
| Bags fill up | `error` "Inventory is full" and `indicators.bag_free_slots` 0, without opening bags |
| Damaged gear | `indicators.durability_warning` (armor figure); exact values in open `character` |
| Out-of-combat cooldowns | `action_bars` (always on screen) |

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
   GearScore everywhere they appear; defined once in `snapshot.proto`, shared by
   composition (optional parts) since protobuf has no inheritance.
9. **Structured data, not screenshots:** bots have no client to render, images cost many more
   tokens, break prompt caching and get numbers wrong, and are hard to test. Exception: an
   optional synthetic top-down render in `surroundings` for spatial problems.

10. **Visibility levels (glance / open / hover):** the bot sees what's on screen; panels and
   tooltips need an action, like a person. Fields carry the `visibility` option in the
   proto; the validator rejects hover fields outside `tooltips` and panels that aren't open.
11. **Sounds** (aggro yells, spell sounds) mostly arrive as events already; a dedicated
   sound channel is deferred.
12. **Habits and remembered views:** routine looking (hover loot, check the map) is done by
   per-bot habits in the harness without LLM calls; the harness remembers closed panels,
   tooltips and chat with their age and renders them as memory.
13. **Protobuf, not JSON Schema** (rev 7): `snapshot.proto` is the single source of truth;
   C++ code is generated from it for the module and the harness (no hand-written structs to
   drift), it has `oneof` for variants and typed enums, and its canonical JSON mapping serves
   recordings and evals (tooling only; the wire is binary, lite runtime). protobuf and its
   abseil dependency are pinned in `third_party/`,
   built by `tools/build-protoc.sh`.
14. **Coverage audited against the client** (rev 7): every UI frame and every server opcode
   is mapped to a snapshot field or marked out of scope with a reason
   ([`coverage-ui.md`](coverage-ui.md), [`coverage-opcodes.md`](coverage-opcodes.md)). New in
   rev 7 from the audit: achievements, titles, currencies, glyphs and dual spec, equipment
   sets, mounts/companions, barber / guild bank / socketing / stable / charter / tabard
   windows, PvP panel and queues, world states, chat links, `/who` and ignore, all
   confirmation popups, roll / kicked / drunk / hearthstone events, stance and totem bars,
   runes, druid mana, weapon enchants, merchant buyback, inspect details.
15. **No caps in the module** (rev 7): `nearby` carries everything in the client's
   visibility set, with detail by distance as the client shows it (nameplate fields within
   name/nameplate range, model-level fields beyond, nothing for far units out of line of
   sight). Selection (attention, salience) is the harness's job, so evals and recordings see
   what was really there.
