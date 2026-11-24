# TODO

Open work only (Now / Next / Later). Completed work lives in `git log`.
Last updated: 2026-11-24.

## Now (first enjoyable play session with a human)

- **Human + 4 bots e2e case:** Add a live e2e scenario with a second scripted human account (not `testhuman`) that invites 4 bots, queues LFG, starts a ready check, rolls on loot, issues party commands ("wait"/"follow"), and rezzes a dead bot.
- **Fast party leader commands (`LeaderCommandParser`):** Parse a small party-chat vocabulary (`wait`, `follow`, `attack my target`, `pull`, `drink`/`oom`, `go sell`, `mount up`, `ready?`) in C++ in one tick, then wake the brain with `leader_said: <cmd> (applied)` so the bot replies in character.
- **Fast popup policies:** Extend standing orders with per-group `policy:` lines (`rez -> accept from party`, `roll -> need if usable upgrade else greed`, `lfg_role -> healer`, `summon -> accept`) applied by `PopupWatch` in `< 1 s`, waking the brain only when no policy matches; add Disenchant roll, LFG vote-kick, and summon accept.
- **Pull discipline, marks & CC:** Show raid marks on nearby units, add `Crowd-control <mob> (moon)` option for CC classes with a `-aoe` "don't break CC" order, and enforce tank-only pulls on unpulled packs in `content/RULES.md`.
- **Party pacing & wipe recovery:** Surface healer mana / drinking calls, add a `Start ready check` leader option before bosses, and handle soulstone/ankh wait vs corpse run + re-buff after a wipe.

## Next (multi-session depth & architecture)

- **Questing together:** Show party members' quest objectives on screen (`Help <name> with <quest> (3/8)`) and keep outdoor followers within ~30 yd of the leader unless told to stay.
- **Short-term per-player memory & richer personas:** Store the last N exchanges per player (source-tagged so player chat never injects trusted instructions), add more character identities beyond `Testpriest`/`Gatda`, and add natural chat typing delays + etiquette cues.
- **Single tick owner per bot:** Replace the string-blocklist between `mod-llm-brain` and `mod-playerbots` with an explicit per-bot mode (`brain-led` vs `playerbots-led`) and a startup check against playerbots' action factory.
- **Socket handshake on connect:** Exchange wire version, schema hash, and peer role on Unix socket connect so mismatched builds or stray tools fail with one clear refusal.
- **Latency & world-thread polish:** Measure 4 Hz combat snapshot cost at raid scale (build lighter combat snapshots if needed), reserve llama slots for the human's party, and add a real held-out eval split run at live temperature (`0.7`).

## Later (broader WoW systems)

- **Economy & upkeep:** Vendor panel for selling non-grey items, mailbox, player trade window, Auction House (wrapping playerbots' AH pricing or `mod-ah-bot`), and bank.
- **Professions:** Learn professions, craft skill-ups, fishing, and party disenchanting/crafting requests (wrapping playerbots' `craft`/`disenchant` actions).
- **Guilds & LFG social graph:** `guild:` chat output, guild invite/charter, LookingForGroup channel listening/posting, and slow background guild life jobs.
- **Raid logistics & level-80 progression:** Main tank/assist assignment, pull timers, Bloodlust/stack/spread raid warnings, master loot, lockout/consumable checks, RTSC room landmarks, talent deviations, glyphs, dual spec, gems/enchants, and emblem vendors.
- **Contributor portability:** Replace Arch-only Percona paths and RTX 5090 `CMAKE_CUDA_ARCHITECTURES=120` with auto-detection, document model download URLs/checksums, and check in one `report_stats.py` if benchmarks resume.
