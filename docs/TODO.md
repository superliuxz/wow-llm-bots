# TODO

Open work only (Now / Next / Later). Completed work lives in `git log`.
Last updated: 2026-11-27 (automated live-model E2E verified).

## Now

Manual live verification (automated live-model E2E `tools/wlb test live` — `dungeon_clear`,
`human_disconnect`, `llm_stall`, `raid_boss` — passed):

1. **Live playtest with a real client:** Alliance preset characters (`Aldric`, `Mirabel`, `Cedric`,
   `Vesper`, `Torvin`, `Bram`, `Pippa`, `Sylva`, `Talaan`) and mock-LLM E2E (`e2e_human_party`,
   `e2e_human_dungeon`) are in place, and automated live-model E2E passed. Run a manual live
   session from a real 3.3.5a desktop client with a human player to confirm pacing and
   single-responder feel in practice.

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
