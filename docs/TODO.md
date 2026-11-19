# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-18.

## In progress

Items #1–#5 are done and verified live (2026-11-18).

## Next, in order

- **Named room landmarks** (`docs/specs/triggers.md`): 132 of the 218 unsayable boss actions move
  to hardcoded room coordinates; deferred until a boss fight needs per-room landmark data.

## Later

- **Bind hearthstone** at an innkeeper as an option of its own (the brain's choice, through the
  bind spell), not part of Talk (`docs/plans/options-and-panels.md`).
- **More e2e cases:** death and respawn, harness reconnect, a failing choice reaching the brain.
- **More perf:** a raid-wide burst (N bots woken in the same instant every M seconds; today's
  `SURPRISES` wakes bots independently), cells past capacity, and the harness-side age points
  (`age_at_ask`, `age_at_plan`) once H4 exists (`docs/plans/harness.md`, "Age").
- **Brain memory**, `var/bots/<name>/` (its own git repo): not created yet, on purpose.
- **`IsInRandomAccountList` audit**, the remaining sites (`docs/playerbots-random-bot-audit.md`).
- **`module/server/`** layout, when a second AzerothCore module exists.

## Loose ends

- When two brain-run bots whisper `"who"` back and forth (`Gatda` -> `Testpriest`), they echoed
  `"who"` several times before switching to a real question (and stopped naturally after
  `"sounds good, let me know when you're ready"`).
- A bot once wrote `tell <its own name>:` (Gatda, Gemma 4, live 2026-11-17); the module refuses
  it. Likely cause, the RULES.md example `tell Gatda:`, is fixed (review finding 8); watch whether
  it recurs.
- **Eval (2026-11-18, Gemma 4):** 42/48 scored cases (88%, random 22.0; 4/4 standing-order cases
  pass, `do:` p50 `72 ms`, total `208 ms`; 7 unfailable cases not scored). Misses:
  `healthy_boar_pull`, `skip_tapped_mob`, `red_mob_nearby`, `hp_60_fine`, `bags_full` (with closed
  bags it explores; in live play the habit opens full bags, and `bags_full_open` passes),
  `new_loot_hover`. The Qwen3.6 shootout was not re-run.
- **Panels, things not built yet** (`docs/plans/options-and-panels.md`): no `Use <item>` option
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

## Done (newest first, keep about ten)

- 2026-11-18: screen goldens populated with real module options (`harness/testdata/screens/*.facts.json`
  captured on the isolated test server, `add_fixture_options.py` checking both `eval` and
  `screens`, `tools/bench/prompt_render.py` taught module option numbering and self auras);
  trigger audit tables (`trigger-conditions.tsv`, `trigger-actions.tsv`, `triggers.md`) re-judged
  against the final grammar (`Points 0..99999` for `mana_points`): **483/547 (88%, 77% clean)**
  conditions and **251/469 (54%, 43% clean)** actions fit (`OrdersTest.*Audit*` passing).
