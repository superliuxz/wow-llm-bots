# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-17.

## In progress

Items #1–#3 are done (live verified 2026-11-17); #4 snapshot fill (`ext` + `indicators`) is done, remaining #4 option kinds and open panels are in `## Next, in order` ahead of #5.

- **#4 Module: more things a bot can do: partly done (2026-11-17).** Built, unit-tested, and
  verified live: `Gather <herb/ore>` and `Use <chest, quest object, door, portal>` options
  (`options.cc`, `option_executor.cc`, `UseOnArrival` in `llm_brain_world_script.cc`);
  `snapshot_builder.cc` + `nearby_builder.{h,cc}` now fill `ext.target`, `ext.combat`,
  `ext.party`, `ext.quest_tracker`, and `base.self.indicators` (`bag_free_slots`,
  `talent_points_unspent`, `new_mail`, `durability_warning`); `screen_renderer.cc` renders all
  of them. **Left:** `Talk`, `Rest`, `Explore`, `Go to` options (resolves the 13 `planned` eval
  cases), then open panels (`bags`, `character`, `quest_log` — `docs/specs/snapshot.md`).

## Next, in order

1. **Module: remaining options and open panels** (`#4`, `docs/plans/harness.md` M1,
   `docs/specs/snapshot.md`): add `Talk` (gossip without master), `Rest`, `Explore`, and `Go to`
   options so the 13 `planned` eval cases run on live options, then open panels (`bags`,
   `character`, `quest_log`).
2. **Standing orders** (`#5`, `docs/specs/triggers.md`, "The grammar (final, 2026-11-17)"). The `when`
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
   **Design decisions to settle with the user before building (2026-11-17):**
   1. **Where orders run.** In the module (reacts within a world tick, the spec's promise, but
      more logic in the server and orders must be sent down the socket) or in the harness (module
      stays thin, but reacts only on snapshots/events: 250 ms-1 s). The spec says "runs within a
      tick" without saying where. The big one.
   2. **Named places in a boss room** (133 of the 254 unsayable actions). Spec's order: places
      relative to a unit or player (works today), then spots computed from positions (behind the
      boss, max range, N yd apart), then per-room landmarks (deferred until a fight needs one).
      Open: build the computed spots now?
   3. **How each new verb runs** (`kite`, `mark`, `board`, `equip`, `pet`, `interact`): a
      playerbots action or our own movement, per verb; check playerbots verb by verb.
   4. Then the small snapshot additions (unit states, impact spots, liquid, main assist):
      mechanical once 1-3 are settled.

## Later

