# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-17.

## In progress

Working order (2026-11-17 09:50): finish #3, #1, then #4, #2, #5.

- **#1 Play Testpriest live: partly done (2026-11-17 01:34-01:38 and from 10:00, Qwen3.6-35B, 16
  slots).** Measured at 01:34: snapshot build p50 33-45 us, p99 58-122 us; built->received p50
  0.09 ms, p95 0.15 ms, max 1.5 ms; world update diff 1 ms; warm `do:` 60-92 ms, full reply
  124-185 ms (first look 231/344 ms, cold prefix); `age_at_apply` p50 91-137 ms, max 295 ms.
  Checked by hand: harness killed -> module logs "harness disconnected", the body keeps playing; a
  client that stops reading for 15 s gets 15 frames, then live ones (0 dropped). **Not yet
  checked:** whisper -> in-character reply (phase 2 "done when" item 1: needs the human player in
  game), death and respawn, combat detection latency as a number, wakes per bot per minute over a
  longer run.
  - Fixed (2026-11-17): the wake loop on a refused Attack (`step failed: can't attack X now`, six
    wakes in ~1 s). `option_executor.cc` now counts a refusal as done when the body already has
    that target (`current target` or `GetVictim()`).
  - Open: loot steps timed out twice (corpse at 0-9 yd, still `lootable yes` after 15 s). The
    cause isn't known yet.
- **#3 E2E latency Tier 2: built (2026-11-17), live numbers pending** (`docs/plans/latency.md`,
  Tier 2). Measure combat-detection age and preemption savings in the #1 run.

- **#4 Module: more things a bot can do: partly done (2026-11-17).** Built, unit-tested, and the
  module sources compiled (not yet run in game): `Gather <herb/ore>` and `Use <chest, quest object,
  door, portal>` options (`options.cc`); the executor (`option_executor.cc`: lootable objects go
  through playerbots' loot strategy, others are walked to with `MovePoint` and clicked on arrival
  by `UseOnArrival` in `llm_brain_world_script.cc`); the screen now writes `ext.target` / `focus`,
  `ext.combat`, `ext.party`, the quest tracker with goals, and low bag space / damaged armor /
  talent points / mail (`screen_renderer.cc`; `bag_free_slots` is now optional, unset = unknown).
  **Left:** the module does not fill `ext` yet (target, combat, party, quest tracker, indicators:
  `snapshot_builder.cc`), so the new screen lines only show in tests; then talk and "go to"; then
  bags, gear, quest log. Needs `tools/wlb build server` + a restart to try live (stops the play
  server).
- **#2, #5:** not started.

## Next, in order

1. **Play Testpriest live with the real model**, end to end (`tools/wlb llm` + `tools/wlb harness`),
   and read the real numbers: wakes per bot per minute, how long plans last, `age_at_ask` /
   `age_at_first_line` / `age_at_plan` / `age_at_apply`, combat and death detection latency,
   latest-wins under a stalled harness. Done when the list in `docs/plans/phase2.md` holds.
2. **Model choice:** decide whether to switch `tools/wlblib/llm.py` default from Qwen3.6-35B-A3B to
   Gemma 4 26B-A4B (`eval` 13/15 vs 10/15; after Tier 1 Qwen3.6 hits `11.3/s` capacity and `79 ms`
   solo `do:` vs Gemma 4's `9.1/s` and `91 ms`; Gemma 4 is 4.8 GiB smaller; both kept in `models/`
   for now), and revisit `herb_to_gather` once quest log is rendered.
