# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-23.

## In progress

**Full-codebase review `7727a71` findings (6 High, 11 Medium, 7 Low — fix & verify one by one, High → Medium → Low):**

*High:*
- [x] **H1 (`common/base/threading/thread.cc:53,76-88`, `common/base/task/task_queue.cc:40-58`):** Fixed — `TaskQueue` now tracks `active_wakeups_` with `wakeups_drained_` `ConditionVariable` and `SetWakeupCallback` drains all active wakeups before returning (`ThreadTest.StopWithConcurrentPostTask`).
- [ ] **H2 (`tools/wlblib/remote.py:287-289,331-335,478-481`, `tools/wlblib/remote_test.py`):** `rsync -s` breaks `~/` remote paths — passing `-s` (`--protect-args`) disables remote shell tilde expansion, so configured repos under `~/...` fail or hit literal `./~/` directories while `remote_test.py` only mocks argv. Remove `-s` (shell-quoting safe paths) or resolve `~` to an absolute `$HOME` path remotely first, and test with a real `rsync` + local `sh -c` SSH wrapper.
- [ ] **H3 (`tools/wlblib/cases/soak.py:167-169,300-335`, `tools/bench/hill_climb.py:610,648-668,794-827`):** Multi-run soak overwrites traces and `score-candidate` bypasses gates — `_run_once` wipes `var/soak_traces` each iteration and `_aggregate_results` only preserves `results[-1]`'s `run_stats`/`candidate_id`, while `cmd_score_candidate` omits human-baseline, confirmation, and held-out gates (allowing `"promotable": true` under `no_human_baseline`). Store per-run traces cleanly / merge all runs' traces and `run_stats` before scoring, and require human baseline, confirmation, and held-out checks in `score-candidate`.
- [ ] **H4 (`content/notes/Uld/notes.md:5,8`, `harness/src/plan/orders.cc:173-256`, `orders_unittest.cc:447`):** `Whirlwind cast -> out of it` cannot be emitted — missing a comma between `**Dark Rune Sentinel** **Whirlwind**:` stops `ExtractNamesFromNotes` from extracting `"Whirlwind"`, so the `.wlb` grammar blocks `Whirlwind cast`; `orders_unittest.cc:447` passed only because it constructed an empty `Env{}`. Separate bold terms cleanly in `Uld/notes.md` and validate all `content/notes/*/notes.md` files against `ExtractNamesFromNotes` in tests.
- [ ] **H5 (`content/notes/ICC/notes.md:35,39`):** `in Vile Gas -> out of it` never fires — Rotface's `SPELL_VILE_GAS_H` (`69240`) applies a player debuff aura rather than spawning a ground hazard / `DynamicObject`, so `in Vile Gas` never matches any snapshot `area_effect`. Remove the dead ground-zone rule (until debuff triggers are supported).
- [ ] **H6 (`content/notes/{BT,Hyjal,Mag,RS,OS,BWL,ZA,Ony}/notes.md`):** Boss orders issue raid-wide `out of it` on wrong/impossible targets — `BT` (`Shadow of Death`) and `Hyjal` (`Air Burst`) target single random non-tank players, `Mag` (`Quake`) hits room-wide, `OS`/`RS` target instant `Cleave` (never in `CASTING`), and dragon breath rules (`RS`, `OS`, `BWL`, `ZA`, `Ony`) also move the main tank (spinning the boss) or treat Onyxia's lane `Breath` as radial. Restrict `out of it` to boss-centered avoidable casts (removing mis-modeled rules until debuff-target or non-tank cone semantics exist).

