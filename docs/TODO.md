# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-20.

## In progress

- Next benchmark run (all Review 8 items verified; all 28 scorer locks re-locked & verified with user
  OK): run `bench_bosses.py` with `--runs 8` (or `10` **per arm**) for `kaelthas`, `ick`, `murmur`,
  and `ikiss` (e.g. `--mode mixed_all --only kaelthas,ick,murmur,ikiss --runs 8 --parallel 3`), then
  save one clean report under `docs/benchmarks/`. Note: on `kaelthas`, `stage_xyz=(7.09, -0.45, -2.5)`
  eliminates arrival deaths and pre-pull combat, while `spawn_xyz=(148.5, 180.0, -16.2)` still pulls
  the nearby Sunblade pack during the fight (all arms run identically).

## Next, in order

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
- **Not individually live-triggered after final edits yet:** environmental/self death (`killer == killed`
  logged as non-PvP `died` with `detail="environment"` in `behavior_trace_scripts.cc`),
  `llmtest questcomplete` when a quest is already active in a bot's quest log (`CompleteQuest` +
  `RewardQuest` branch in `test_commands.cc`, source-verified), and the updated
  `areatrigger_teleport` entrance / pull coordinates for `ick`, `murmur`, and `ikiss` (verified in
  `acore_world`; live Kael'thas verified entrance staging after the final edit, while Ikiss and the
  `questcomplete`/`tele ... 0`/`clearstrat` console checks passed in the prior live check before
  adding entrance staging/dismount).

## Done (newest first, keep about ten)

- 2026-11-20: **Review 8 fixes (New Lows 1–3) + user-approved scorer relock:**
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
  when re-grouping an existing member; `behavior_trace_scripts.cc` handles null-killer `OnUnitDeath`
  via `GetLootRecipientGroup()` / `GetLootRecipient()` within 100 yd; `TraceSample` adds `inst`
  (`instance_id`) and uses `Spell::m_targets.GetDstPos()` for ground-targeted spells, and
  `dodge_timeline.py` isolates cast windows per `inst` (plus `--bots`). Verified via `tools/wlb check`
  (400 C++ + 25 Python tests, 6/6 in `dodge_timeline_test.py`) and live on Ikiss (`--mode mixed
  --runs 2 --parallel 2`: `llm` 1/1 kill `56.6 s` `0` deaths vs `llm_no_notes` 0/1 wipe `6` deaths;
  `dodge_timeline.py` cleanly separated `inst=1` and `inst=2`).
- 2026-11-20: **Docs cleanup:** deleted superseded/invalidated docs (`docs/reviews/2026-11-18-f40b0b6.md`,
  `docs/reports/hill-climb-2026-11-19.md`, `docs/benchmarks/2026-11-20-boss-*.md`,
  `docs/plans/{phase2,harness,latency,grammar-idl}.md`, `docs/specs/trigger-audit.md`); merged
  lasting decisions into `docs/design.md`, moved built specs to `docs/specs/{harness,grammar-idl,options}.md`,
  repointed all code/doc comments, and pruned `docs/TODO.md`.
- 2026-11-20: **Review 4 partial follow-up (DBC spell radii, cast-origin dodge distance, impact-dodge meter, incremental trace tailing):**
  `TraceSample` (`behavior_trace.{h,cc}`, `llm_brain_world_script.cc`) emits `target_x`, `target_y`,
  and DBC `target_cast_radius` via `SpellInfo::Effects[eff].CalcRadius()`; `dodge_timeline.py` drops
  the hardcoded radius table, measures Euclidean distance from caster `(target_x, target_y)` for
  all bots, and filters `act_delay_ms` to dodge/movement verbs; `boss_meter.py` reports
  `impact_dist_yd` & `outside_at_impact`; `bench_bosses.py` uses incremental byte-offset
  `_TraceTailer`. *(Remaining gaps from Review 5 tracked above: ground-targeted spell origin,
  cross-party scoring in shared trace dir, rotated files read twice, L10–L16.)*
