# TODO

Open work only (Now / Next / Later). Completed work lives in `git log`.
Last updated: 2026-11-27 (team-play review @ `70b0878`).

## Now

Live verification following the `70b0878` team-play P0 fixes:

1. **Live playtest with a real client:** Alliance preset characters (`Aldric`, `Mirabel`, `Cedric`,
   `Vesper`, `Torvin`, `Bram`, `Pippa`, `Sylva`, `Talaan`), mock-LLM E2E (`e2e_human_party`,
   `e2e_human_dungeon`), and live-model E2E (`tools/wlb test live`: `dungeon_clear`,
   `human_disconnect`, `llm_stall`, `raid_boss`) are in place. Run a live session from a real
   3.3.5a client to confirm pacing and single-responder feel in practice.

## Next

Team-play P1 gaps and selected P2 items from the `70b0878` review:

### Questing & travel
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