*Medium:*
- [x] **M1 (`common/net/unix_socket_client.cc:121-137`, `common/net/unix_socket_server.cc:118-125`, `common/base/functional/callback.h:81-83`):** Fixed — `UnixSocketClient::OnConnectionClosed` now calls `ScheduleRetry()` before `on_disconnected`, both client and server move `unique_ptr<FramedConnection>` into the deferred cleanup `OnceClosure`, and `RepeatingCallback::Run` pins `impl_` locally (`CallbackTest.RepeatingCallbackCanDestroySelfDuringRun`, `UnixSocketTest.ClientCanDestroySelfInOnDisconnected`).
- [x] **M2 (`common/base/synchronization/lock.cc:12-28`):** Fixed — `Lock` now checks `== 0` and calls `std::abort()` on non-zero return values from `pthread_mutexattr_*`, `pthread_mutex_init`, `pthread_mutex_lock`, and `pthread_mutex_unlock`.
- [x] **M3 (`common/base/threading/platform_thread.cc:60-66`):** Fixed — `PlatformThread::Sleep` returns early for `!duration.is_positive()` and retries `nanosleep` only when `errno == EINTR` (`PlatformThreadTest.SleepNonPositiveReturnsImmediately`).
- [ ] **M4 (`server_module/src/bridge/brain_bridge.h:92,113`, `common/base/memory/raw_ptr.h:115-118`):** `base::raw_ptr<IoCore>` triggers UB / fails GCC 15 — `IoCore` is forward-declared in `brain_bridge.h`, forcing callers to instantiate `raw_ptr<IoCore>::Counted() -> std::is_base_of_v<RawPtrTarget, IoCore>` on an incomplete class (C++ UB `[meta.rel]/2`, compile error on GCC 15). Define `IoCore` before `raw_ptr<IoCore>` or wrap it in a complete handle type.
- [ ] **M5 (`server_module/src/snapshot/behavior_trace.h:11,15,19,249-258`, `behavior_trace.cc:380,389,423,587,598`, `config/config_parsing.cc:7,16`):** `CONTRIBUTING.md` C++ bans violated — `BehaviorTraceWriter` uses `std::mutex`, `std::condition_variable`, `std::thread`, `std::lock_guard`, and `std::unique_lock` without `GUARDED_BY` or a deviation comment (bypassing `-Werror=thread-safety`), and `config_parsing.cc` uses banned `std::stringstream`. Port `BehaviorTraceWriter` to `base::Lock` (`GUARDED_BY`), `base::ConditionVariable`, and `base::Thread`, and use `base::SplitString` in `config_parsing.cc`.
- [ ] **M6 (`server_module/src/snapshot/panels_builder.cc:181-187`):** `TypeWord` armor subclass table is shifted — `kArmor[]` puts `""` at 4 (`ITEM_SUBCLASS_ARMOR_PLATE`), `"Plate"` at 5 (`BUCKLER`), `""` at 6 (`SHIELD`), `"Shield"` at 7 (`LIBRAM`), and `"Relic"` at 11 (`MAX_ITEM_SUBCLASS_ARMOR`), dropping `"Plate"`/`"Shield"` and mislabeling Bucklers/Relics. Align `kArmor[]` indices with upstream `enum ItemSubclassArmor` (`ItemTemplate.h:388-403`).
- [ ] **M7 (`server_module/src/options/act_executor.cc:1789-1806`):** `Command_Act::EQUIP` ignores `SwapItem` failures — `EQUIP` calls `CanEquipItem` (omitting `CanUnequipItem` and bag-store/offhand checks) and returns `{.done = true}` unconditionally after `SwapItem`, even when `SwapItem` aborts after sending an equip error. Route `EQUIP` through `WorldSession::HandleAutoEquipItemOpcode` or verify `bot->GetItemByPos(dest) == entry.item` afterwards (as `option_executor.cc:1703-1713` does).
- [ ] **M8 (`server_module/src/options/act_executor.cc:1647-1650`):** `Command_Act::JUMP` creates a 1 ms orientation-only spline — calling `MoveJump` with the bot's own `(x, y, z)` yields `SegLength == 0`, so upstream `MoveSplineInit::Launch()` sets `isOrientationOnly = true` and clamps duration to 1 ms instead of jumping. Emit a genuine jump movement update (`MSG_MOVE_JUMP`) or non-zero airborne jump path instead of self-coordinate `MoveJump`.
- [ ] **M9 (`docs/specs/snapshot.md:92-93`, `harness/src/runtime/bot_loop.cc:163-170,231-237,371-378`, `harness/src/plan/triggers.cc:473-477`):** Spec/code mismatch on repeated `Snapshot::events` — `snapshot.md:93` says `Snapshot::events` repeats events already sent live via `ImmediateEvent`, while `BotLoop` processes both `OnEvent` and `OnSnapshot(events)`. Either update `snapshot.md` to say `Snapshot::events` is not re-sent live, or deduplicate event IDs across `OnEvent` and `OnSnapshot`.
- [ ] **M10 (`tools/bench/hill_climb.py:672-676`, `tools/bench/blind_rate.py:142-147`, `tools/bench/pairwise_ranker.py:296`):** Unanchored blind Bradley-Terry ratings — `blind_rate.py` excludes solo human clips unless `--include-human-anchors` is passed, `hill_climb.py` omits that flag, and `fit_bradley_terry` silently falls back to `ref = 0.0` around `1000.0` when `anchor_label` is missing. Pass `--include-human-anchors` when an AI/external judge or multi-human pool is present, and explicitly mark/fail missing anchors in `fit_bradley_terry`.
- [ ] **M11 (`docs/benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md:66`, `content/notes/Hyjal/notes.md:11-16`, `content/notes/Nex/notes.md:3-9`):** Benchmark label and encounter-note mismatches — `five-boss-n16.md` labels Murmur baseline as "tightly scripted C++" despite `strip_strat=""` (`bench_bosses.py:227`), `Hyjal/notes.md` uses `Stomp` vs Kaz'rogal's `SPELL_WAR_STOMP = 31480`, and `Nex/notes.md` only matches `## Horde Commander` (missing `Alliance Commander` / `Commander Stoutbeard` / `Commander Kolurg`). Correct the Murmur baseline description, verify Kaz'rogal's exact DBC spell name, and add faction/heroic headings to `Nex/notes.md`.

