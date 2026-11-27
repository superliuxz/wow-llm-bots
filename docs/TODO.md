# TODO

Open work only (Now / Next / Later). Completed work lives in `git log`.
Last updated: 2026-11-26 (team-play review @ `70b0878`).

## Now

Ordered team-play P0 fixes from the `70b0878` review, followed by live verification:

1. **Errand movement & left-behind catch-up (P0 #7):** A bot left behind on another map, after a
   missed boat/flight, or far away can't catch up — auto-recall only works on the same map over
   short paths, "Follow (party leader)" only appears at 8–25 yd, and the leader is never a travel
   destination (`server_module/src/bot_manager/bot_manager.cc:994-1019`,
   `server_module/src/options/exec_social.cc:544-548`,
   `server_module/src/options/option_facts.cc:432-435`). Pause follow and the leash while a brain
   errand is running, and add a "go to leader" travel target with cross-map routing.
2. **Live-model E2E team-play tests:** Add end-to-end tests with the live model covering a dungeon
   clear with pulls and a wipe, a human disconnect, an LLM stall, and a 10-man boss.
3. **Live playtest with a real client:** Alliance preset characters (`Aldric`, `Mirabel`, `Cedric`,
   `Vesper`, `Torvin`, `Bram`, `Pippa`, `Sylva`, `Talaan`), `e2e_human_party` and
   `e2e_human_dungeon` (a scripted client leads 4 bots through the Dungeon Finder into the
   Deadmines: ready check, summon, loot roll, ping/go there/los, a bot rez, Sap/Polymorph on marks;
   mock LLM, no pulls or boss fights) are in place. Run a live session from a real 3.3.5a client to
   confirm pacing and single-responder feel in practice.

## Next

Team-play P1 gaps and selected P2 items from the `70b0878` review:

### Control seam (brain vs. playerbots)
- **Follow cancels brain errands:** `follow` and `KeepSafeGroupFollow` (5.5 yd) cancel
  loot/gather/talk/use moves and stop casts; 40 yd leash resets brain walks every 4 s
  (`bot_manager.cc:1002-1019,1101-1151`; `pb:.../MovementActions.cpp:1086-1255`).
- **Wake storms drop plans:** Every spell miss (dodge/parry/resist) wakes brain at P1 and stops the
  plan; any wake during an LLM call drops the reply (`harness/src/plan/triggers.cc:640-654`,
  `bot_loop.cc:1299-1303,1991-1993`).
- **Healer main-tank leash:** All healers leash to main tank (>34 yd or LoS every 1 s), breaking
  two-tank fights, Valithria, and spread positions (`bot_manager.cc:1060-1097,1153-1206`).
- **Taunt ping-pong:** After brain taunts, playerbots' "lost aggro" reflex makes the old tank taunt
  back; no tank-swap coordination.
- **Scripted raid strategy conflict:** Playerbots scripted raid strategies (Naxx, Ulduar, ICC) and
  brain standing orders move the same bots without coordinating.
- **Solo bots playerbots overlap:** Without a human in group, playerbots `new rpg`/`grind` stays on
  alongside the brain (accepts/turns in/abandons quests, picks rewards, flies, teleports when
  stuck).

### Pull & threat discipline (dungeons)
- **Skull on unpulled mob chain-pulls:** A skull on an unpulled mob counts as an attacker -> chain
  pulls mid-fight; tank's "Mark with Skull" picks nearest hostile including CC'd
  (`pb:.../AttackersValue.cpp:38-44`, `exec_social.cc:632-658`).
- **Warlock Fear on CC target:** Warlock bots Fear the moon (CC) target into other packs
  (`pb:.../GenericWarlockStrategy.cpp:118-135`).
- **Tank nameplate range blindspot:** Tank can't see a mob's target past 20 yd; mobs hitting a
  healer at 25–35 yd don't wake the tank or offer a Taunt option
  (`server_module/src/snapshot/nearby_builder.cc:439-441,680-690`).
- **Healer mana pull wait:** Waiting for healer mana before pulling is only a prompt rule (soaks
  logged dungeon stalls up to 1,793 s, `docs/results.md:42-43`).

### Loot
- **Immediate auto-popup voting & roll criteria:** Auto-popup policy votes immediately, so "never
  Need against a human who already Needed" almost never fires; Need = any usable higher-iLvl
  non-BoE of armor type (no stat/spec check), and Disenchant is never picked
  (`exec_social.cc:1345-1382`).

### Raid scale & coordination
- **Role mismatch (harness vs. server):** Harness falls back to spec role while server checks only
  Main Tank flag or LFG role; in hand-formed raids `tank`/`offtank`/`healer` orders fail or target
  the wrong player (`order_evaluator.cc:383-418` vs `act_executor.cc:259-283,407-425`).
- **Standing orders limitations:** Compiled once on instance entry from the first 2,000 bytes of
  notes; never recompiled per boss; can't filter by role; `phase N` never fires because server
  never fills encounter data (`prompt.cc:34,933-945`; `bot_loop.cc:1199-1254`; `orders.wlb:16`;
  `order_evaluator.cc:1117`).
- **LLM throughput at 25-man scale:** Emotes, `/rw`, raid deaths, and loot rolls each wake all 24
  bots (~7 wakes/s capacity, no tank/healer priority).
- **Boss notes coverage & wiki fallback:** TOC is 5-man only (no Trial of the Crusader), AQ40 has
  none, Naxx is missing Heigan/4H/KT, no tank-swap orders, UK/Nex/GD cover 1 boss each; having
  *any* notes file disables wiki boss fallback (`prompt.cc:789-792`, `knowledge.cc:186-191`).
- **`away group` scope mismatch:** `away group` means subgroup on server vs. whole raid in harness.

### Questing & travel
- **No boats, zeppelins, or portals:** `Sylva` (Teldrassil) and `Talaan` (Azuremyst) are stuck on
  islands; bots can't reach Northrend alone (`config/llm-brain.overrides:4-9`).
- **Flight path limits:** At most 4 destinations, direct links only (`exec_npc.cc:412-432,617-624`).
- **No riding training or mounts:** Bots fall behind a mounted human from level 20
  (`exec_npc.cc:293-401`).
- **No NPC dialogue options or vehicle controls:** `Talk to` only walks up/targets; vehicle
  snapshot fields are never filled, blocking WotLK vehicle quests (`exec_npc.cc:163-184`,
  `snapshot/snapshot.h:1423-1431,2039`).
- **Full bags:** Bots only sell grey items and never destroy anything, so they stop looting quest
  items (`exec_npc.cc:53-94,321-336`).

### P2 (selected)
- **Leader commands scope & semantics:** Only work when human is group leader and only in
  party/raid chat (whispers don't count); `"stop"` maps to `"wait"` (keeps attacking); `"wait"`
  never times out (`llm_brain_world_script.cc:658-661,710-714`, `leader_command.cc:31`).
- **`act:` gated on group wakes:** `act:` lines are only allowed when wake reason starts with
  party/raid/leader (whispers can't trigger acts) (`prompt.cc:815-825`).
- **Redundant rez wake & retry backoff:** Brain is still woken when playerbots accepts a rez
  itself; invalid plans retry every 10 s with no backoff.
- **Docs drift:** Out-of-date paths for triggers/scheduler; docs say standing orders run only in
  combat.

## Later (broader WoW systems)

- **Economy & upkeep:** Vendor panel for selling non-grey items, mailbox, Auction House
  (wrapping playerbots' AH pricing or `mod-ah-bot`), and bank.
- **Professions:** Learn professions, craft skill-ups, fishing, and party disenchanting/crafting
  requests (wrapping playerbots' `craft`/`disenchant` actions).
- **Guilds & LFG social graph:** `guild:` chat output, guild invite/charter, LookingForGroup channel
  posting, and slow background guild life jobs.
- **Raid logistics & level-80 progression:** Main tank/assist assignment, pull timers,
  Bloodlust/stack/spread raid warnings, master loot, lockout/consumable checks, RTSC room
  landmarks, talent deviations, glyphs, dual spec, gems/enchants, and emblem vendors.
- **Contributor portability:** Replace Arch-only Percona paths and RTX 5090
  `CMAKE_CUDA_ARCHITECTURES=120` with auto-detection, document model download URLs/checksums, and
  check in one `report_stats.py` if benchmarks resume.
