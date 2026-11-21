# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-21.

## In progress

(none)

## Next, in order

- **Where this leaves the project — boss notes & selective-dodge acts**
  ([`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md)):
  1. Notes reliably change behaviour: bots dodge mechanics they're told about.
  2. They don't yet reliably change kill rates. Only Ikiss hints at it, and it doesn't survive
     multiple-comparison correction.
  3. Dodging costs DPS. On Loken, `llm` did `11.5k` vs `15.5k` for `llm_no_notes`.
  4. The playerbots scripts are still better at the mechanics they cover (`Flame Strike`).
  The next gains are in the notes and the acts (dodge only when it's worth it, then return to DPS),
  not in the bench. Drop `Devourer` and `Loken`, or lower their gear, so every boss can show a
  difference.

- **Human baseline coverage & initial blind-rating batch** (`docs/plans/self-improving-loop.md`):
  layered scorer (Steps 1–6) is built and live-verified, and General/Say/Party chat + quest
  grouping are wired. Before running real tuning rounds: record additional human play sessions
  under the new 5 Hz + events format across all situations (questing, fighting, looting, town,
  dungeons, chat; ideally a few players, ~30% held out) and run an initial ~30-pair
  `tools/bench/blind_rate.py` batch + `llm-agree` check.
- **Named room landmarks** (`docs/specs/triggers.md`): 132 of the 218 unsayable boss actions move
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
- **Panels, things not built yet** (`docs/specs/options.md`): no `Use <item>` option
  for bag items (and thus no spec §10 use-item quest eval case yet: needs item target selection for
  unit/object/self quest items); the module sends no `quest_accepted` / `quest_complete` /
  inventory-full events, so the habits read the snapshot instead (`bag_free_slots`, the quest
  tracker); "open bags at a vendor" waits for vendor windows (`interaction`); hover only for items
  and quests (spells, auras, units, places later).
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
  - Chaining `away 25, then to Loken` on a 4 s cast finishes `away 25` in ~3.0–3.5 s and immediately
    runs back into the blast before the cast finishes (two separate orders or waiting for the cast
    to end avoids returning early).
  - Mid-fight wakes (`attacked`, `hp<N`) replace all standing orders; Gemma 4 occasionally rewrites
    a working `Lightning Nova cast` order into `Loken cast` or `Lightning Nova appears`.
- **Not individually live-triggered after final edits yet:** `llmtest questcomplete` when a quest
  is already active in a bot's quest log (`CompleteQuest` + `RewardQuest` branch in
  `test_commands.cc`, source-verified), plus Review 10 single-arm `_wait_checkstrat` +
  `+{strip_strat}` (`tools/bench/bench_bosses.py`) and `O(1)` cap eviction
  (`server_module/src/snapshot/behavior_trace.{h,cc}`), which are unit-tested and compiled into
  `out/server-test` without starting a new live benchmark run (per user instruction).
  *(Environmental/self death `died` with `detail="environment"` caveat closed per Review 9 —
  live-triggered on `murmur`, and entrance staging/pull coordinates for `ick`, `murmur`, and
  `ikiss` live-verified across `32` valid trials each.)*
- **Deliberately skipped benchmark speedup proposals (Items 4, 5, adaptive 6):** left unbuilt on
  purpose to preserve valid boss immunity/kite phases (Item 4: `20 s` zero-damage abort would
  cut `Gravity Lapse`/`Planar Shift` and censor `llm` bugs via survivorship bias), cross-boss DB
  isolation + startup-bound `LlmBrain.TraceDir` (`scorer.sha256` lock) (Item 5: cross-boss
  server reuse without `test_databases_reset()` saves only `~44 s/boss`), and unbiased fixed-`n`
  statistics (adaptive Item 6: stopping at `n=3` has a `42.2%` false-negative rate on a `75%` arm;
  details in [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md) §6).

## Done (newest first, keep about ten)

- 2026-11-21: **Fix `/dev/tty` `"r+"` relock error & user-approved scorer relock (`d6704ae5f8a1...`):**
  fixed `open("/dev/tty", "r+", encoding="utf-8")` raising `io.UnsupportedOperation` (non-seekable
  character device) in [`tools/bench/hill_climb.py`](../tools/bench/hill_climb.py#L232-L274)
  `_confirm_relock` by opening separate `"r"` and `"w"` handles, and replaced the `io.StringIO` mock
  in [`tools/bench/hill_climb_test.py`](../tools/bench/hill_climb_test.py#L92-L212) with a real
  `pty.openpty()` slave character device. User interactively ran `hill_climb.py lock-scorer
  --force-relock` over SSH, updating `~/.local/share/wow-llm-bots/scorer.sha256` and
  [`config/scorer-lock.sha256`](../config/scorer-lock.sha256) (`d6704ae5f8a1...`); verified
  `hill_climb._verify_all_locks()` (`all 28 measurement locks verified`) and `tools/wlb check`.

- 2026-11-21: **Review 10 fixes (Medium + Lows 1–3) & "Where this leaves the project" added to TODO:**
  restructured [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md)
  to lead with behaviour/mechanic results (`m = 24` Holm–Bonferroni `p_holm = 0.00039–0.0329` for
  `Murmur` `Sonic Boom` / `Ikiss` `Arcane Explosion` dodges and `Kael'thas` `Flame Strike` script
  superiority `p_holm = 0.0010`), added `p_holm` across all `30` pairwise kill tests (`Bonferroni
  0.00167`), reworded `Ikiss` kill gains (`p_raw = 0.0155`, `p_holm = 0.465`) as suggestive only,
  and documented cross-rerun `3–5` kill variance + `Devourer`/`Loken` `16/16` ceilings + DPS cost of
  dodging (`Loken` `11.5k` vs `15.5k`); [`tools/bench/hill_climb.py`](../tools/bench/hill_climb.py)
  removes `WLB_CONFIRM_RELOCK` completely, adds `/proc/<pid>/cgroup` agent detection
  (`_has_agent_cgroup`) catching `setsid`/double-fork reparenting, and requires `/dev/tty` `'RELOCK'`
  only outside agent runs (`CONTRIBUTING.md` & `.agents/rules/contributing.md` updated; tested in
  [`hill_climb_test.py`](../tools/bench/hill_climb_test.py) incl. `ppid=1` cgroup and live
  double-fork + `os.setsid()` + `env -i`); [`bench_bosses.py`](../tools/bench/bench_bosses.py) adds
  `+{strip_strat}` for single-arm `baseline` and polled `_wait_checkstrat` (tested in
  [`dodge_timeline_test.py`](../tools/bench/dodge_timeline_test.py));
  [`behavior_trace.{h,cc}`](../server_module/src/snapshot/behavior_trace.cc) replaces full-map
  cap-eviction scans under `mu_` with `O(1)` sorted intrusive `all`/`idle` index lists in
  `RoleSampleTracker` (tested in `EvictsIdleTrackedCharactersAndReportsCapOverflow` with `1,000`
  overflow samples & out-of-order stale eviction). Verified via `tools/wlb check` (`ALL CHECKS PASSED`:
  `60 + 119 + 224` C++ tests and `4 + 7 + 9 + 15 + 5` Python tests) and `build.server(test=True)`
  without starting a new live benchmark run (per user request; `scorer.sha256` relock kept open in
  `In progress` awaiting explicit user OK).

- 2026-11-21: **Single-commit 5-boss × 4-arm × 16-runs benchmark (`320` valid trials at `d7b3e13`)
  & multi-wave slice-isolation fix:** replaced `ick` (`8/8` ceiling) with `devourer` (`wotlk-fos`,
  with `DEVOURER_NOTES` `Mirrored Soul on anyone` grammar fix) + `loken` (`wotlk-hol`) in
  [`tools/bench/bench_bosses.py`](../tools/bench/bench_bosses.py); moved `loken` `spawn_xyz` to
  `(1170.0, 33.83, 60.81)` (`16.47 yd < 20.0 yd` `detection_range`, eliminating `38` Wave 2–20
  `no_pull`s in superseded `f553eba`), and fixed [`_slice_traces_after`](../tools/bench/bench_bosses.py#L662-L677)
  in `_wait_fight` + scoring to slice from `stage_ms` with `inst == party_instances[p_idx]`
  (eliminating `-5000 ms` cross-wave `mob_kill` bleed across `~1.1 s` fast resets that aborted
  slower `llm` pulls in superseded `e5297af`, verified by
  `test_slice_traces_after_excludes_prior_wave_kill_across_fast_reset` in
  [`dodge_timeline_test.py`](../tools/bench/dodge_timeline_test.py) `14/14` + 3-wave live check).
  Completed all 5 bosses × 4 arms × 16 runs at commit `d7b3e13` (`320` valid scored trials over
  `81` waves, `110.8 min`, median wave overhead `0.90–1.21 s`, `1` `pre_pull_combat` invalid trial
  on `murmur`, `0` `no_pull`, `0` wave errors): `devourer` & `loken` hit `16/16` across all arms;
  **Ikiss** `llm` `8/16` (`50.0%`) & `llm_no_notes` `6/16` (`37.5%`) vs `no_brain` `1/16` (`6.2%`,
  Fisher `p_raw = 0.0155`, `p_holm = 0.464` across 30 kill comparisons) & `baseline` `2/16`
  (`12.5%`; post-hoc pooled LLM `14/32` vs PB `3/32` `p_raw = 0.0038`, also above `0.0017` —
  **suggestive only**, does not survive multiple-comparison correction, with `3–5` kill swings across
  reruns); `notes.md` standing orders drove significant trial-level AoE dodges on **Murmur**
  (`Sonic Boom >= 34 yd`: `12/16` vs `0/16` everywhere, `p = 1.61e-05`) and **Ikiss** (`Arcane
  Explosion >= 35 yd`: `10/16` vs `1/16` `p = 0.0021` & `0/16` `p = 0.0002`), while `baseline`
  (`tbc-mgt`) dominated `Kael'thas` `Flame Strike` (`14/16 <= 1,000` dmg vs `1–4/16`, `p <= 0.0010`)
  and naive `AWAY` runouts lowered DPS / increased `Loken` distance-scaled `Pulsing Shockwave`
  damage; full report + superseded runs disclosed in
  [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md).

- 2026-11-21: **Review 9 fixes (Medium 1–3, Low 1–4, closed environmental-death caveat; partly — see Review 10 Low 1 & Low 3):**
  [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md)
  splits Cohort A (`523ab4c`: `kaelthas`, `ick`) vs Cohort B (`aac0546`: `murmur`, `ikiss`) with no
  cross-commit pooled row, adds Wilson 95% CIs + exact two-sided Fisher `p`-values across kills and
  independent trial-level AoE dodges/`Flame Strike` damage, reports all discarded pre-`aac0546`
  `murmur`/`ikiss` waves + pre-pull invalidations, documents the pre-first-plan pull handicap on
  LLM arms, and closes the environmental-death caveat in `Loose ends`;
  [`tools/bench/hill_climb.py`](../tools/bench/hill_climb.py) adds `/proc` ancestor detection
  (`_has_agent_ancestor`), drops the leaked token from `_confirm_relock` refusal/prompt messages,
  and tracks/verifies the approved `scorer.sha256` manifest digest in
  [`config/scorer-lock.sha256`](../config/scorer-lock.sha256) (updated docs in
  [`docs/plans/self-improving-loop.md`](plans/self-improving-loop.md),
  [`CONTRIBUTING.md`](../CONTRIBUTING.md), `.agents/rules/contributing.md`);
  [`behavior_trace.{h,cc}`](../server_module/src/snapshot/behavior_trace.cc) +
  [`llm_brain_world_script.cc`](../server_module/src/llm_brain_world_script.cc) raise
  `kDefaultMaxTrackedCharsPerRole` to `4096`, evict drained/stale windows, and warn via
  `TakeUntrackedSampleChars()` when concurrent active cap overflows; updated `devourer`
  `stage_xyz` to Forge of Souls entrance `(4922.86, 2175.63, 639.0)` in
  [`bench_bosses.py`](../tools/bench/bench_bosses.py). Verified via `tools/wlb check`
  (`60 + 119 + 224` C++ tests incl. `EvictsIdleTrackedCharactersAndReportsCapOverflow`; `7/7` in
  [`hill_climb_test.py`](../tools/bench/hill_climb_test.py) incl. `env -i` `/proc` ancestor and
  digest checks), `build.server(test=True)`, and (with explicit user OK) re-locked & verified all
  `28` measurement locks in `~/.local/share/wow-llm-bots/scorer.sha256` and
  [`config/scorer-lock.sha256`](../config/scorer-lock.sha256) (`46de81d26630...`).

- 2026-11-21: **Boss benchmark runner speedups (Items 1–3) & skipped-proposal rationale (partly — see Review 10 Low 2: single-arm `checkstrat` race):** added
  `phase_timings` (`setup_s`, per-wave phases, `teardown_s`, `wall_s`) to benchmark JSON,
  auto-defaulted `--parallel` to `4` in `mixed_all` (constant 10 LLM bots on 16 `llama-server`
  slots, `3` otherwise), and replaced fixed per-command sleeps with 1-in-flight `5 ms`-polled
  tokenized `_console_batch` (preventing GNU `readline` echo on `CliThread` from interleaving
  into `CliHandler::SendSysMessage` output), two-stage leader-then-follower `_wait_where`
  worldport polling, and `1.0 s` fight trace polling (`0.5 s` settle) in
  [`tools/bench/bench_bosses.py`](../tools/bench/bench_bosses.py); documented measured
  wall-clock budget (incl. `no_brain` + `baseline` costing `0` GPU slots and only `+2.2 min`
  across 32 waves) and why Items 4, 5, and adaptive 6 were skipped in
  [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md) §6.
  Verified via `tools/wlb check` (`13/13` in
  [`tools/bench/dodge_timeline_test.py`](../tools/bench/dodge_timeline_test.py), incl.
  `test_console_batch_serializes_commands_to_avoid_readline_interleaving`), all `28` scorer locks
  (`all 28 measurement locks verified`), and live `out/server-test` run
  (`bench_bosses.py --only ikiss --mode mixed_all --runs 2`, auto `--parallel 4`: `8/8` valid,
  `0` invalid, `0` wave errors, fresh instances `[1..4]` & `[5..8]`, `setup_s=44.10 s`, Wave 1/2
  pre-fight overhead `1.05 s`/`1.06 s` vs `47.6 s` median gap previously).

- 2026-11-21: **Multi-wave test-harness fixes & 4-boss × 4-arm parallel benchmark (`128` valid
  trials):** fixed three multi-wave bugs exposed on `murmur` and `ikiss`: in `test_commands.cc`,
  `HandleResetBot` strips `SPELL_AURA_SPIRIT_OF_REDEMPTION` (`27827`) before resurrection +
  re-checks `!IsAlive()` after aura removal (preventing aura-end `JustDied`) and resets playerbots'
  `"death count"` + `sRandomPlayerbotMgr` `"dead"`/`"revive"` (`SetValue(..., 0)`) so ghosts at
  `death count >= 5` never trigger `RandomPlayerbotMgr::Revive` → `LeaveOrDisbandGroup()`,
  `HandleGroup` adds optional `token`, and `HandleMove` clears non-combat attack/cast/selection +
  moves live pets with owners; in `bench_bosses.py`, added `_ensure_party_group` before wave
  resets, `_wait_harness_connected` replacing the `3.0 s` entrance sleep, and `wait_s=0.05` on
  `llmtest move` (eliminating entrance `pre_pull_combat`); updated `dodge_timeline_test.py` (`11/11`
  tests, adding `test_wait_harness_connected_returns_on_log_marker`). Verified `tools/wlb check`
  (402 C++ + 36 Python tests), all `28` scorer locks (`all 28 measurement locks verified`), and
  completed `bench_bosses.py --mode mixed_all --runs 8 --parallel 4` in two 64-trial cohorts
  (`kaelthas` & `ick` at `523ab4c`; `murmur` & `ikiss` at `aac0546`; `32/32` valid per boss, `0`
  invalid, `0` wave errors; Ikiss `6/8` & `6/8` vs `3/8` & `2/8`, suggestive, not significant
  pairwise at `n = 8`: Fisher `p = 0.31` & `0.13`, pooled LLM `12/16` vs playerbots `5/16`
  `p = 0.032` confounded by `baseline` trash pulls; Murmur `Sonic Boom` dodged on `6/7` cast
  trials for `llm` vs `1/8` `llm_no_notes` `p = 0.010` and `0/8` `no_brain`/`baseline` `p = 0.0014`,
  cutting median impact damage 32% `5,323` vs `7,769–7,894`; see
  [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md)).

- 2026-11-20: **Review 8 fixes (New Lows 1–3; partly — see Review 9 Low 1, Low 2, Low 4) + user-approved scorer relock:**
  `tools/bench/bench_bosses.py` narrows setup, outer boss-loop, cleanup, and latency-parse
  exception handling (`except (Failed, RuntimeError)`, `except (Failed, OSError)`,
  `except (json.JSONDecodeError, ValueError, TypeError)`) so bench programming bugs crash loudly
  (tested in `tools/bench/dodge_timeline_test.py` for `Failed`, console `RuntimeError`, and
  uncaught `TypeError`); `tools/bench/hill_climb.py` refuses unconfirmed `lock-scorer` even when
  `scorer.sha256` is missing, removes `cmd_init` auto-lock creation, requires interactive `"RELOCK"`
  or exact `WLB_CONFIRM_RELOCK=RELOCK:<hash12>`, ignores `--lock-file` during agent runs (`cmd_lock_scorer`,
  `cmd_init`, `cmd_eval_candidate`), and updates `CONTRIBUTING.md` / `.agents/rules/contributing.md`
  (tested in `tools/bench/hill_climb_test.py` and CLI refusal checks);
  `BehaviorTraceWriter::TakeMeasuredSampleHz(role, min_intervals)` (`behavior_trace.{h,cc}`) &
  `LlmBrainWorldScript::CheckMeasuredTraceRate` check `"bot"` and `"human"` trace sample rates
  separately matching `trace_parity.check_source_rate_parity` (unit-tested with 10 fast bots vs 1
  slow human, and live-verified on `out/server-test` at `TraceSampleIntervalMs = 290` logging
  `measured bot trace sample rate 3.44 Hz`). Verified via `tools/wlb check` (402 C++ + 35 Python
  tests) and, with explicit user OK, re-locked and verified all 28 measurement locks in
  `~/.local/share/wow-llm-bots/scorer.sha256`.

- 2026-11-20: **Review 7 fixes (New Lows 1–4 + environmental-death caveat restored):**
  removed the misleading `<= 285 ms` config warning and instead measure actual recorded sample rate
  in `BehaviorTraceWriter::TakeMeasuredSampleHz` (`behavior_trace.{h,cc}`) checked by
  `LlmBrainWorldScript` in `LogStats` and `OnShutdown` (warns when measured median rate < `3.5 Hz`,
  accounting for world-tick rounding; updated `llm_brain_config.{h,cc}`, `mod-llm-brain.conf.dist`,
  and `docs/specs/trace-events.md`); updated `CONTRIBUTING.md` & `.agents/rules/contributing.md` so
  agents never force-relock `scorer.sha256` without explicit user OK; isolated pre-wave setup errors
  inside `run_boss_waves` (`tools/bench/bench_bosses.py`) so a setup failure marks that boss
  `failed: True` and moves on; added optional per-call `token` (`tok=<token>`) across `test_commands.cc`
  (`HandleTele`, `HandleGear`, `HandleCheckStrat`, `HandleSetStrat`, `HandleQuestComplete`,
  `HandleResetBot`, `HandleWhere`) and all `_console_bot` calls in `bench_bosses.py` so late replies
  from earlier identical commands (`where`, etc.) can never match. Verified via `tools/wlb check`
  (402 C++ + 34 Python tests, incl. `MeasuresActualSampleHzIncludingTickRounding` and the new token
  + setup-failure tests in `dodge_timeline_test.py`) and two live `out/server-test` checks:
  (1) `TraceSampleIntervalMs = 290` logged `measured trace sample rate 3.45 Hz` matching Python
  `trace_parity.estimate_source_hz`; (2) `bench_bosses.py --only kaelthas --mode mixed_all
  --arms no_brain,baseline --runs 1 --parallel 2` ran with `failed=False`, `invalid_runs=0`,
  `wave_errors=[]`, and `tok=c1..c80` matched on all `gear`/`questcomplete`/`where`/`resetbot`/
  `tele`/`setstrat`/`checkstrat` replies in `worldserver.out`.

- 2026-11-20: **Review 6 fixes + Kael'thas staging/arrival fix + self-check rule (`CONTRIBUTING.md`):**
  `docs/specs/trace-events.md` rewritten to match C++ and `PARITY_ALLOWED_EVENTS` verbatim and
  enforced by `tools/bench/trace_events_spec_test.py` (5/5 tests, wired into `tools/wlb check`);
  `bench_bosses.py` makes `--runs` per arm, keeps wave party count constant via slot-rotating
  `_wave_arms` (including `gcd > 1`), gates default strategy stripping on `arms != ("baseline",)`,
  sends console commands once and polls `worldserver.out` (`_console_bot`), records per-party
  `party_pull_ms`, checks `alive=1` on dungeon arrival, clears strategy overrides at shutdown
  (`llmtest clearstrat` + `BotManager::Stop()`), and stages `ick`, `murmur`, `ikiss`, and
  `kaelthas` at `areatrigger_teleport` dungeon entrances while `llmtest tele` dismounts before
  dungeon teleport (fixing Kael'thas arrival fall deaths and pre-pull trash combat);
  `test_commands.cc` explicitly stores & reports requested `specNo=` and rejects unknown quest IDs in
  `questcomplete`; `behavior_trace.{h,cc}` & `llm_brain_config.{h,cc}` add configurable
  `LlmBrain.TraceMaxFileMb` (64) & `LlmBrain.TraceMaxRotatedFiles` (8) + `LOG_WARN` when
  `TraceSampleIntervalMs > 285` (removing conflicting conf comment); added self-double-check MUST
  to `CONTRIBUTING.md` & `.agents/rules/contributing.md`; re-locked & verified all 28 scorer locks.
  Verified after final edit via `tools/wlb check` (401 C++ + 32 Python tests) and live Kael'thas
  3-arm wave (`0` arrival deaths, `0` pre-pull combat, `0` invalid trials, `baseline` killed
  Kael'thas in `93.3 s` with `KillSelf()` `mob_kill` credited to all 5 party members); earlier live
  checks before final edit also verified Ikiss 3-arm wave, unknown-quest rejection, `tele ... 0`
  entry-rule refusal, and `clearstrat`.

- 2026-11-20: **Review 5 remaining items (M6, L10–L12, L14–L16; partly — see Review 6 above):**
  `harness run` adds `--only-bots=A,B,...` (`run.{h,cc}`, `run_unittest.cc`); `BotManager` adds
  per-bot strategy overrides + `llmtest setstrat` (`bot_manager.{h,cc}`, `test_commands.cc`);
  `bench_bosses.py` adds `--mode mixed_all` and `--arms` to mix `llm`, `llm_no_notes`, `no_brain`,
  and `baseline` in the same wave; `boss_meter.py` delegates `ImpactDodge` to `dodge_timeline`;
  `behavior_trace_scripts.cc` logs environmental/self deaths (`killer == killed`) as non-PvP `died`
  (`detail="environment"`); `LlmBrain.TraceSampleIntervalMs` makes trace cadence configurable
  (volume cap still fixed); `llmtest tele` accepts optional `gm_mode` (0 = enforce entry rules);
  `llmtest questcomplete` completes & rewards active log quests (unknown ids still marked);
  `docs/specs/trace-events.md` added (**inaccurate**, see Review 6 New 1). Live on Ikiss
  (`--mode mixed_all --arms llm,no_brain,baseline --runs 3 --parallel 3`: `llm` killed in `63.4 s`,
  `no_brain` wiped in `52.4 s`, `baseline` killed in `144.5 s` in the same wave).
- 2026-11-20: **Review 5 fixes (New 1, New 2, M4, L2/New L5, L4, L6, L13, New L1–L4):**
  `bench_bosses.py` uses word-bounded regexes (`\bcombat=(\d+)` / `\bnoncombat=(\d+)`) in
  `_check_strat_line` (fixing the `combat=0` substring bug), summarizes and balances mixed arms
  separately (`by_arm`, running until every arm reaches `per_arm_target`), invalidates trials with
  pre-pull combat (`_had_pre_pull_combat`), keys `_TraceTailer` offsets by inode `(st_dev, st_ino)`
  so rotated files are never read twice, and retries slow console replies (`_console_bot`) with
  per-wave/per-boss error isolation; `test_commands.cc` counts `passives=` in a separate read-only
  pass after aura removal, performs full `EffectResurrectPet`-style pet revive + hunter DB summon in
  `resetbot`, refreshes spells and pets in `gear`, and avoids converting an existing 5-man to a raid
  when re-grouping an existing member; `behavior_trace_scripts.cc` handles null-killer `OnUnitDeath` via `GetLootRecipientGroup()` /
  `GetLootRecipient()` within 100 yd; `TraceSample` adds `inst` (`instance_id`) and uses
  `Spell::m_targets.GetDstPos()` for ground-targeted spells, and `dodge_timeline.py` isolates cast
  windows per `inst` (plus `--bots`). Verified via `tools/wlb check` (400 C++ + 25 Python tests, 6/6
  in `dodge_timeline_test.py`) and live on Ikiss (`--mode mixed --runs 2 --parallel 2`: `llm` 1/1 kill
  `56.6 s` `0` deaths vs `llm_no_notes` 0/1 wipe `6` deaths; `dodge_timeline.py` cleanly separated
  `inst=1` and `inst=2`).
- 2026-11-20: **Docs cleanup:** deleted superseded/invalidated docs (`docs/reviews/2026-11-18-f40b0b6.md`,
  `docs/reports/hill-climb-2026-11-19.md`, `docs/benchmarks/2026-11-20-boss-*.md`,
  `docs/plans/{phase2,harness,latency,grammar-idl}.md`, `docs/specs/trigger-audit.md`); merged
  lasting decisions into `docs/design.md`, moved built specs to `docs/specs/{harness,grammar-idl,options}.md`,
  repointed all code/doc comments, and pruned `docs/TODO.md`.
