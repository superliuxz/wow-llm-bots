# TODO

Open work only (Now / Next / Later). Completed work lives in `git log`.
Last updated: 2026-11-24.

## Now (team play first: the gaps a human feels in the first hour)

- **Finish human + 4 dungeon e2e and live playtest:** Alliance preset characters (`Aldric`,
  `Mirabel`, `Cedric`, `Vesper`, `Torvin`, `Bram`, `Pippa`, `Sylva`, `Talaan`) and `e2e_human_party`
  (`tools/wlblib/wowclient.py` covering invite, `FindNewMaster` master handoff, single-responder
  chat, boss emote, wait/follow, and quest progress) are in place. Extend `e2e_human_party` to a
  full human + 4 dungeon run (LFG queue, ready check, loot roll, summon accept, and resurrecting a
  dead bot), and run a live session from a real 3.3.5a client to confirm pacing and
  single-responder feel in practice.

Lessons from official AI companions (Follower Dungeons, FFXIV Duty Support, Dragon's Dogma pawns,
Guild Wars heroes):
- Copy: match the player's pace (when the human tanks, NPCs never pull); wait patiently when the
  player goes AFK; no blame, low stress; direct tactical control (Guild Wars-style flags: instant
  "stay here / go there / kill skull").
- Avoid: body pulls and drifting healers (the top Follower Dungeons complaint); late heals;
  last-second dodges (they look like near-deaths); repetitive, obvious or sycophantic chatter;
  replies slower than about 1.5-2 s for commands; carrying the human so hard dungeons feel empty.

## Next (multi-session depth & architecture)

- **Short-term per-player memory & richer personas:** Store the last N exchanges per player
  (source-tagged so player chat never injects trusted instructions) and add more identities.
- **Single tick owner per bot:** Replace the string-blocklist between `mod-llm-brain` and
  `mod-playerbots` with an explicit per-bot mode (`brain-led` vs `playerbots-led`) and a startup
  check against playerbots' action factory.
- **Socket handshake on connect:** Exchange wire version, schema hash, and peer role on Unix socket
  connect so mismatched builds or stray tools fail with one clear refusal.
- **Latency & world-thread polish:** Measure 4 Hz combat snapshot cost at raid scale (build lighter
  combat snapshots if needed), reserve llama slots for the human's party, and add a real held-out
  eval split run at live temperature (`0.7`).

## Later (broader WoW systems)

- **Economy & upkeep:** Vendor panel for selling non-grey items, mailbox, player trade window,
  Auction House (wrapping playerbots' AH pricing or `mod-ah-bot`), and bank.
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