- 2026-11-18: live standing-orders e2e suite (`tools/wlblib/cases/e2e_orders.py`,
  `config/tests/orders.llm-brain.overrides`, `server_module/testdata/characters/priest-l80-boars.pdump`,
  test-only `llmtest` console commands in `server_module/src/test/test_commands.{h,cc}`): runs real
  `harness run` + `act_executor.cc` on the isolated test server across moving + `then` chain +
  `group` with nearby mobs + edge-latching (`away group 10, then to behind Telda`), `target` +
  `mark` (`SKULL`), `use` (`Power Word: Shield`), and >5 s `kite` + `stop moving`; also filled
  `Unit::raid_marker` (`RaidMarkerOf` in `world_util.{h,cc}`, `nearby_builder.cc`,
  `snapshot_builder.cc`), which the `mark` e2e caught missing. Standing orders (#5) complete.
- 2026-11-18: turned standing orders on in live `BotLoop` calls and `RULES.md` (`CollectOrderNames`
  from `notes.md` and the screen passed to both `PlanGrammar` and `ParsePlan`; `order:` reply line
  generated from `plan.wlb` and verb section added to `content/RULES.md`; `harness/src/` grouped
  into `screen/`, `plan/`, `llm/`, `runtime/`, `test/`). Eval on Gemma 4: **42/48** scored cases
  (88%, random 22.0; **4/4** standing-order cases pass, 0 spurious orders on leveling cases,
  first line p50 73 ms).
- 2026-11-18: review of the standing-order commits (d912e3d..736b1f9): `group` crash, orders
  latch per crossing, wakes covered by condition kind and subject (`WakeTrigger::kinds`,
  `OrderCoversWake`), separate `done_act_command_id` (`kWireVersion` 2), act owns movement over a
  plan walk, timeouts per act with `stop moving`, `kite` ends when its condition clears, bare
  `away` anchored, `anyone` friendly only, `use` through the client's handlers and failing when
  the cast doesn't start, bounded history. Not yet run live: no e2e case runs an order yet.
- 2026-11-18: review of f40b0b6, all 18 findings (`docs/reviews/2026-11-18-f40b0b6.md`, "Status"),
  plus the parts of review c80e791 left open (canned replies, say/yell checks, perf's parser copy);
  all 7 e2e cases pass on the test server; eval 39/44 scored cases (a random option scores 21;
  7 unfailable cases left out), failing: ignore_critter, red_mob_nearby, hp_60_fine, bags_full,
  new_loot_hover.
- 2026-11-18: isolated test stack (`docs/plans/phase2.md`, E): e2e and perf run on their own
  test server (`out/server-test`, port 8086, `acore_test_*` databases reset from golden copies)
  while the play server keeps running; perf beside a busy play server warns instead of failing.
- 2026-11-18: grammar IDL steps 3 and 4, the plan finished (`docs/plans/grammar-idl.md`,
  "Progress"): RULES.md's reply form generated and checked (`tools/wlb check rules`); the parser
  prefers the names the call offered (fixes `use <name> on`); `OrderGrammar` without `oline`.
- 2026-11-18: grammar IDL step 2: `plan.wlb` generates the plan code (`docs/plans/grammar-idl.md`,
  "Progress"). Parity: eval 47/53 (same as before), `perf brain` PASS with every call valid
  (capacity 8.5/s vs baseline 8.9; replay p95 1.3 s vs 0.8 s, on the llama-server already noted as slow).
- 2026-11-18: review of 3a28d9d / fa50c95: `LootCorpseNow` says "Inventory is full" only when
  there is no room ("You can't carry <item>" for the server's other refusals; items being rolled
  for or held for the master looter stay and the step is done); RULES.md without the eval-shaped
  wording; the loot trade-off documented (`docs/plans/options-and-panels.md`, "Loot").

- 2026-11-18: live `harness` run (`Testpriest`, `Gatda`) verified `open:`, `close:`, `hover:`
  (`hover: Kolkar Leaders` -> quest tooltip wake), remembered panels, and the habits in live play;
  fixed two live findings + time-based hover (`UiState::kHoverDuration = 5 s` instead of 5
  snapshots; `LootCorpseNow` through the server's loot opcodes so >80%-full bags no longer leave
  grey items on corpses and loop `Loot`, returning `"Inventory is full"` when items cannot fit;
  `FormatQuestText` for `$B`/`$N`/`$C`/`$R`/`$G` tokens in quest log and tooltips); `6/6` e2e
  suites passing.

- 2026-11-18: looking eval cases (`food_in_open_bags`, `bags_full_open`, `new_loot_hover`,
  `forbid_look` on `healthy_boar_pull`, `eval_plans.py` `must_open`/`must_hover`/`forbid_look`);
  aligned `PlanGrammar` + `FormatPlan` order (`goal? say? tell? open? close? hover?`) with
  `RULES.md` so `goal:` no longer blocks `hover:`, and added panel/tooltip visibility facts +
  example in `RULES.md`: **49/53 (92%)** on Gemma 4 (`new_loot_hover`, `quest_giver_and_boar`, and
  `red_mob_nearby` passing), `perf brain` **PASS** (`10.2/s` capacity, `1040/1040` valid).

- 2026-11-18: open panels (bags, character, quest log) and hover for items and quests
  (`docs/plans/options-and-panels.md`): module fill and tooltips (item "Use:" text from Spell.dbc), `open:` /
  `close:` / `hover:` reply lines with a wake once shown, remembered panels, three habits; RULES.md
  reply form; `e2e panels`; unit tests (`ui_state`, `spell_text`, plan, screen, bot loop).

- 2026-11-17: `e2e talk` and `e2e rest` (`config/tests/talk`); Rest fixes they found: the body
  stood a sitting bot up at once (playerbots' next AI check: `SetNextCheckDelay` while sitting
  without food, no `ResetActionDuration` after eat / drink), and a second item used in the same
  tick was refused, so a bot ate and never drank (`ContinueRest`, once a second). `perf
  snapshots` and `perf walk` re-baselined with OptionFacts (build p50 94 us, walk path p50
  ~250 us).

- 2026-11-17: review of 528f479 (the options), all 11 points: eval facts written by the module
  (`test/facts_dump.cc`, `LlmBrain.TestFactsQueries`, `add_fixture_options.py --capture`);
  `WorldPlaces` indexed at startup (76 ms), a bot's spots/places recomputed after 20 yd or 10 s,
  quest map points cached; training through `Trainer::TeachSpell`; no hearthstone binding in
  Talk; repair no-ops removed; place ids from position; the stall rule counts movement and paths
  are asked for at most once a second; interrupted walks report an error; RULES.md states facts;
  `e2e walk`; `position_facts` unit tests.

- 2026-11-17: completed `#1`–`#3` and `#4` snapshot fill (`docs/results.md`,
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

- 2026-11-17: the rest of review c80e791 (fixed in 4ce9490): the done
  signal (`Snapshot::done_command_id`), Gather/Use walk-then-use with the refusal reason,
  `bag_free_slots`, loot up/down slopes, Attack aimed at the attacker, only Keep while dead, chat
  checks (`CleanChatText`, whisper checks), stale-snapshot reset, eval fixtures with live
  options (`fixture_options`; 13 cases `planned`), varied whisperers, test-stack restore
  (state file, compile before stop, installed flavor, no test backups), zombie-safe `alive()`,
  `perf brain` fails on failed calls, Glaze read limits and wire tests.

- 2026-11-17: review fixes 1, 2, 7, 9, 10 (fixed in 4ce9490): whispers no
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
  `docs/results.md`).
- 2026-11-16: plan quality fixes: action verbs on numbered screen lines, `look >= 10` s floor, and
  `RULES.md` facts + example (`Gemma 4 26B` 9/15 → 13/15 at 239 ms, `Qwen3.6-35B` 6/15 → 11/15 at
  206 ms; `docs/results.md`).
- 2026-11-16: `tools/wlb test eval` with 15 live-capture cases (`harness/testdata/eval/`); model
  shootout + `perf brain` on Gemma 4 26B-A4B (9/15, 8.6/s) vs Qwen3.6-35B-A3B (6/15, 7.8/s);
  `gpt-oss-20b` dropped (`docs/results.md`).
- 2026-11-16: `tools/wlb test perf` with baselines (snapshots, brain); `wlb test e2e` with the first
  case (`choose`); `wlb test unit`.
- 2026-11-16: `tools/wlb`, one Python CLI, replaces the 21 shell scripts.
- 2026-11-16: `server_module/src` grouped (`config/`, `bridge/`, `snapshot/`, `options/`,
  `bot_manager/`).
- 2026-11-16: options in every snapshot and `Choose` carried out by the bot's body (M1); live: a bot
  attacked, killed and looted a boar.
- 2026-11-16: standing orders designed; all 595 playerbots boss triggers checked against the grammar.
- 2026-11-16: snapshot delivery fixes (staggered, latest-wins, measured age); harness H1-H3.
