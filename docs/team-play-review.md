# WoW Team Play Review

Tracking document for findings from the WoW Team Play Review. Each item is tagged with its target milestone (`[M1]`..`[M17]`) and checked off as that milestone lands.

## P0: breaks the session or wipes the group

- [x] **[M1] P0-1 — A human skull mark before the pull makes every bot attack.** Playerbots treats any skull-marked unit as an attacker. The repo guards only the bots' own marking (`pull_discipline.h:108-112`). Marking skull is the standard tank habit, so a human tank gets every pack pulled out from under them. (dungeon/raid)
- [x] **[M1] P0-2 — "stop" breaks the next fight.** `kStop -> halt_attacks() -> SetStopDps(true)` on every bot, tanks included (`llm_brain_world_script.cc:849-885`). Said out of combat, `saw_combat=false`, so it stays on through the whole next fight and clears only when that fight ends (`pull_discipline.cc:91-109`). DPS and pets stand idle, and the tank does no damage, builds no threat, and doesn't taunt. (combat/social)
- [ ] **[M8] P0-3 — Raid scripts are removed with nothing to replace them.** `AdjustRaidStrategies` turns off `naxx/ulduar/icc/voa/os/eoe/...` when brain-led (`config_parsing.cc:175-185`). That throws away playerbots' C++ for vehicles, harpoons, cannons, Malygos drakes, and positioning. The notes miss whole bosses (ICC Gunship, Festergut, Blood-Queen; LK Defile and Val'kyr) and some are wrong (LK Quake sends bots outward; Four Horsemen is modeled as taunt-in-place). Orders can't drive a vehicle. (raid)
- [ ] **[M9] P0-4 — Prompt budget at raid size.** The slot is 4096 tokens (`prompt.cc:30`, `llm.py:13`) and `RULES.md` takes about 2.6k. At 25-man the raid frame likely pushes out the boss notes, which are the only source of standing orders. The wake reason and the human's chat line can be dropped too. (raid/harness)
- [ ] **[M2] P0-5 — Bots can't abandon quests.** Nothing sends `CMSG_QUESTLOG_REMOVE_QUEST`. A failed escort or timed quest blocks that quest chain for good. Bots accept every `!` quest, so at 25 quests the log is full and the human's shares fail silently. (questing)
- [ ] **[M3] P0-6 — Bags fill up for good.** Bots sell only grey items (`exec_npc.cc:101,126`), can't destroy greens, and can't buy or equip bags. Greens pile up until quest drops can't be looted, and those corpses stop showing as lootable. (loot)
- [ ] **[M13] P0-7 — A ghost can get stuck forever.** If a corpse run can't make progress, the bot gets no spirit-healer option, no wake, and the human is never told. (travel/death)
- [ ] **[M5] P0-8 — Brain target picks don't stick.** `target` / `Attack` only set the selection, and playerbots' `dps assist`/`tank assist` picks again. 35 note files rely on `-> target nearest <add>` (Bone Spike, Web Wrap, Kinetic Bomb...). (combat)

## P1: hurts play often

### Questing

- [ ] **[M4]** The pull gate hides mobs attacking the human's pet, an escort NPC, or a friendly quest NPC. Bots stand by while the escort dies. The same gate stops bots from starting fights on the human's quest mobs, so `help <human> with <quest>` can't happen and grouping is no faster than solo.
- [ ] **[M2]** One pending share blocks every later share for up to 60 s. Shares aren't auto-accepted even with "answer popups" on.
- [ ] **[M2]** Bots can't take quests from objects or items ("Wanted: Hogger", item-started quests) and can't turn in quests that end at an object.
- [ ] **[M2]** Bots leave the group 60 s after the human logs off and keep levelling alone, so they drift ahead between sessions.

### Dungeons

- [ ] **[M6]** Tank behavior comes from the talent spec, not the group role. A tank-spec bot next to a human tank keeps `tank assist` and fights the human for aggro.
- [ ] **[M6]** A non-empty `var/bots/<bot>/notes.md` silently replaces the curated boss notes in every instance.
- [ ] **[M6]** In a bot-led group (LFG, or a bot invited the human), the human can't get lead: no promote, Heroic toggle, loot method, or reset.
- [ ] **[M4]** The pull gate applies only when the human leads. In bot-led groups, DPS bots still get `Attack (not pulled)`.

### Raids

- [ ] **[M11]** Raid members' buffs and debuffs are invisible except on tanks, so `<debuff> on anyone`, dispels, and rebuffs can't work.
- [ ] **[M11]** `/rw spread` and `/rw stack` move tanks too. At 25-man the spread ring is about 5 yd apart.
- [ ] **[M11]** Bots have no battle res and there's no wipe/release command. Dead bots wait up to 5 min while any healer lives.
- [ ] **[M11]** DBM fields and `ext.instance` (difficulty, lockout) are in the schema, but nothing fills them.
- [ ] **[M9]** Each boss emote wakes all ~24 bots at top priority. The pull starts 24 order compiles of up to 350 tokens each, so reactions lag by seconds.

### Combat

