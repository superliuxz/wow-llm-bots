# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-23.

## In progress

- [ ] **Phase 4 expanded plan & 4 h endurance soak (Classic + TBC + WotLK):**
  - **Already fixed & live-verified (`5814b75`, `a670917`, `61adb82`):** group/dungeon non-combat strategies (`AdjustNonCombatStrategies`), `Heavy Sharpening Stone` spam (`51` → `0` invalid), `item_equip` ping-pong (`61` → `0`), dungeon death/corpse-run recovery (`BrainPolicyListener` + `RecoverStuckDungeonGhost`), CI `rsync` fix, and `acore_world` `quest_poi` backfill (`+2,438` POI rows -> `9,135` quests covered).
  - **Step 1 — Close remaining dungeon-execution gaps:**
    - [x] Add `party leader` self-tag on `me:` in `harness/src/screen/screen_renderer.cc` so dungeon leaders know they are the leader and pick `Explore` after clearing a room (`36/36` screen tests, `0` golden diffs, `47/52` eval parity).
    - [x] Pass explicit `tank`/`heal`/`dps` roles in `soak.py` and add `llmtest difficulty heroic` + `llmtest setskill` in `server_module/src/test/test_commands.cc` (live-verified across all 6 endurance scenarios).
    - [x] Live-verify `RecoverStuckDungeonGhost` on an actual dungeon wipe/death (caught `Player::GetCorpse()` cross-map `nullptr` bug, switched to `Player::GetCorpseLocation()` + `stuck_ghost_since_ms`, live-verified L12 bot die in Deadmines -> release to Westfall `map=0` -> recover into `map=36 instance=1 alive=1` after 30 s).
  - **Step 2 — Expand open-world bot mix (longer quests, professions & world activities):**
    - [x] Spread open-world soak bots across Classic, TBC, and WotLK zones (`L32–L78`: STV, Tanaris, EPL, Hellfire, Nagrand, Borean Tundra, Dragonblight, Zul'Drak, Icecrown) with multi-step quest chains, item drops, vendors, trainers, and flight paths.
    - [x] Seed gathering & secondary professions (Herbalism, Mining, Skinning, First Aid, Cooking, Fishing + tools) on open-world bots via `llmtest learn`/`setskill`/`additem`.
  - **Step 3 — 4-hour endurance soak & comparison (Classic + TBC + WotLK):**
    - [x] Add/verify boss notes (`content/notes/Ulda/notes.md`, `content/notes/Strat/notes.md` — verified against upstream AzerothCore scripts, `Spell.dbc`, and `OrdersTest.AllContentNotesParseWithRealOrdersParser`) and wire `SUITES["endurance"]` (6 scenarios: Uldaman L42, Stratholme L60, Sethekk Halls L67, Magisters' Terrace L70H, Ahn'kahet L74, Pit of Saron L80H) with boss-notes seeding + 6/6 smoke checks passing (`0` LLM errors, `0` invalid plans, `10/10` moved).
    - [ ] Run the full **4 h endurance soak** (`SUITES["endurance"]`, `6 × 40 min` = `4 h` wall-clock / `40` bot-hours) with `stall_check.py` and `soak_scorer.py`, tracking dungeon clears/wipes/recoveries/orders and open-world quests/professions/gathers/skill-ups.
  - **Step 4 — Independent review & sign-off:**
    - Cold review of `2591949..b7bf727` (`0` High, `5` Medium, `2` Low) — all 7 fixed & verified:
      - [x] **Medium 1 (`bot_manager.cc:195-207`):** `EnsurePolicyListener` now runs on every `BotManager::Update` world tick (O(1) hash lookup) so `BOT_STATE_DEAD` is protected immediately on first death.
      - [x] **Medium 2 (`bot_manager.cc:241-252`):** `RecoverStuckDungeonGhost` now teleports the ghost directly into the dungeon entrance so AzerothCore's `HandleMoveWorldportAck` resurrects and converts the corpse on the dungeon map; only falls back to in-place revive if `!entrance` or `TeleportTo` fails.
      - [x] **Medium 3 (`bot_manager.cc:105-121`):** `HasEligibleTempEnchant` now checks only `EQUIPMENT_SLOT_MAINHAND` when `is_stone == false` (matching `ImbueWithOilAction::Execute`).
      - [x] **Medium 4 (`backfill_quest_poi.py:357`):** `creature_template.lootid == 0` now skipped (`if lootid > 0:`), unit-tested, and `quest_poi_backfill.sql` regenerated.
      - [x] **Medium 5 (`soak.py:592-605`, `tools/wlb:98-101`):** `--runs` now defaults to `0` (run full suite once) and `tools/wlb` help lists `endurance`.
      - [x] **Low 6 (`bot_manager.cc:166-169`):** `"equip upgrades packet action"` allowed when `event.GetSource() == "no ammo"` so Hunters can auto-equip replacement ammo.
      - [x] **Low 7 (`backfill_quest_poi.py:409-410,445`):** `apply_sql` deduplicates DB names and validates `^[A-Za-z0-9_]+$`.
    - Follow-up cold review of `ee34a87` (`0` High, `1` Medium, `1` Low) — both fixed & verified:
      - [x] **Medium (`bot_manager.cc:173-176`):** `HunterNoAmmoTrigger` inherits `ItemCountTrigger::getName()` (`"item count"`), so updated carveout to `event.GetSource() == "item count" && bot_ && bot_->getClass() == CLASS_HUNTER`.
      - [x] **Low (`tools/wlblib/testing.py:439`):** updated wrapper `def soak(..., runs=0, ...)`.

## Next, in order

**Priority direction (2026-11-21):** Mechanical WoW competence (dungeons, raids/ICC, clearing quests,
soloing elite quests, smart gearing) takes priority over `"human-like"` mimicry (`D`). **Phases 1–3
are signed off** (see [`docs/benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md`](benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md) §4: `llm` matches/slightly leads hand-written C++ boss scripts generically without boss-specific C++; real ceilings are Playerbots' class AI, `gemma-4-26B-A4B-it` (26B MoE / 4B active) order reliability, and generic movement vs. hardcoded room landmarks). **Phase 4 (`B1–B3`) core and initial dungeon-cohesion/loop fixes (`5814b75`) are implemented; Phase 5 (`C4`) is signed off (`gemma-4-26B-A4B-it` remains default).**

1. **Phase 4 follow-up & 4 h endurance soak:** add a `party leader` self-tag on `me:` in `screen_renderer.cc` so dungeon leaders pick `Explore` reliably after clearing a room, live-trigger `RecoverStuckDungeonGhost` on an actual dungeon death, then run a full 4 h soak with `stall_check.py`.
2. **Phase 5 — Fine-tuning exploration (`C3`, Decision #3: keep as TODO; `C4` model bake-off DONE — keep `gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf`):**
   - **`C4` (DONE 2026-11-23):** See [`docs/benchmarks/2026-11-23-phase4-soak-and-phase5-c4-faceoff.md`](benchmarks/2026-11-23-phase4-soak-and-phase5-c4-faceoff.md). `Qwen3.8-27B-IQ4_XS.gguf` scored `49/52` (`94%`) on offline eval vs `gemma-4-26B-A4B-it` `47/52` (`90%`), but tied on live 5-boss kills (`51/80` `63.8%` vs `52/80` `65.0%`, `p = 1.0000`) while being `2.1×` lower throughput (`4.6` vs `9.6 wake-ups/s`), `3–4×` slower in live combat (`552–970 ms` vs `168–249 ms`), and saturating at 100 bots (`21.69 s` vs `0.41 s`). Keep `gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf` as default.
   - **`C3. Train the model on its own good decisions (TODO — Decision #3: evaluate cost/feasibility on one RTX 5090 first)`:**
     Once `C1` and `C2` produce a few thousand graded decisions, evaluate cost and benefit of:
     (a) a small LoRA fine-tune of Gemma on plans that led to kills; (b) preference pairs (good vs
     bad plan on the same snapshot), gated on offline eval plus the boss bench.
3. **Human-likeness with human traces (`D`, `docs/plans/self-improving-loop.md` — deprioritized behind mechanical gameplay & ICC per Decision #4):**
   - Already built/planned: record human play sessions across all situations (5 Hz + events, ~30%
     held out), run the parity and pairwise scorer, and run `tools/bench/blind_rate.py` batches +
     `llm-agree`.
   - AI-side additions when resumed: draw reaction delays, pauses, and pacing from human trace
     distributions instead of constants; compare bot choices against human choices in matched
     situations (pulls, looting, town visits) and turn mismatches into `C1` eval cases; keep chat
     reply rate and style rare and matched to logs.
4. **Named room landmarks** (`docs/specs/triggers.md`): 132 of the 218 unsayable boss actions move
   to hardcoded room coordinates; deferred until a boss fight needs per-room landmark data.

## Later

- **Dungeon Finder (LFG / RDF) integration (`server_module/`):** wire AzerothCore's `sLFGMgr` state
  (`queued`, `proposal`, `role_check`, `in_dungeon`) and brain-chosen LFG options (`Queue Dungeon
  Finder`, `Accept/Decline proposal`, `Leave queue`, `Teleport to/out of dungeon`) so bots can queue
  organically while questing (snapshot schema and `harness/src/plan/triggers.cc` LFG wake triggers
  already exist).
- **Optional brain-chosen group movement / formation tuning:** currently out-of-combat group members
  use Playerbots' non-combat `+follow` formation movement controller between pulls when the brain
  picks `Keep` (combat & boss positioning are brain-owned). If less formation-followy movement or
  explicit brain-chosen split/regroup is desired later, tune formation spacing or expose a
  `Regroup` / `Go to <leader>` option.
- **Bind hearthstone** at an innkeeper as an option of its own (the brain's choice, through the
  bind spell), not part of Talk (`docs/specs/options.md`).
- **More e2e cases:** death and respawn, harness reconnect, a failing choice reaching the brain.
- **More perf:** a raid-wide burst (N bots woken in the same instant every M seconds; today's
  `SURPRISES` wakes bots independently) and cells past capacity (`docs/specs/harness.md`, "Age").
- **GPU `--backend-sampling` or thin `libllama` runner:** GBNF disables GPU `--backend-sampling`
  (~10 ms/step CPU overhead under 16-slot load); either run unconstrained with `--backend-sampling`
  + 1 grammar retry, or replace `llama-server` HTTP with a thin `wlb_net` `libllama` daemon that
  masks structured fields and GPU-samples free text.
- **Brain memory**, `var/bots/<name>/` (its own git repo): not created yet, on purpose.
- **`IsInRandomAccountList` audit**, the remaining sites (`docs/playerbots-random-bot-audit.md`).
- **`module/server/`** layout, when a second AzerothCore module exists.

## Loose ends

- When two brain-run bots whisper `"who"` back and forth (`Gatda` -> `Testpriest`), they echoed
  `"who"` several times before switching to a real question (and stopped naturally after
  `"sounds good, let me know when you're ready"`).
- A bot once wrote `tell <its own name>:` (Gatda, Gemma 4, live 2026-11-17); the module refuses
  it. Likely cause, the RULES.md example `tell Gatda:`, is fixed; watch whether it recurs.
- **Eval (2026-11-18, Gemma 4):** 42/48 scored cases (88%, random 22.0; 4/4 standing-order cases
  pass, `do:` p50 `72 ms`, total `208 ms`; 7 unfailable cases not scored). Misses:
  `healthy_boar_pull`, `skip_tapped_mob`, `red_mob_nearby`, `hp_60_fine`, `bags_full` (with closed
  bags it explores; in live play the habit opens full bags, and `bags_full_open` passes),
  `new_loot_hover`. The Qwen3.6 shootout was not re-run.
- **Panels, things not built yet** (`docs/specs/options.md`): `Use <item>` for bag items now works
  for self/selected-target on-use items (`A6`), while explicit unit/object/corpse target selection
  for spec §10 quest items (and its eval case) is not built yet; the module sends no
  `quest_accepted` / `quest_complete` / inventory-full events, so the habits read the snapshot
  instead (`bag_free_slots`, the quest tracker); "open bags at a vendor" waits for vendor windows
  (`interaction`); hover only for items and quests (spells, auras, units, places later).
- Dead bots: only Keep is offered while dead, refused steps back off (2-30 s), and coming back
  to life wakes the brain ("alive again"). Release spirit / run to corpse are not options yet
  (the body's playerbots dead strategy handles it).
- **Running an agent unattended:** an Antigravity chat in the VS Code Remote-SSH sidebar runs as
  `agy --hub` under VS Code's extension host. When the SSH client drops, VS Code Server keeps it for
  3 h, or for only 5 min if the laptop dark-wakes and reconnects briefly. Then it kills the session
  (this happened 2026-11-17: disconnect 01:59:57, reconnect 02:00:21, killed 02:05:23;
  `~/.vscode-server/data/logs/20261116T091853/remoteagent.log`). Processes started detached
  (`worldserver`, `mysqld`, `llama-server`) survive. For overnight work, run the agent outside VS
  Code. `agy remote-control` daemon: set up and active since 2026-11-17 09:42 (systemd user unit
  `antigravity-cli-daemon.service`, `Linger=yes`, instance `cachyos`). Not yet tested: whether a
  conversation started through it survives the laptop closing. Alternative: `agy` in `tmux` on
  CachyOS.
- `tools/wlb setup data`, `llama` and `msan` were ported from shell and **not run** (each
  overwrites something that works). Test them in a scratch directory, or on the next fresh checkout.
- `CLAUDE.md` imports `CONTRIBUTING.md` with `@CONTRIBUTING.md`: **not yet confirmed** that a fresh
  Claude Code session loads the rules through it.
- The human player's account name is in the git history (commit `c8ae0f4`,
  `tools/server-start.sh`; and `test_accounts_unittest.cc` until 2026-11-17); it is out of the
  current files. Removing it means rewriting history and a force-push: their decision.
- Unit tests for the AzerothCore-dependent module code (`bot_manager`, `option_executor`,
  `nearby_builder`, ...) can't run without the game server: only the e2e cases cover them.
- `BrainBridge`: a connection generation drops stale frames, but a stalled old connection's
  commands still arrive in order before its disconnect is seen. Judged not worth more yet.
- Bugs found in playerbots (not ours, `third_party/`): `RangeTriggers.cpp:275` returns before its
  distance loop (Yogg-Saron Brain Link); `NotBehindIngvarTrigger` fires when the bot *is* behind its
  target. Five `*Cheat*` boss triggers fire only with playerbots' raid cheat on.
- A flaky risk in `e2e choose`: if the boar drops nothing there is no corpse to loot, and the case
  fails with "never offered". Passed every run so far.
- **Live boss findings (Loken, 2026-11-18):**
  - *(Fixed by `A1` hold-until-cast-ends and `A4` mid-fight empty-order preservation; remaining edge
    case:)* Gemma 4 occasionally rewrites a working `Lightning Nova cast` order into `Loken cast`
    or `Lightning Nova appears` when a mid-fight wake emits non-empty `order:` lines.
- **Not individually live-triggered after final edits yet:** `llmtest questcomplete` when a quest
  is already active in a bot's quest log (`CompleteQuest` + `RewardQuest` branch in
  `test_commands.cc`, source-verified), `tools/wlb remote` live `Ctrl-C` PGID termination / SSH-drop `255` cleanup / `status` with an active `worldserver` (unit-tested locally against real subprocesses & mocks in `tools/wlblib/remote_test.py`), plus Review 10 single-arm `_wait_checkstrat` +
  `+{strip_strat}` (`tools/bench/bench_bosses.py`) and sorted list-head cap eviction
  (`server_module/src/snapshot/behavior_trace.{h,cc}`), which are unit-tested and compiled into
  `out/server-test` without starting a new live benchmark run (per user instruction).
  *(Environmental/self death `died` with `detail="environment"` caveat closed per Review 9 —
  live-triggered on `murmur`, and entrance staging/pull coordinates for `devourer`, `loken`,
  `kaelthas`, `murmur`, and `ikiss` live-verified across `64` valid trials (`n = 16` per arm)
  each.)*
- **Phase 1–3 open caveats & deferred polish:** `tools/bench/bench_bosses.py` still uses inline boss-note constants rather than reading `content/notes/*/notes.md` directly; `A5` `party hurt:` and `loose mobs:` screen formatting are unit-tested only (not separately captured from live `harness.log`); `eregos` (`bench_bosses.py`) still hits `no_pull` because Oculus requires flying drake engagement/boss trigger; `tools/bench/notes_hill_climb.py --confirm-runs > 0` re-run is unit-tested only (not live-verified).
- **Phase 4 live play observation (2026-11-22):** level 14 priest observed still farming mobs in Northshire Abbey (level 1 starting zone) — directly motivates Phase 4 `B1` (level-appropriate questing/zone guidance over grinding grey mobs).
- **`private/BigFootAddOns` licensing reminder:** private nested submodule rehosting proprietary addons (DBM, Omen, etc.); fine while private — never publish it or copy any of it into the public GPL repo.
- **Human trace GM-speed cleanup & raw-turn check (deprioritized — Phase D only):** `2026-11-22.1.jsonl` in `~/.local/share/wow-llm-bots/human_baseline/human/Hele/` was cleaned of `> 10 yd/s` GM-fly/speed segments; strip `> 10 yd/s` segments from `2026-11-22.jsonl` if Phase D human-likeness scoring is resumed, and optionally check `trace_parity.py` raw-turn snap rates on human traces (user deprioritized movement-mimicry metrics in favor of mechanical WoW competence).
- **Deliberately skipped benchmark speedup proposals (Items 4, 5, adaptive 6):** left unbuilt on
  purpose to preserve valid boss immunity/kite phases (Item 4: `20 s` zero-damage abort would
  cut `Gravity Lapse`/`Planar Shift` and censor `llm` bugs via survivorship bias), cross-boss DB
  isolation + startup-bound `LlmBrain.TraceDir` (`scorer.sha256` lock) (Item 5: cross-boss
  server reuse without `test_databases_reset()` saves only `~44 s/boss`), and unbiased fixed-`n`
  statistics (adaptive Item 6: stopping at `n=3` has a `42.2%` false-negative rate on a `75%` arm;
  details in [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md) §6).

## Done (newest first, keep about ten)

- 2026-11-23: **Phase 4 live soak (1 h / `10` bot-hours) & Phase 5 (`C4`) model face-off (`docs/benchmarks/2026-11-23-phase4-soak-and-phase5-c4-faceoff.md`):**
  - *Phase 4 live soak (`3 × 20 min`, `soak_horde_rfc` / `soak_alliance_dm`, `world_knowledge.json`, `scorer.sha256` verified):* `2,186` decisions (`0` LLM errors, `0` invalid plans, `0` repeated refusals, `do_p95 = 352 ms`), `4,476 XP/h`, `7` deaths, `30/30` moved, `4.98 Hz` parity. Open-world (`15` bot-runs): **`0/15` stalls**, `80` quests accepted, **`44` quest turn-ins**, `29` level-ups, `582` mob kills, `263` item equips, live cross-zone travel (`Tirisfal -> Silverpine`, `Elwynn -> Westfall`, `Silverpine -> Durotar -> The Barrens`). Honest gaps logged in `## In progress`: `8/15` dungeon-party followers stalled (`525.9–1,078.0 s`) and `human_react_timing` missed by `4 ms` (`146.0 ms` vs `>= 150 ms`).
  - *Phase 5 (`C4`) model face-off — **keep `gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf` as default**:* 5-model `tools/wlb test eval` (`52` cases): `Qwen3.8-27B-IQ4_XS` **`49/52` (`94%`, `153/418 ms`)**, `gemma-4-26B-A4B-it` **`47/52` (`90%`, `68/232 ms`)**, `Qwen3.6-35B-A3B` `37/52` (`71%`), `gemma-4-E4B` `31/52` (`60%`), `Qwen3-4B` `28/52` (`54%`). `perf brain`: `gemma-4-26B-A4B-it` `9.6 wake-ups/s` (100-bot `wake->do` p50 `0.41 s`) vs `Qwen3.8-27B` `4.6 wake-ups/s` (100-bot saturates at `21.69 s`). Full 4-arm `n = 16` (`320` valid trials, `0` invalid) 5-boss bench (`var/bench_boss_c4_qwen38_27b_n16.json`): `Qwen3.8-27B` `llm` **`51/80` (`63.8%`)** vs `no_brain` `33/80` (`p_holm3 = 0.0208`) and `Ikiss` `13/16` vs `3/16` (`p_holm15 = 0.0164`), statistically identical to `gemma-4-26B-A4B-it` `52/80` (`65.0%`, `p_raw = 1.0000`, `p_holm5 = 1.0000` on every boss) while `3.1×–4.4×` slower in live combat (`552–970 ms` vs `168–249 ms`).

- 2026-11-23: **Full-codebase review `7727a71` (all 6 High, 11 Medium, 7 Low) + all post-commit review follow-ups (`9045684`–`231a326`) & user-approved `scorer.sha256` relock (`1a867dcdf2fd...`):**
  - *`common/base/` & `common/net/` (`H1, M1–M3, L2`):* drained active `TaskQueue` wakeups before `Thread::Stop()` destroys `pump_`; pinned `RepeatingCallback::Run` `impl_`, moved `unique_ptr<FramedConnection>` into deferred cleanup, and called `ScheduleRetry()` before `on_disconnected`; checked all `pthread_mutex_*` return values in `base::Lock` (`LockDeathTest`); fixed `PlatformThread::Sleep` non-positive duration / `EINTR`; fixed `MessagePumpEpoll` controller reuse and dual non-persistent `WATCH_READ_WRITE` with pump-monotonic `watch_id_` + watcher/controller lifetime sentinels + re-arm suppression (`wlb_common_unittests` `67/67`).
  - *`server_module/` & `harness/` (`M4–M9, L1, L3–L4` + `"equip:"` guard):* eliminated `raw_ptr<IoCore>` incomplete-type UB via `IoCoreHandle`; ported `BehaviorTraceWriter` to `base::Lock` (`GUARDED_BY`) / `base::ConditionVariable` / `base::PlatformThread` and `config_parsing.cc` to `base::SplitString`; aligned `kArmor[]` with `ItemSubclassArmor` and passed matched spell slot index to `GetSpellCharges(idx)`; verified `Command_Act::EQUIP` by `ObjectGuid`, fixed `Command_Act::JUMP` physics/LoS/state guards, and added explicit `"equip:"` guard in `OptionTargetProgress`; coalesced order wakes + uncovered snapshot triggers and preserved edge triggers during refusal backoff (`harness_unittests` `247/247`, `llm_brain_unittests` `126/126`, `out/server-test` rebuilt; server-module C++ edits compile/source-verified).
  - *Tools, boss notes & docs (`H2–H6, M10–M11, L5–L7`):* fixed `remote.py` `rsync` `~/` expansion + spaced `pull()` (live `rsync` test in `remote_test.py` `24/24`); namespaced multi-run soak traces per run (`<name>_r00`) inside `LlamaServer` scope, preserved candidate traces across held-out overwrites, enforced `human_baseline`/confirm/held-out gates in `score-candidate`, scoped `_revert_to` with `--no-renames`, wired `--min-eval-passed`/`--eval-passed`, and surfaced `UNANCHORED` Bradley-Terry ratings (`hill_climb_test.py` `13/13`, `soak_suites_test.py` `5/5`); fixed `Uld`/`ICC`/`BT`/`Hyjal`/`Mag`/`RS`/`OS`/`BWL`/`ZA`/`Ony`/`Nex` boss notes with per-`## ` section `CollectOrderNames` validation in `orders_unittest.cc`; updated benchmark Fisher stats and spec docs; relocked all `30` measurement files (`1a867dcdf2fd...`) with user approval (`ALL CHECKS PASSED`).

- 2026-11-22: **Phases 1–3 signed off & Phase 4 (`B1–B3`) core + boss strategies + Reviews `cd0ca9c`–`904059d` fixes:**
  - *Phases 1–3 sign-off & conclusions:* documented findings, insights, and ceilings (Playerbots C++ class AI, `gemma-4-26B-A4B-it` 26B MoE / 4B active order reliability, generic movement vs. hardcoded landmarks) in [`docs/benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md`](benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md) §4.
  - *`B1` 3.3.5a Atlas+Wiki knowledge → prompt, outcome breadcrumb, prompt inspector, `"Turn in"` & outleveled-zone travel:* [`export_knowledge.py`](../tools/knowledge/export_knowledge.py) exports strictly pinned 3.3.5a WotLK zones (`61` canonical `AreaTable` zones, `The Barrens (7-30)` in Durotar/Mulgore `next_zones`, questless/PvP zones excluded from `next_zones`), all `77/77` canonical 3.3.5a dungeons (`0` Cataclysm/`(Classic)` leaks, level-80 Naxxramas/Onyxia preserved, headings stripped, short/long mirror fallback `"Stockade"` tested in `export_knowledge_test.py`), `1,183` 3.3.5a boss & elite-add `Abilities` + `Strategy`/`Tactics` summaries (`{{abilities|...}}`/`{{spell|...}}` expansion, balanced `[[File:/Image:/Category:...]]` stripping with `0` stray `]]` leaks, exact-before-broadened heading priority + non-stub `finditer()`, anchored `{{Cataclysm}}` banner & Cataclysm-revamp `(tactics)` filters eliminating `Neltharion`/`Hogger`/`Balnazzar` revamps while retaining `{{Cataclysm/Update}}`, `{{Infobox ability` player-spell filter excluding `Blizzard`/`Mirror Image`, `"Please see..."`/placeholder stub & lore-heading filters without dropping concise real text, `>= 60`-char `abilities`-only raid adds retained, redirect resolution for `The Lich King` -> `Lich King (tactics)`, and instance-scoped multi-encounter keys like `Magisters' Terrace::Kael'thas Sunstrider`), and `9,464` quests (`$b`/`$N`/`$g` + HTML entities cleaned -> `var/knowledge/world_knowledge.json`, `2.50 MiB`); pure-C++26 [`WorldKnowledge`](../harness/src/screen/knowledge.cc) (`std::flat_map`/`std::flat_set` with transparent `std::less<>`, `ext.encounter->boss` + target/proximity + `where.zone`/`where.map` instance-scoped boss injection ahead of dungeon/quest lines, friendly/totem exclusion, NaN-safe sort, suppressed when boss notes exist; `knowledge_unittest.cc`), epoch-guarded `last_outcome` breadcrumb across all 4 `Requeue` sites + startup `boss_count()` / warning log in [`bot_loop.cc`](../harness/src/runtime/bot_loop.cc) (`BotLoopTest.LastOutcomeDeliveredOnceAndKnowledgeInjected` covers all 4 `Requeue` sites — superseded-wake, same-epoch cancelled, brain error, invalid-plan — plus valid-reply consumption and non-empty wrong-epoch disconnect), boss-notes-first budget + single-pass `InspectPrompt` / `--inspect-prompt` & `--knowledge` in [`prompt.cc`](../harness/src/screen/prompt.cc) / [`decide.cc`](../harness/src/runtime/decide.cc) (`decide_unittest.cc`); `"Turn in <quest> at <place>"` in [`options.cc`](../server_module/src/options/options.cc) (`options_unittest.cc`); `±3` same-zone → cross-zone outleveled-spot fallback, hash-indexed `WorldPlaces::Build()`, and deduplicated small-hub `[min, max]` / large-hub IQR quest-level ranges in [`option_facts.cc`](../server_module/src/options/option_facts.cc).
  - *`B2` Pure-harness equip options & stall detector:* [`option_executor.cc`](../server_module/src/options/option_executor.cc) shares internal-linkage `PickEquipSlot` (preferred + alternate dual-slot fallback for rings, trinkets, 1H dual-wielding, Titan's Grip 2H dual-wielding, and Unique-Equipped swaps), ranks equippable non-grey bag gear by `ilvl - equipped_ilvl` delta (cap 8) with factual slot/quality/subclass/ilvl/full-DPS/Armor/stats/BoE + verified `SwapItem` in `ExecuteEquipItemOption`; [`plan_runner_unittest.cc`](../harness/src/plan/plan_runner_unittest.cc) tests `"equip:"` completion; [`stall_check.py`](../tools/bench/stall_check.py) uses exact `trace-events.md` event names (`mob_kill`, `quest_reward`, etc.), records full stall durations, and guards logout gaps before progress events (`stall_check_test.py` `3/3`).
  - *`B3` Pull danger (social packs):* [`nearby_builder.cc`](../server_module/src/snapshot/nearby_builder.cc) populates `u.pack.size` strictly for living hostile attackable NPCs of mutually friendly factions (`IsFriendlyTo`) within `15 yd` when no explicit `CreatureGroup` exists. Verified via `tools/wlb check` (`ALL CHECKS PASSED`, `245` harness / `126` module / `60` common tests) and `tools/wlb build server --test` (server-module C++ edits are compile/source-verified; live 4 h soak pending).

- 2026-11-22: **User-approved `scorer.sha256` relock (`6983fb57e2b7...`, `M3d`):**
  added [`tools/bench/notes_hill_climb.py`](../tools/bench/notes_hill_climb.py) and [`tools/bench/bench_bosses.py`](../tools/bench/bench_bosses.py) to `LOCKED_PATTERNS` in [`tools/bench/hill_climb.py`](../tools/bench/hill_climb.py) (and [`hill_climb_test.py`](../tools/bench/hill_climb_test.py)), ran user-approved `hill_climb.py lock-scorer --force-relock` (`30` locked files), and updated [`config/scorer-lock.sha256`](../config/scorer-lock.sha256) (`6983fb57e2b7...`, verified via `hill_climb._verify_all_locks()` and `hill_climb_test.py` `8/8`).

- 2026-11-22: **Comprehensive review fixes (`6cfed30`, `52c4c0c`, `458047b`, `c046bd5`, `c3daf8c`) & Review `c3daf8c` Low 1–3 polish:**
  - *Server module & acts (`H1`, `M6–M8`, `L-Act1–3`, `L-Mod1–5`):* heap-allocated `BlockAutoInviteListener` per `Engine` with live offline→relogin→snapshot verification in `e2e_invite.py` (`PASS 10.0 s`); named `A5` assist options after the mob; queued rate-limited outbound chats (`<= 4`) with tracked `command_id` errors (waking brain without stopping active `PlanRunner`, verified in `BotLoopTest.ChatErrorWakesBrainWithoutStoppingActiveRunner`); suppressed self-cancelled movement wakes and cleared standing orders + `STOP/MOVING` on instance exit (`BotLoopTest.LeavingInstanceClearsOrdersAndSelfCancelDoesNotWake`); multi-target `AvoidedTargetsByBot()` cleared on combat exit/disconnect; 60 s invite auto-decline, dead bots hear `say`/`yell`, script-blocked chat error, cached hidden-trigger LoS raycasts + `IsHostileTo`, and safe `ObjectGuid`/duration-based item-use aura check (`had_aura_before`, no raw `Aura*` or immutable `GetApplyTime()`).
  - *Boss notes, grammar & `C2` hill-climb (`H2`, `M3a–c`, `M9–M10`, `L-Gram1–4`, `Rc046bd5-M1/L1`):* fixed 5 boss-note contradictions (`ICC`/`Uld`/`PoS`) and added `SelectRelevantNotes` (fast-path `<= 2,000 B`, boss-heading selection without `self.unit.target`, synced `compiled_orders_cache_`); exact GBNF upper-bound decimals, `VX-001` digit preservation, real C++ `orders.wlb` test across all 41 `notes.md` files, and mistyped `orders:` rejection; `notes_hill_climb.py` now requires `confirm_runs >= 1` against a 16-run baseline, uses Bonferroni-corrected Fisher + exact two-sample Mann–Whitney permutation test on pooled deaths (handling 0-death ties), pools both `cand_trace_dir` and `conf_trace_dir` in `summarize_fight_timeline`, enforces fixed `n`, adds optional `--held-out-boss` guard, and logs flags + script SHA-256 + HEAD (`notes_hill_climb_test.py` `10/10`).
  - *Tools & remote workers (`M5`, `L-Tool1–5`, `Rc046bd5-H1`):* raw `/`-split repo validation, shell metachar rejection, `--protect-args`, remote sentinel/`$HOME` guard, remote `flock -o` with exported `WLB_PGID=$$` written to `pgid_rel` before clearing `waiter_rel` (locally tested against real `setsid`+`flock` subprocesses in `remote_test.py` `24/24`); `db_start(allow_init=False)` + `skip-name-resolve`; `post_commit_review.py` `--sandbox`/amend/anchored `FAILED` (`5/5`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and `tools/wlb build server --test` (server-module C++ edits other than `e2e_invite.py` are compile/source-verified).

- 2026-11-22: **Review `10352f7` fixes (Low 1–2):**
  clarified in [`ICC/notes.md`](../content/notes/ICC/notes.md) that `Ball of Flames Visual` covers the unempowered orb (`38332`) while empowered orbs pulse `Empowered Flare` (`10 yd` soak, `L1`); fixed the `2da0350` → `10352f7` SHA reference and labeled `entry 38332, display_id 26767` in `docs/TODO.md` (`L2`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`).

- 2026-11-22: **Review `2da0350` fixes (Medium 1–3, Low 1–3):**
  updated [`ICC/notes.md`](../content/notes/ICC/notes.md) to `Ball of Flames Visual within 12 -> away Ball of Flames Visual 16` under `## Blood Prince Council` (matching `InspectTriggerHazard` on invisible-display `Ball of Flame`, entry `38332`, `display_id 26767`, `M1`), [`Kara/notes.md`](../content/notes/Kara/notes.md) to `Netherspite Infernal within 20 -> away Netherspite Infernal 23` (clearing `Hellfire Effect`'s `20 yd` radius, `M2`), and [`SSC/notes.md`](../content/notes/SSC/notes.md) to remove the non-firing `Whirlwind cast` order on instant-cast Leotheras `37640` (`M3`); updated `test_all_content_notes_standing_orders_valid` in [`notes_hill_climb_test.py`](../tools/bench/notes_hill_climb_test.py) to split compound `&` conditions and unit-test both `cast on boss` and compound `on boss` (`L1`); corrected `"6th overall"` in [`2026-11-22-phase1-phase3-five-boss-n16.md`](benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md) and `docs/TODO.md` (`L2`), preserved the 4 open Phase 1–3 caveats in `## Loose ends`, and recorded `var/bench_boss_raids_360_nonllm.json` Marrowgar (`no_brain 1/2`, `baseline 2/2`, `L3`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`).

- 2026-11-22: **Review `e832542` fixes (Medium 1, Low 1–3) & cleared `## In progress`:**
  switched permanently unselectable hazard NPCs `Netherspite Infernal` ([`Kara/notes.md`](../content/notes/Kara/notes.md)) and `Ball of Flame` ([`ICC/notes.md`](../content/notes/ICC/notes.md)) from `target nearest` to `within 12 -> away ... 16` (`M1`; further refined in `10352f7` above); updated `test_all_content_notes_standing_orders_valid` in [`notes_hill_climb_test.py`](../tools/bench/notes_hill_climb_test.py) to use `nhc.extract_standing_orders` (covering inline orders and language-tagged fences) and allow valid `<Spell> cast on boss` (`L1`); resolved conflicting `Whirlwind cast` rules in [`SSC/notes.md`](../content/notes/SSC/notes.md) and moved `Gravity Lapse` under `## Kael'thas Sunstrider` in [`TK/notes.md`](../content/notes/TK/notes.md) (`L2`); cleared completed Phases 1–3 from `## In progress`, amended the `711c8c7` `Done` bullet below, and clarified `"3rd Phase 1–2 sign-off 5-boss evaluation (6th overall)"` in [`2026-11-22-phase1-phase3-five-boss-n16.md`](benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md) (`L3`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`).

- 2026-11-22: **Review `711c8c7` fixes (Medium 1–3, Low 1) & `n = 16` benchmark report:**
  - *`content/notes/` table-separated validation & prose sync (`M1`, `M2`, `L1`):* fixed the 5 wrong-table orders (`BWL` `Corrupted Fire Nova Totem V` / `Corrupted Stoneskin Totem VI` / `Corrupted Windfury Totem III`, `SSC` `Coilfang Guardian`, `ICC` `Gas Cloud within 10 -> away Gas Cloud 14`), removed inverted/untargetable/duplicate orders (`OS` `Safe Area`/`Twilight Fissure`/`Flame Tsunami`, `RS` `Orb Carrier` + duplicate `Flame Breath`, `Nex` `Spike`/`Crystal Spike`, `Ramp` `Tremor Totem`, duplicate `Uld` `## Dark Rune Acolyte`), and synced prose bullets 1:1 with validated standing orders across all 36 auto-extracted files (`0` `hand-check`/stale draft prose, `141` orders, `0` strict DBC/SQL table errors); added `test_all_content_notes_standing_orders_valid` in [`notes_hill_climb_test.py`](../tools/bench/notes_hill_climb_test.py) so `tools/wlb check` validates all 41 `content/notes/*/notes.md` files automatically.
  - *Benchmark report & `TODO.md` precision (`M3`, `L1`):* added [`docs/benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md`](benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md) with full `m = 15` per-boss and `m = 3` pooled Holm–Bonferroni tables (`p_holm3 = 0.0326` vs `no_brain`, `p_holm15 = 0.0135` on Ikiss), explicit Kael'thas DPS waiver, and Murmur/raid caveats; fixed `## Next, in order` list numbering (`1.–4.`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`).

- 2026-11-22: **Phase 1–3 sign-off benchmark (`n = 16`, `320` valid trials, `0` invalid) & initial 41/41 `content/notes/` audit (`711c8c7`):**
  - *Single-codebase `n = 16` 5-boss benchmark at `d5b8516` (`var/bench_boss_p12_signoff_v3_n16.json`, `mixed_all`, `parallel=4`):* `llm` **`52/80` (`65.0%`)** pooled kills beats `no_brain` `35/80` (`43.8%`, **Fisher `p_raw = 0.0109, p_holm3 = 0.0326`**), and numerically leads `llm_no_notes` `43/80` (`53.8%`, `p_holm3 = 0.1977`, n.s.) and hand-written `baseline` `40/80` (`50.0%`, `p_holm3 = 0.1564`, n.s.); **Ikiss** `12/16` vs `no_brain` `2/16` (**`p_raw = 0.0010, p_holm15 = 0.0135`**, `-2.7%` DPS), **Loken** `14/16` (`0.0` med deaths, `-2.7%` DPS), **Devourer** `7/16` (`+5.3%` DPS), **Murmur** `10/16` (`-3.0%` DPS), **Kael'thas** `9/16` (`1,708.4` DPS vs `baseline` `1,548.0` DPS / `12/16` and `llm_no_notes` `2,142.1` DPS / `4/16`, `p_raw = 0.0659` vs `no_brain` `3/16`).
  - *`content/notes/` audit:* cleaned all 36 remaining draft `content/notes/*/notes.md` files (`150` orders initially; 5 wrong-table orders, untargetable hazards, and prose sync finished + automated `tools/wlb check` notes test added in `e832542` and the follow-up above).

- 2026-11-22: **Review `0a7d8bd` fixes (Medium 1, Low 1):**
  unified `"trigger:<guid>"` resolution in [`act_executor.cc`](../server_module/src/options/act_executor.cc) by scoping `ResolveTriggerHazard(Player*, const pb::Command_Act&)` to `THING_UNSPECIFIED`/`IT` or `NAMED`/`NEAREST_NAMED` matching `thing_name` through `ResolveHiddenTriggerByGuid` (`M1`), and changing `required_name` to `std::optional<std::string_view>` so empty `thing_name` never bypasses name validation (`L1`). Rebuilt `out/server-test` and passed `tools/wlb check` (`ALL CHECKS PASSED`; `act_executor.cc` changes are compile/source-verified only).

- 2026-11-22: **Review `b3fe09e` fixes (Medium 1–2, Low 1–3):**
  restricted `"trigger:<guid>"` lookup in [`act_executor.cc`](../server_module/src/options/act_executor.cc) `ResolveThing` to `act.thing == IT` or `NAMED`/`NEAREST_NAMED` matching `hz->name` (`M1`), removed the unreachable duplicate GUID lookup in `case IT:` and cleared `by_name.it_guid` before recursing (`L1`); updated `ScreenRendererTest.RendersNearbyAreasNearestFirst` in [`screen_renderer_unittest.cc`](../harness/src/screen/screen_renderer_unittest.cc) with `dist_yd = 1200.0f` to pin the `infinity()` sentinel (`L2`); fixed the transposed Kael'thas duration arm order (`93.8 s vs 70.5 s`, wipes `94.1 s vs 65.5 s`, `M2`) and added compile/source-only caveat on `a1b6dfe` below (`L3`). Rebuilt `out/server-test` and passed `tools/wlb check` (`ALL CHECKS PASSED`; `act_executor.cc` changes are compile/source-verified only).