- 2026-11-20: **Review 4 fixes (Fixed: H1, M2, M5, L1, L7, L8, L9; Partly fixed: M3, M4, M6, L2, L3, L4, L6, L10, L12) + auto-loaded rules:**
  `llmtest resetbot` preserves passive auras (`passives=` count bug tracked above) and resets
  rage/energy/runic power, ammo, and reagents (pet revive partial, tracked above); `llmtest checkstrat`
  uses non-allocating `GetSupportedStrategies()`; `bench_bosses.py` reads `first_line_ms` from
  `bots_dir/*/events/*.jsonl` (fails if empty), scopes console checks to bot names (strip check
  substring bug New 1 tracked above), verifies `map=0` pre-reset and non-reused dungeon instance IDs
  across waves, adds `--mode mixed` for `llm`/`llm_no_notes` (per-arm summary & balancing New 2
  tracked above), and overwrites output JSON unless `--append`; `behavior_trace_scripts.cc` bounds
  group `mob_kill` to 100 yd (no-killer kill L4 still open); `trace_parity.py` drops `item_unequip`
  & `quest_abandon` (scorer re-locked); standardized the 3-step review-fix procedure in
  `CONTRIBUTING.md` + `.agents/rules/contributing.md` (`always_on`).
- 2026-11-20: **Review 3 fixes on `trace-all-events`:**
  `server.console()` returns captured output (`tools/wlblib/server.py`); `bench_bosses.py` stages
  bots inside the dungeon before starting `harness`, enforces role and post-arrival `checkstrat`
  checks, splits `no_brain` / `llm_no_notes` / `llm` / `baseline` modes, and invalidates zero-pull
  trials; `llm_brain_world_script.cc` logs `act_sent` events; `behavior_trace_scripts.cc` credits
  group `mob_kill` via `OnUnitDeath` and drops unvalidated `vendor_buy`; `behavior_trace_unittest.cc`
  covers multi-file rotation and direct `TraceSample` formatting.
- 2026-11-19: **General / Say / Party chat + quest grouping (invite/accept/decline/leave):**
  wired `/s Say`, `/y Yell`, `/p Party`, `/ra Raid`, and `/1 General` / Trade / LFG hooks in
  `server_module/src/chat/chat_scripts.cc` and `speak.cc`; added `INVITE`, `ACCEPT_INVITE`,
  `DECLINE_INVITE`, `LEAVE_GROUP` options in `snapshot/snapshot.h`, `options.{h,cc}`,
  `option_executor.cc`, `llm_brain_world_script.cc`, and harness `screen_renderer.cc` / `bot_loop.cc`.
- 2026-11-19: **Layered human-likeness scorer (Steps 1–6, `docs/plans/self-improving-loop.md`):**
  5 Hz observer-parity recording + explicit gameplay events (`behavior_trace_scripts.{h,cc}`,
  `trace_parity.py`), pairwise Bradley–Terry ranker with bootstrapped 95% CIs + `blind_rate.py`,
  Layer D calibration (`known_bad_bots.py`, `scorer_calibration.py`), Layer C per-situation
  diagnostic classifier + MMO adversary suite (`adversary_suite.py`), Step 5 population metrics
  (`population_metrics.py`), and `soak_scorer.py` / `hill_climb.py` switched to Layer A gates +
  Layer B ranking (retired 0–100 composite score). Live-verified on 10-bot soak (11/11 Layer A
  gates PASS, Layer D calibration PASS: `heldout_human 1000.0 > current_bots 175.1 > all 4 known-bad`).
- 2026-11-19: **Unified human + bot behavior tracing & live soak test:**
  `behavior_trace.{h,cc}` (`LlmBrain.TraceDir` + `LlmBrain.ObserveHumans`) and `tools/wlb test soak`
  (`config/tests/soak.llm-brain.overrides`, `tools/wlblib/cases/soak.py`); fixed empty-vendor `Talk`
  loop (`TalkToNpcNow` returns refusal so `StepRefusalBackoff` engages).
- 2026-11-18: **`e2e boss` & `e2e orders` live plumbing tests:**
  repeatable Loken boss test (`tools/wlblib/cases/e2e_boss.py`, `-wotlk-hol`, mock brain) verifying
  `Lightning Nova` visibility over background `Pulsing Shockwave`, `1 ms` `AWAY Loken 25` dispatch,
  and mid-fight replan preservation of in-flight movement orders; all 6 `e2e orders` scenarios pass.
