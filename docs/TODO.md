# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-17.

## In progress

(none)

## Next, in order

1. **Play Testpriest live with the real model**, end to end (`tools/wlb llm` + `tools/wlb harness`),
   and read the real numbers: wakes per bot per minute, how long plans last, `age_at_ask` /
   `age_at_first_line` / `age_at_plan` / `age_at_apply`, combat and death detection latency,
   latest-wins under a stalled harness. Done when the list in `docs/plans/phase2.md` holds.
2. **Model choice:** decide whether to switch `tools/wlblib/llm.py` default from Qwen3.6-35B-A3B to
   Gemma 4 26B-A4B (`eval` 13/15 vs 10/15; after Tier 1 Qwen3.6 hits `11.3/s` capacity and `79 ms`
   solo `do:` vs Gemma 4's `9.1/s` and `91 ms`; Gemma 4 is 4.8 GiB smaller; both kept in `models/`
   for now), and revisit `herb_to_gather` once quest log is rendered.
3. **E2E latency Tier 2** (`docs/plans/latency.md`): (e) expand `SnapshotSchedule::Flags` (loss of
   control, attacker count, option target completion) + attach fresh snapshot to alerting
   `ImmediateEvent`s (`-125 to -1,000 ms`); (f) preempt obsolete lower-priority in-flight calls on
   urgent combat wakes + P0-P4 priority tiers in `scheduler.cc` / `llm_client.cc` (`-0.8 to
   -1.8 s`).
4. **Module: more things a bot can do** (`docs/plans/harness.md`, M1 "Not yet"). Options today:
   Attack, Loot, Keep. Missing: talk (playerbots opens gossip only for a bot's master), gather,
   use, "go to" (options exist only for named units within 20 yd). Then the rest of the snapshot:
   quest log (plans invent quests without it), bags, gear, target (`ext.target`), combat, party
   (`docs/specs/snapshot.md`; most of `common/proto/snapshot.h` is not filled yet).
5. **Standing orders** (`docs/specs/triggers.md`, "Standing orders", proposed). Settle the order
   syntax, build it in `harness/src/plan.cc`, the executors in the module, then evals. Cheap
   grammar gaps to add first: `me` as a thing (my health), master and assist tank as `who`,
   line of sight, "behind it", my current target, game objects, aura time left, "anyone within N",
   absolute mana. Snapshot gaps: a flying / untargetable / submerged state on `Unit`, assist-tank
   index, where a cast will land (`lands_in_s`), liquid under the bot, named places in a room
   (the hard one). Phase numbers come from DBM (decided). Re-run the trigger table with the
   revised grammar (`docs/specs/trigger-conditions.tsv`, `tools/wlb check triggers`) for a real
   coverage number (85% estimated, 69% without stretches).

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
