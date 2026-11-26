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

- **Parallel e2e (use the idle cores):** steps 1-2 and 4 are done: `tools/wlb test e2e -j N` (default
  by free RAM and cores, capped at 6; `-j 1` is the old serial run) runs the groups on numbered
  test servers (`common.test_instance(k)`: `run-k`, `etc-k`, `var/logs/test-k`, `acore_test_k_*`,
  port 8086 + k), 21.4 -> 6.5-7 min here, and `tools/wlb test all [-j N]` runs `e2e` and `eval`
  concurrently (7.1 min total here). As built: shards share the test world database
  (`acore_test_world`, 558 MB, not written by tests) instead of one copy each; a shard's
  worldserver gets its `etc-k` bind-mounted over the install's `etc` in an unprivileged
  mount namespace (`unshare --user --map-root-user --mount`), because AzerothCore reads module
  configs from the compiled-in etc path (no namespaces: `-j 1`); SOAP/RA are off, so no ports for
  them; playerbots' command server (TCP 8888, shared with the play server) is off for tests. A
  test worldserver peaks at about 6 GB resident (maps loaded); 6 shards took ~29 GB.
  `human_dungeon` passed in every run (serial and parallel, runs back to back). Two cases were
  flakier under 6-way load and got test-only fixes: `orders` (a leftover phase-3 wake could
  install the kite plan before the phase-4 whisper woke the bot; a missed Shield act is now
  retried) and `boss` (a healer already 25 yd out when the Nova starts has nothing to move).
  Left (not started):
  3. **Both machines:** `tools/wlb test e2e --remote 10700k` splits the groups between this
     machine and the worker (`tools/wlb remote` sync, `touch` before rsync so mtimes don't
     leave stale objects) and merges the results. Only when asked (CONTRIBUTING rule 5).
  Checks: e2e passes with `-j 1` and with `-j N`, twice in a row; timing recorded in the commit.

- **Fix queue (next up, in this order; one commit each, full gates):**
  1. **`parse: error parsing grammar` during a passing test:** find which test prints it (grep
     the test logs and the GBNF loader), fix the grammar or the test input that triggers it, and
     make that message fail the test from then on.
  2. **Boss notes vs. loose-mob wake** and **tank swap** (below): after 1, as one tanking step.
  Older items not in this file any more get re-checked and listed if still real: no e2e for
  Hunter Dismiss Pet, /raid class/role addressing works only in /p. (`human_dungeon` run twice
  on one server: not reproduced in back-to-back full e2e runs, serial and parallel.)

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
  1. **Body pulls / drifting healers (not done):** nothing keeps bots off a pack's aggro edge or
     the healer in range and line of sight of the tank. E2e: a party walks past an unpulled pack
     (no bot aggroes it); during a pull the healer stays within 40 yd and LoS of the tank.
  2. **Late heals (not done):** no measure of heal reaction time. Perf/e2e: time from the tank
     dropping below a threshold to the healer's heal starting; set a budget.
  3. **Carrying the human (not done):** nothing limits bots' share of the work. Measure it in
     the human-party e2e (bots' share of damage/kills while the human plays) and decide a target.
  4. **Command reply speed (partial):** commands apply in the harness at once, but the LLM chat
     reply after one isn't gated. Perf: p95 from a party command to the bot's reply <= 2 s.
  5. **Last-second dodges (partial):** dodge eval cases exist; nothing checks the bot leaves
     early. E2e: time left on the cast when the bot clears the area, with a minimum margin.
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
- **Boss notes don't beat a loose-mob wake:** planned eval `party_tank_rotface_ooze_notes`
  (Rotface: the notes give Little Oozes to the off-tank) fails 0/8: woken by `loose mob Little
  Ooze on <dps>`, the tank still leaves Rotface for the ooze, even when the notes name it. The
  leader's word (`party_tank_lets_mage_kite`) and `fixated on` facts do work. Try putting
  notes nearer the screen, or a harness fact tying a nearby mob to its notes line.
- **No tank swap yet:** ICC notes now name tank swaps (Rune of Blood, Mutated Plague, Soul
  Reaper), but no option or act taunts, and the off-tank can't see the debuff stacks on the
  other tank; expose them as facts plus a `Taunt <mob>` option before raid tanking.

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
