# TODO

Open work only (Now / Next / Later). Completed work lives in `git log`.
Last updated: 2026-11-26.

## Now

- **Live playtest with a real client:** Alliance preset characters (`Aldric`, `Mirabel`, `Cedric`,
  `Vesper`, `Torvin`, `Bram`, `Pippa`, `Sylva`, `Talaan`), `e2e_human_party` and
  `e2e_human_dungeon` (a scripted client leads 4 bots through the Dungeon Finder into the
  Deadmines: ready check, summon, loot roll, ping/go there/los, a bot rez, Sap/Polymorph on marks;
  mock LLM, no pulls or boss fights) are in place. Run a live session from a real 3.3.5a client to
  confirm pacing and single-responder feel in practice.

## Next

- *(Empty — pull from `## Later` when `## Now` is clear.)*

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