- **GPU `--backend-sampling` or thin `libllama` runner** (`docs/plans/latency.md`, Tier 3 #8):
  unconstrained + `ParsePlan` with 1 grammar retry (`+16%` capacity), or a `wlb_net` Unix-socket
  `libllama` daemon combining ~15-token ID masks on structured fields with GPU sampling on free text
  (`-200 to -330 ms` under load).
- **Isolated e2e stack** (`docs/plans/phase2.md`, E): own ports and socket, database restored from a
  golden copy, so tests never stop the play server. Today `wlb test e2e|perf` stops it for the
  run plus a test-flag switch each way (only our module's files and a relink since 2026-11-17;
  one test install per run, configs switch with a restart).
- **More e2e cases:** death and respawn, harness reconnect, a failing choice reaching the brain.
- **More perf:** a raid-wide burst (N bots woken in the same instant every M seconds; today's
  `SURPRISES` wakes bots independently), cells past capacity, and the harness-side age points
  (`age_at_ask`, `age_at_plan`) once H4 exists (`docs/plans/harness.md`, "Age").
- **Brain memory**, `var/bots/<name>/` (its own git repo): not created yet, on purpose.
- **`IsInRandomAccountList` audit**, the remaining sites (`docs/playerbots-random-bot-audit.md`).
- **`module/server/`** layout, when a second AzerothCore module exists.

## Loose ends

- Bots without Herbalism or Mining are still offered `Gather <herb/ore>` and walk there before
  failing with `you lack the skill, tool or key for ...` (seen live on Copper Vein, Kingsblood,
  Peacebloom); gate or annotate by `required_skill` / learned skills.
- When two brain-run bots whisper `"who"` back and forth (`Gatda` -> `Testpriest`), they echoed
  `"who"` several times before switching to a real question (and stopped naturally after
  `"sounds good, let me know when you're ready"`).
- A bot once wrote `tell <its own name>:` (Gatda, Gemma 4, live 2026-11-17); the module refuses
  it. Likely cause, the RULES.md example `tell Gatda:`, is fixed (review finding 8); watch whether
  it recurs.
- **Eval on live options (2026-11-17, Gemma 4):** 34/37 live cases (`herb_to_gather` now
  passes with `ext.quest_tracker`: 1/1), 7/7 whispers, 10/13 planned. With only Attack/Gather/Use/Keep
  on screen, it attacks a critter (`ignore_critter`), a red level-6 mob (`red_mob_nearby`), and a
  tapped mob (`skip_tapped_mob`): no way to walk away exists yet (Explore / Go to town are
  `planned`). The Qwen3.6 shootout was not re-run on the new set; the model choice was made on the
  old one.
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

- 2026-11-17: completed `#1`–`#3` and `#4` snapshot fill (`docs/phase2-results.md`,
  `docs/plans/latency.md`): `snapshot_builder.cc` + `nearby_builder.{h,cc}` fill `ext.target`,
  `ext.combat`, `ext.party`, `ext.quest_tracker`, and `base.self.indicators`; `herb_to_gather`
  updated and passing (`1/1`); `wlb test perf --update-baselines --verify` passed (`2/2`) on Gemma
  4 26B-A4B (`8.9/s` capacity, `50 bots / 30 s` p50 `0.4 s` / p95 `0.8 s`, snapshot build p50
  `83 µs`); live run with rebuilt server verified whisper replies without running `who` as a
  playerbots command, `ext` lines on screen, death -> `Keep` only -> `alive again`, urgent
  state-flip `age_at_ask_ms` p50 `0.18 ms`, and `do:` p50 `109.6 ms` (`86` decisions).

- 2026-11-17: follow-up review fixes on 4ce9490 (`bot_loop.cc`, `option_executor.cc`,
  `nearby_builder.cc`, `llm_brain_world_script.cc`): preserve `say`/`tell` and refusal backoff
  when an early-dispatched step fails during streaming; clear `last movement` before
  `UseObjectNow`; check `GO_FLAG_INTERACT_COND`/`ActivateToQuest` and `GO_READY` for
  `GameObject::usable`; clear stale/disconnected bot call state; send `OnSay` errors as
  non-step events (`command_id = 0`) without an alerting snapshot; reset `step_refusals` on
  normal `look timer`.

- 2026-11-17: the rest of review c80e791 (`docs/reviews/2026-11-17-c80e791.md`): the done
  signal (`Snapshot::done_command_id`), Gather/Use walk-then-use with the refusal reason,
  `bag_free_slots`, loot up/down slopes, Attack aimed at the attacker, only Keep while dead, chat
  checks (`CleanChatText`, whisper checks), stale-snapshot reset, eval fixtures with live
  options (`fixture_options`; 13 cases `planned`), varied whisperers, test-stack restore
  (state file, compile before stop, installed flavor, no test backups), zombie-safe `alive()`,
  `perf brain` fails on failed calls, Glaze read limits and wire tests.

- 2026-11-17: review fixes 1, 2, 7, 9, 10 (`docs/reviews/2026-11-17-c80e791.md`): whispers no
  longer run as playerbots commands; watch latches start from the last snapshot; friendly units
  aren't attackers; a cancelled/failed/superseded call's wake reasons and events are put back;
  repeated step refusals back off (2-30 s).

- 2026-11-17: default model switched to Gemma 4 26B-A4B (`tools/wlblib/llm.py`) by the rule
  "the more accurate within the speed limit": 48-case eval Gemma 4 44/48 (`do:` 134 ms, full
  306 ms) vs Qwen3.6 35/48 (118 / 218 ms); Qwen3.6 kept in `models/` (`MODEL=`).
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