3. **E2E latency Tier 2** (`docs/plans/latency.md`): built; measure live (In progress).
4. **Module: more things a bot can do** (`docs/plans/harness.md`, M1 "Not yet"). Options today:
   Attack, Loot, Keep. Missing: talk (playerbots opens gossip only for a bot's master), gather,
   use, "go to" (options exist only for named units within 20 yd). Then the rest of the snapshot:
   quest log (plans invent quests without it), bags, gear, target (`ext.target`), combat, party
   (`docs/specs/snapshot.md`; most of `common/proto/snapshot.h` is not filled yet).
5. **Standing orders** (`docs/specs/triggers.md`, "The grammar (final, 2026-11-17)"). The `when`
   and `<act>` grammar is final and both audit tables parse against it
   (`tools/wlb check triggers`; `docs/specs/trigger-conditions.tsv` and `trigger-actions.tsv`).
   Left: (a) re-judge the unsayable and stretched rows with the final grammar for a measured
   coverage number (judged against the earlier grammar: conditions 85%, actions 46%; upper bounds
   with the new constructs about 89% and 55%); (b) build it in `harness/src/plan.cc` and retire the
   Python copy of the grammar (`tools/check-trigger-grammar.py`), then the executors in the module
   (the new verbs `kite`, `mark`, `board`, `equip`, `pet`, `interact` need engine support);
   (c) the snapshot gaps: named places in a room (**133 of the 254 unsayable actions are "go to
   hardcoded coordinates"**: the biggest one), a flying / untargetable / submerged state on `Unit`,
   assist-tank index, where a cast will land (`lands_in_s`), liquid under the bot; (d) evals.
   `trigger-actions.tsv` and its brief are not committed yet.

## Later

- **GPU `--backend-sampling` or thin `libllama` runner** (`docs/plans/latency.md`, Tier 3 #8):
  unconstrained + `ParsePlan` with 1 grammar retry (`+16%` capacity), or a `wlb_net` Unix-socket
  `libllama` daemon combining ~15-token ID masks on structured fields with GPU sampling on free text
  (`-200 to -330 ms` under load).
- **Isolated e2e stack** (`docs/plans/phase2.md`, E): own ports and socket, database restored from a
  golden copy, so tests never stop the play server. Today `wlb test e2e|perf` stops it for ~15
  minutes (the module flavor switch recompiles the whole `modules` target both ways).
- **More e2e cases:** death and respawn, harness reconnect, a failing choice reaching the brain.
- **More perf:** a raid-wide burst (N bots woken in the same instant every M seconds; today's
  `SURPRISES` wakes bots independently), cells past capacity, and the harness-side age points
  (`age_at_ask`, `age_at_plan`) once H4 exists (`docs/plans/harness.md`, "Age").
- **Brain memory**, `var/bots/<name>/` (its own git repo): not created yet, on purpose.
- **`IsInRandomAccountList` audit**, the remaining sites (`docs/playerbots-random-bot-audit.md`).
- **`module/server/`** layout, when a second AzerothCore module exists.

## Loose ends

- `Use` on a door, quest object or portal (`option_executor.cc`): the harness counts a Use step done
  only when the object stops being usable (`plan_runner.cc` `IsTargetDone`), which a clicked door
  never does, so such steps end on the 15 s timeout ("step timed out"). Needs a done signal from
  the module (a wire change: decide its shape first). Also unverified live: whether playerbots'
  own movement overrides the `MovePoint` walk to the object.
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
- The human player's account name is in the public git history (older commits of
  `tools/server-start.sh`); it is out of the current files. Removing it means rewriting history and
  a force-push: their decision.
- Unit tests for the AzerothCore-dependent module code (`bot_manager`, `option_executor`,
  `nearby_builder`, ...) can't run without the game server: only the e2e case covers it
  (rebuild/restart `out/server` with `tools/wlb build server` before connecting the new BEVE
  `harness`, since the running play server predates the Glaze wire switch).
- `BrainBridge`: a connection generation drops stale frames, but a stalled old connection's
  commands still arrive in order before its disconnect is seen. Judged not worth more yet.
- Bugs found in playerbots (not ours, `third_party/`): `RangeTriggers.cpp:275` returns before its
  distance loop (Yogg-Saron Brain Link); `NotBehindIngvarTrigger` fires when the bot *is* behind its
  target. Five `*Cheat*` boss triggers fire only with playerbots' raid cheat on.
- A flaky risk in `e2e choose`: if the boar drops nothing there is no corpse to loot, and the case
  fails with "never offered". Passed every run so far.

## Done (newest first, keep about ten)

- 2026-11-17: replaced `protobuf` + `abseil-cpp` with `stephenberry/glaze` v9.1.0
  (`common/proto/snapshot.h` plain C++20 structs + `snapshot_wire.cc` BEVE wire/JSON +
  `snapshot_check` + `wire_py.py`), removed `third_party/abseil-cpp`, `third_party/protobuf`,
  `protoc`, and `.pb` fixtures, and added GitHub Actions CI (`.github/workflows/ci.yml`) running
  `wlb check --sanitizers`.
- 2026-11-16: Chromium `common/base/strings/` (`strcat`, `string_util`, `string_split`,
  `string_number_conversions`, `stringprintf`), migrated `harness/` off `absl::*`, replaced `<pthread.h>`
  in `harness/src/main.cc` and all `<set>`/`<map>` across `harness/` + `server_module/` (`std::flat_map`,
  `std::flat_set`, `std::vector`), enforced the bans in `CONTRIBUTING.md` + `wlb check includes`, and
  split `*SAN` out of fast `wlb check` into `wlb check sanitizers` / `--sanitizers`.
- 2026-11-16: H4 live harness loop (`screen_renderer` aligned with `snapshot.options`,
  `module_connection`, `triggers`, `plan_runner`, `bot_loop`, `harness run` + `tools/wlb harness`;
  113 harness unit tests passing across default, ASan+UBSan, TSan, MSan).
- 2026-11-16: E2E latency Tier 1 (`option_executor.cc` `ResetActionDuration()`, `do:`-first + SSE
  streaming early dispatch + `TCP_NODELAY`, optional `goal?`, shared-`system` checkpoint split +
  soft slot affinity): solo `do:` `206 → 79 ms` (Qwen3.6) / `239 → 91 ms` (Gemma 4); Qwen3.6
  capacity `7.8 → 11.3/s` (+45%) and 100-bot 15 s `2.1/3.2 s → 0.8/1.5 s` (`docs/plans/latency.md`,
  `docs/phase2-results.md`).
- 2026-11-16: plan quality fixes: action verbs on numbered screen lines, `look >= 10` s floor, and
  `RULES.md` facts + example (`Gemma 4 26B` 9/15 → 13/15 at 239 ms, `Qwen3.6-35B` 6/15 → 11/15 at
  206 ms; `docs/phase2-results.md`).
- 2026-11-16: `tools/wlb test eval` with 15 live-capture cases (`harness/testdata/eval/`); model
  shootout + `perf brain` on Gemma 4 26B-A4B (9/15, 8.6/s) vs Qwen3.6-35B-A3B (6/15, 7.8/s);
  `gpt-oss-20b` dropped (`docs/phase2-results.md`).
- 2026-11-16: `tools/wlb test perf` with baselines (snapshots, brain); `wlb test e2e` with the first
  case (`choose`); `wlb test unit`.
- 2026-11-16: `tools/wlb`, one Python CLI, replaces the 21 shell scripts.
- 2026-11-16: `server_module/src` grouped (`config/`, `bridge/`, `snapshot/`, `options/`,
  `bot_manager/`).
- 2026-11-16: options in every snapshot and `Choose` carried out by the bot's body (M1); live: a bot
  attacked, killed and looted a boar.
- 2026-11-16: standing orders designed; all 595 playerbots boss triggers checked against the grammar.
- 2026-11-16: snapshot delivery fixes (staggered, latest-wins, measured age); harness H1-H3.
