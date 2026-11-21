# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-20.

## In progress

- Next benchmark run: run `--mode mixed` + `--mode baseline` + `--mode no_brain` with `--runs 6`
  on the fixed runner (`bench_bosses.py`) for statistically powered comparison data, and save one
  clean report under `docs/benchmarks/`.

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

## Done (newest first, keep about ten)

- 2026-11-20: **Docs cleanup:** deleted superseded/invalidated docs (`docs/reviews/2026-11-18-f40b0b6.md`,
  `docs/reports/hill-climb-2026-11-19.md`, `docs/benchmarks/2026-11-20-boss-*.md`,
  `docs/plans/{phase2,harness,latency,grammar-idl}.md`, `docs/specs/trigger-audit.md`); merged
  lasting decisions into `docs/design.md`, moved built specs to `docs/specs/{harness,grammar-idl,options}.md`,
  repointed all code/doc comments, and pruned `docs/TODO.md`.
- 2026-11-20: **Review 4 Lows 11 & 14 (DBC spell radii, cast-origin dodge distance, impact-dodge meter, incremental trace tailing):**
  `TraceSample` (`behavior_trace.{h,cc}`, `llm_brain_world_script.cc`) emits `target_x`, `target_y`,
  and DBC `target_cast_radius` via `SpellInfo::Effects[eff].CalcRadius()`; `dodge_timeline.py` drops
  the hardcoded radius table, measures Euclidean distance from cast origin `(target_x, target_y)` for
  all bots, and filters `act_delay_ms` to dodge/movement verbs; `boss_meter.py` reports
  `impact_dist_yd` & `outside_at_impact`; `bench_bosses.py` uses incremental byte-offset
  `_TraceTailer` (partial-line safe). Unit-tested (`dodge_timeline_test.py` in `tools/wlb check`)
  and live-verified on Ikiss (DBC `radius=35.0yd`).
- 2026-11-20: **Review 4 fixes (High 1, Mediums 2–7, Lows 8–10, 12–13) + auto-loaded rules:**
  `llmtest resetbot` preserves passive auras (`passives=62` live-verified) and resets pet
  health/power, rage/energy/runic power, ammo, and reagents; `llmtest gear` refreshes glyphs, ammo,
  and reagents; `llmtest checkstrat` uses non-allocating `GetSupportedStrategies()`;
  `bench_bosses.py` reads `first_line_ms` from `bots_dir/*/events/*.jsonl` (fails if empty), scopes
  console checks to bot names, verifies `map=0` pre-reset and non-reused dungeon instance IDs across
  waves, asserts baseline scripts are ON, adds `--mode mixed` (rotates `llm`/`llm_no_notes` across
  parallel parties in one wave), and overwrites output JSON cleanly unless `--append`;
  `behavior_trace_scripts.cc` bounds group `mob_kill` to 100 yd; `trace_parity.py` drops
  `item_unequip` & `quest_abandon` (scorer re-locked); standardized the 3-step review-fix procedure
  in `CONTRIBUTING.md` + `.agents/rules/contributing.md` (`always_on`).
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
- 2026-11-18: **Grammar IDL (`docs/specs/grammar-idl.md`):**
  `tools/grammar/wlb_gen.py` generates C++ types, parser, printer, GBNF, and `content/RULES.md`
  reply form from `harness/grammar/{orders,plan}.wlb`, verified against real `llama-grammar.cpp`.
- 2026-11-18: **Isolated test stack (`docs/design.md`):**
  separate `out/server-test` build (port 8086, `acore_test_*` reset from `acore_golden_*`) so e2e,
  perf, and boss benchmarks run without touching the play server (`out/server`).