*Low:*
- [ ] **L1 (`harness/src/runtime/bot_loop.cc:236-237,319-334,1150-1166`, `harness/src/plan/triggers.cc:481-486`):** Edge `WakeTrigger` dropped during order refusal backoff — `EvaluateSnapshot` updates edge state at `:237`, then `:320` returns early whenever `!ord.wake_reason.empty()` even if `WakeForOrder` deferred to the backoff timer without waking. Check backoff deferral before dropping `trig`, or accumulate the trigger's wake reason onto `pending_reasons`.
- [x] **L2 (`common/base/message_loop/message_pump_epoll.cc:107-125,146-167`):** Fixed — `WatchFileDescriptor` stops any prior watch when `controller` switches `fd` or `pump`, and `Dispatch` handles both `WATCH_WRITE` and `WATCH_READ` (guarded by controller generation) before one-shot removal (`MessagePumpEpollTest.OneShotReadWriteAndControllerReuse`).
- [ ] **L3 (`harness/src/llm/llm_client_unittest.cc:5,12,135,149-150,165`):** Banned `<chrono>`/`<thread>` in test helper — `FakeLlamaServer` uses `std::this_thread::sleep_for(std::chrono::milliseconds(...))` instead of `base::PlatformThread::Sleep(base::Milliseconds(...))`. Switch to `base::PlatformThread::Sleep` and drop `<chrono>`/`<thread>`.
- [ ] **L4 (`server_module/src/snapshot/panels_builder.cc:306-313,328-330`):** `BagItem` reads spell charges from slot 0 — `UseSpell` can match `ITEM_SPELLTRIGGER_ON_USE` at index `i > 0`, but `BagItem` calls `item->GetSpellCharges()` (defaulting to `0`), unlike `option_executor.cc:668-669,1562-1563`. Pass the matched spell slot index to `item->GetSpellCharges(idx)`.
- [ ] **L5 (`tools/bench/hill_climb.py:368-370,587,767-769,863-865`):** `--auto-revert` runs `git reset --hard` + repo-wide `git clean -fd` — reverting a failed round checks out `best["commit"]` and cleans untracked files across the entire repo, risking loss of unrelated work or scratch files. Refuse `--auto-revert` when unrelated files are dirty/untracked, or scope revert/clean to `ALLOWED_EDIT_GLOBS` (`git restore`/`clean --`).
- [ ] **L6 (`tools/bench/soak_scorer.py:301-302,408-410`, `tools/bench/hill_climb_test.py`):** Unwired `offline_eval` guardrail and untested CLI subcommands — `hill_climb.py` never passes `min_eval_passed`/`eval_passed` into `score_soak`, and `hill_climb_test.py` covers helper functions without exercising CLI subcommands (`init`, `eval-candidate`, `score-candidate`, `auto-loop`, `calibrate`). Wire `wlb test eval` pass counts into `score_soak` or remove dead params, and add stubbed CLI tests for subcommands.
- [ ] **L7 (`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md:73,97,100`, `docs/specs/harness.md:31`, `harness/src/main.cc:6,45`, `docs/plans/self-improving-loop.md:241,251`, `README.md:51,53`, `docs/specs/options.md:125,147-148`):** Minor stats/doc drift — two Fisher `p_raw` values (`0.716` vs `0.722`, `0.219` vs `0.221`) and Loken `"+83%"` phrasing drift slightly on recomputation; `harness.md`/`main.cc` still say `<snapshot.pb>`, `README.md` omits `rules`/`bench_tests`, `options.md` omits `place:quest:...`, and `self-improving-loop.md` says `28` locked files vs `30` in `config/scorer-lock.sha256` plus a hardcoded `/home/will/` path. Update doc counts/flags and re-run the table calculation script.