- [ ] **[M12]** Spending talents mid-session doesn't reset playerbots' class strategies, so a bot that goes Prot or Holy keeps its old rotation. Tests hide this with `ResetStrategies()`.
- [ ] **[M12]** A failed `out of it` step stays latched, so the bot keeps standing in fire until the LLM answers.
- [ ] **[M12]** No reaction to fire outside the boss notes (trash, abilities the notes don't list).
- [ ] **[M12]** Tank swaps need an LLM round trip, and "offtank" just means the 2nd tank in group order.

### Human <-> bot control

- [x] **[M1]** Leader commands are dropped while the harness is disconnected, and `stay` / `stop dps` set before that never clear.
- [ ] **[M7]** "brb"/"afk" wakes only the alphabetically first bot, so a bot tank keeps pulling while the human is away. The single responder is always that bot, even when another bot is the one who can act.
- [x] **[M1]** "wait" keeps holding in combat, so the healer doesn't move to the tank.
- [ ] **[M7]** A bot leader can queue Random Dungeon, start ready checks, or invite strangers without asking.

### Travel and death

- [ ] **[M13]** Followers chase the human's ghost to the graveyard.
- [ ] **[M14]** Bots can't fly: the picker takes a ground mount and movement is navmesh-only. Northrend group travel and Dalaran break.
- [ ] **[M14]** No wake when the leader changes map or takes a portal, boat, or flight. `follow` reports "applied" but does nothing across maps.
- [ ] **[M14]** "Go to leader" while the leader is on a taxi or boat aims at the leader's mid-route spot.
- [ ] **[M13]** Bots can't click meeting stones or summoning portals.

### Brain / harness

- [ ] **[M10]** P0 wakes in a row can throw away every reply mid-fight (livelock).
- [ ] **[M10]** One timed-out call marks the LLM unhealthy for all bots, and control flips to playerbots: release, corpse run, and popups. That flaps under load.
- [ ] **[M9]** Orders are compiled only out of combat, once at the instance entrance, with a 7-order cap. A boss pulled straight from trash gets no orders.
- [ ] **[M10]** No coordination between bots: no spread slots, interrupt rotation, or soak groups, so every bot does the same thing.

### Loot and upkeep

- [ ] **[M15]** `Buy` passes `count=1`, and food or drink is offered only when the bot has none, so bots can't stock up.
- [ ] **[M15]** Hunter ammo is never loaded: no `SetAmmo`, and playerbots' reload is blocked.
- [ ] **[M15]** No training or repair while grouped. Bots can't buy reagents or poisons (no Rebirth, Ankh, or poisons).
- [ ] **[M16]** Rolls: the 10 s "let the human vote first" delay applies only in auto-popup mode. The Need check counts attack power as a fit for both strength and agility classes and ignores hit, crit, and haste, so a warrior Needs a hunter's bow.
- [ ] **[M16]** Gear and quest rewards are picked by item level alone, ignoring armor type and stats.
- [ ] **[M16]** The trade window offers only the first 8 tradeable kinds of items.

## Milestone Plan

- **M1:** P0-1 (human skull mark), P0-2 ("stop" vs "stop dps"), P1 ("wait" in combat, disconnect clearing stay/stop dps)
- **M2:** P0-5 (abandon quest), P1 Questing (share queue/auto-accept, object/item quests, human logoff grace)
- **M3:** P0-6 (sell/destroy non-grey items, buy & equip bags)
- **M4:** P1 Pull gate (pets/totems/escorts/friendly quest NPCs, help human quest mobs, bot-led groups)
- **M5:** P0-8 (brain target picks stick)
- **M6:** P1 Dungeons (group role vs talent spec, `var/bots` `notes.md` vs curated boss notes, bot-led promote/heroic/loot/reset)
- **M7:** P1 Human<->bot control (brb/afk tank pause + capability-aware single responder, bot leader guard against unasked LFG/ready-check/invites)
- **M8:** P0-3 (keep playerbots raid strategies as body layer, audit & fix curated raid boss notes)
- **M9:** P0-4 (8k prompt slot, guaranteed wake/chat/notes budget), P1 Raids/Harness (raid-wide wake stagger/dedupe, shared role order compile, order compile from trash & cap)
- **M10:** P1 Brain/Harness (P0 livelock prevention, thresholded/per-bot LLM health, deterministic bot coordination slots/interrupts/soaks)
- **M11:** P1 Raids (raid buffs/debuffs beyond tanks, `/rw spread` & `/rw stack` tank exclusion + 25-man spacing, battle res & wipe/release, DBM & `ext.instance` metadata)
- **M12:** P1 Combat (talent `ResetStrategies`, failed `out of it` latch, generic fire reaction, tank swap speed & offtank role)
- **M13:** P0-7 (stuck ghost spirit healer/wake/notify), P1 Travel/Death (no graveyard chase, meeting stones & summoning portals)
- **M14:** P1 Travel/Death (flying mounts & flight movement, cross-map/portal/boat/taxi leader wake & follow & go-to-leader)
- **M15:** P1 Upkeep (`Buy` count>1 & food/drink restock, hunter `SetAmmo` & reload, grouped train/repair/reagents + leader go repair/train)
- **M16:** P1 Loot/Trade (10s human vote delay always, Need/gear/quest armor+stat scoring, trade window >8 items)
- **M17:** Tests
  - [ ] **[M17]** Live-LLM human-led dungeon E2E + 25-man raid fight with phases or vehicles
