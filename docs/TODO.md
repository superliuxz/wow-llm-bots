# TODO

Open work only (Now / Next / Later). Completed work lives in `git log`.
Last updated: 2026-11-26.

## Now (team play first: the gaps a human feels in the first hour)

- **Live playtest with a real client:** Alliance preset characters (`Aldric`, `Mirabel`, `Cedric`,
  `Vesper`, `Torvin`, `Bram`, `Pippa`, `Sylva`, `Talaan`), `e2e_human_party` and
  `e2e_human_dungeon` (a scripted client leads 4 bots through the Dungeon Finder into the
  Deadmines: ready check, summon, loot roll, ping/go there/los, a bot rez, Sap/Polymorph on marks;
  mock LLM, no pulls or boss fights) are in place. Run a live session from a real 3.3.5a client to
  confirm pacing and single-responder feel in practice.

- **Parallel e2e (done):** steps 1-4 are done: `tools/wlb test e2e -j N` (default by free RAM and
  cores, capped at 6; `-j 1` is the old serial run) runs the groups on numbered test servers
  (`common.test_instance(k)`: `run-k`, `etc-k`, `var/logs/test-k`, `acore_test_k_*`, port 8086 + k),
  21.4 -> 6.5-7 min here; `tools/wlb test all [-j N]` runs `e2e` and `eval` concurrently (7.1 min
  total here); and `--remote WORKER` on `e2e`/`all` syncs with `rsync -a --no-times --checksum`
  (no stale objects or spurious rebuilds) and splits groups proportional to each machine's
  `default_jobs()` (7.2 min with `10700k` at `-j 1` + local `-j 6`, bounded by the single 8-case
  `core` group). Only use `--remote` when asked (CONTRIBUTING rule 5).

- **Fix queue:** cleared. (`human_dungeon` run twice on one server: not reproduced in back-to-back
  full e2e runs, serial and parallel.)

Lessons from official AI companions (Follower Dungeons, FFXIV Duty Support, Dragon's Dogma pawns,
Guild Wars heroes):
- Copy: match the player's pace (when the human tanks, NPCs never pull); wait patiently when the
  player goes AFK; no blame, low stress; direct tactical control (Guild Wars-style flags: instant
  "stay here / go there / kill skull").
- Avoid: body pulls and drifting healers (the top Follower Dungeons complaint); late heals;
  last-second dodges (they look like near-deaths); repetitive, obvious or sycophantic chatter;
  replies slower than about 1.5-2 s for commands; carrying the human so hard dungeons feel empty.
- Where we stand (2026-11-26): pace, AFK wait, no-blame and instant commands are in RULES.md
  and the harness (AFK, kill-skull and go-there have e2e). Gaps, to plan as tests first:
  1. **Body pulls / drifting healers (done):** `party_positioning` e2e (`e2e_group.py`) walks a
     4-bot party past an unpulled Deadmines pack with 0 body pulls and verifies the healer closes
     to <= 40 yd and LoS of the tank during a pull; pure-harness `KeepSafeGroupFollow` and
     `KeepHealerInTankRangeAndLos` enforce both while yielding to stay, casts, and forced moves.
  2. **Late heals (done):** `heal_reaction` e2e (`e2e_group.py`) + `llmtest sethp` measures 3
     in-combat tank drops to 45% HP against a 1500 ms budget; fixed grouped healers restoring
     their exclusive heal strategy and keeping `-threat`.
  3. **Carrying the human (not done):** nothing limits bots' share of the work. Measure it in
     the human-party e2e (bots' share of damage/kills while the human plays) and decide a target.
  4. **Command reply speed (partial):** commands apply in the harness at once, but the LLM chat
     reply after one isn't gated. Perf: p95 from a party command to the bot's reply <= 2 s.
  5. **Last-second dodges (done):** `boss` e2e (`e2e_combat.py`) starts Loada inside Loken's 20 yd
     Lightning Nova radius and enforces >= 1000 ms remaining on the cast when clearing 20 yd.
  6. **Chatter (partial, rules only):** no long-session check. Soak: count repeated, narrating
     or sycophantic lines per hour (simple string checks plus an LLM judge) with a ceiling.

## Next (multi-session depth & architecture)

- **`act:` eval coverage:** No eval case asks for an `act:` (e.g. party `"sheep moon"`, a ping
  plus `"go there"`), and the eval only counts unasked `act:` lines (1 of 59 at T=0) without
  scoring them. Add asking cases, then score unasked acts as misses.
- **Short-term per-player memory & richer personas:** Store the last N exchanges per player
  (source-tagged so player chat never injects trusted instructions) and add more identities.
- **Latency & world-thread polish:** Measure 4 Hz combat snapshot cost at raid scale (build lighter
  combat snapshots if needed).

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