**Deferred (touches locked `scorer.sha256` files — user granted relock permission 2026-11-23):**
- [ ] **B1-POI-fallback & `"equip:"` guard (`snapshot_builder.cc`, `llm_brain_world_script.cc`):** `quest_poi` → `creature`/`gameobject`/`loot` spawn fallback (`499` quests in `atlas.sqlite` lack any `quest_poi_spots`; `547` lack turn-in POI; `2,717` have objectives lacking POI), plus explicit `target.guid.starts_with("equip:")` guard in `OptionTargetProgress` (currently safe because `ExecuteEquipItemOption` is synchronous `used=true`, but explicit guard is cleaner).

## Next, in order

**Priority direction (2026-11-21):** Mechanical WoW competence (dungeons, raids/ICC, clearing quests,
soloing elite quests, smart gearing) takes priority over `"human-like"` mimicry (`D`). **Phases 1–3
are signed off** (see [`docs/benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md`](benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md) §4: `llm` matches/slightly leads hand-written C++ boss scripts generically without boss-specific C++; real ceilings are Playerbots' class AI, `gemma-4-26B-A4B-it` (26B MoE / 4B active) order reliability, and generic movement vs. hardcoded room landmarks). **Phase 4 (`B1–B3`) core is implemented; live soak verification & Phase 5 (`C4`) are next.**

1. **Phase 4 live validation (4 h soak & quests/hr):** run live open-world soak with `var/knowledge/world_knowledge.json` + [`tools/bench/stall_check.py`](../tools/bench/stall_check.py) to verify zero `>= 5 min` stalls, outleveled-zone travel, quest turn-ins, and gear upgrades in live play.
2. **Phase 5 — Model bake-off (`C4`) & fine-tuning exploration (`C3`, Decision #3: keep as TODO)**
   *(Done when: a significant bench gain at the same latency)*:
   - **`C4. Model choice bake-off`:** Try at least one other base model on `tools/wlb test eval`,
     `perf`, and the boss bench (same 4 arms, `n = 16`) before investing in `C3`, so the base model
     choice is settled with data. Candidate GGUFs pre-downloaded in `models/`:
     `gemma-4-E4B-it-qat-IQ4_XS-imatrix.gguf` and `Qwen3.8-27B-IQ4_XS.gguf`.
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
