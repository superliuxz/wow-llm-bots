# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-22.

## In progress

- **Phase 1 & Phase 2 remaining benchmark sign-off criteria** (see single-codebase `n = 8` 5-boss report [`docs/benchmarks/2026-11-21-phase1-phase2-five-boss.md`](benchmarks/2026-11-21-phase1-phase2-five-boss.md)):
  - Single-codebase `n = 8` report (`var/bench_boss_10_mixed_all.json`): `llm` `24/40` (`60.0%`) vs `llm_no_notes` `25/40` (`p = 1.0000`), `no_brain` `20/40` (`p = 0.5005`), `baseline` `22/40` (`p = 0.8213`).
  - Exploratory split-host `n = 8` run (`var/bench_boss_p12_llm.json` local `mixed, parallel=2` vs `var/bench_boss_p12_nonllm.json` remote `mixed_all, parallel=4` — **confounded by host/parallelism, not apples-to-apples**): `Ikiss` `llm` `5/8` (`1,022` DPS = `+1.4%` vs `llm_no_notes` `1,007`, `3/8`; remote `no_brain` `1/8`, `baseline` `0/8`), `Loken` `llm` `7/8` (`+15.4%` DPS) vs `llm_no_notes` `7/8`, `Devourer` `llm` `3/8` (`-0.6%` DPS) vs `llm_no_notes` `1/8` (remote `no_brain` `3/8` `4,797` DPS, `baseline` `1/8`), `Murmur` `llm` `4/8` (`-5.1%` DPS) vs `llm_no_notes` `6/8`, and `Kael'thas` `llm` `2/8` (`-23.3%` DPS regression: `1,453` vs `llm_no_notes` `1,894`, `4/8` — `Arcane Sphere` order reverted in `f644f1b`).
  - **Still open before Phase 3 sign-off:** `llm` kills do not yet significantly beat `no_brain` after Holm–Bonferroni correction, and `A3` draft notes (`content/notes/`) remain draft (only 1 boss section each in `FoS`/`Seth`/`ICC` was edited so far).
- **Phase 3 (`A5`, `A6`, `C2`, Patchwerk & ICC) live verification:**
  - `A5`, `A6`, and `C2` (`tools/bench/notes_hill_climb.py`) are implemented, unit-tested, and compiled into `out/server-test`; initial 10-man raid smoke runs completed (`var/bench_boss_raids_llm.json`: `Patchwerk` `0/1` kill, wipe `11` deaths, `11,291` DPS / `41.6 s`; `Lord Marrowgar` ICC 10-man with `-icc` stripped `0/1` kill, wipe `10` deaths, `26,709` DPS / `11,248` HPS / `220.7 s`, `1` valid + `5` invalid waves each from raid instance reuse on wave 2+; `var/bench_boss_raids_nonllm.json` on a remote worker had `9` invalid waves leaving `baseline` at `0/0` and `Lord Marrowgar` `no_brain` at `0/1` kill `17,129` DPS — `n = 1` split-host, not apples-to-apples). Live `C2` reflection re-run (with `enable_thinking: False` and resolved absolute `--trace-dir`) and `eregos` vehicle check remain open.
- **Pending user-approved `scorer.sha256` relock (Rule #6):** `server_module/src/snapshot/snapshot_builder.cc` (`A5` `self.unit.group` + `OptionFacts::party_targets`, `A6` `CollectUsableBagItems`) and `server_module/src/llm_brain_world_script.cc` (`A6` cast-time `kUse` item progress) were modified. Both `~/.local/share/wow-llm-bots/scorer.sha256` and tracked `config/scorer-lock.sha256` were left untouched awaiting explicit user OK.

## Next, in order

**Priority direction (2026-11-21):** Mechanical WoW competence (dungeons, raids/ICC, clearing quests,
soloing elite quests, smart gearing) takes priority over `"human-like"` mimicry (`D`). All Review
`ede3f6c` High (1–4), Medium (5–20), and Low findings are fixed and verified (unit tests, live e2e
`invite`/`whisper`/`orders`, `eval` `4/4` on answer-stripped mined cases, and single-codebase 5-boss
`n = 8` benchmark); remaining Phase 1/2 sign-off work is lifting `llm` boss kills significantly
above `no_brain` (e.g. `Kael'thas` `3/8` vs `baseline` `6/8`, `Devourer` `3/8` vs `5/8`) and
hand-checking draft `content/notes/` before enabling them.

1. **Close Phase 1/2 boss-kill sign-off gaps (`Kael'thas`, `Devourer` & pooled kill significance):**
   *(Done when: `llm` DPS is within `10%` of `llm_no_notes` on all 5 bosses and `llm` boss kills
   beat `no_brain` significantly after Holm–Bonferroni correction on a single-codebase apples-to-apples run.)*
2. **Phase 3 — Targeting, roles, items/vehicles, self-revising notes & raids / ICC (`A5`, `A6`, `C2`)**
   *(Promoted ahead of open-world / human-likeness per Decision #4. Done when: kills on Kael and
   Murmur up, Patchwerk with a proper comp, and raid / ICC encounter progression working)*:
   - **`A5. Targeting and roles` (implemented & unit-tested; live verification pending):** `"Attack
     <main assist/tank> <name>'s target (<mob>)"` option resolved by exact target GUID, `"main
     tank"` / `"main assist"` on screen, factual `party hurt:` (distance + LoS) and `loose mobs:`
     (when a tank is designated).
   - **`A6. Items and vehicles` (implemented & unit-tested; live verification pending; touches locked `snapshot_builder.cc` — relock pending explicit user OK):**
     exposes usable non-equip on-use bag items as `Use <item>` options (`docs/specs/options.md`,
     self/selected-target) so Eregos drakes, potions, Hearthstone, and raid consumables/vehicles
     work (unit/object/corpse quest-item targeting tracked in `Loose ends`).
   - **`C2. Let the brain revise its own `notes.md` (competence hill-climb loop)` (implemented & unit-tested; live verification pending):**
     [`tools/bench/notes_hill_climb.py`](../tools/bench/notes_hill_climb.py) summarizes fight
     timelines from wipes, runs a reflection LLM call to propose revised `notes.md`, validates
     standing orders, and gates edits on a short boss bench re-run scored by **kills, deaths, and
     damage taken** (`bench_bosses.py --notes-file`, plus `marrowgar` ICC 10-man).
4. **Phase 4 — Mechanical open-world competence: questing, solo elite quests, smart gearing & zero stalls (`B1–B3`)**
   *(Done when: quests per hour within `30%` of human traces, solo elite quests & smart gear
   upgrades working, and no `5 min` stalls in a `4 h` soak. Incorporates compact adaptations from
   Club WoW's structured-state agent while keeping our 4K-context / 16-slot / ~72 ms architecture —
   explicitly skipping 23K-token MCP schemas, 128K single-stream loops, and blocking combat tools)*:
   - **`B1. Questing over grinding (Atlas facts + tiny working goal + prompt inspector)`:**
     - *Compact Atlas facts (`sObjectMgr` live; `var/atlas-335a/atlas.sqlite` + `var/wiki-335a/wiki_335a.sqlite` offline):*
       show quest log objectives with nearest spawn distance & direction (falling back from
       `quest_poi` to `creature`/`gameobject`/`loot` spawns to cover the `153` obtainable indoor
       dungeon quests without POIs), offer `"Turn in <quest>"` as a single option pointing to the
       quest-ender NPC, and surface nearby quest givers beyond vision range.
     - *Tiny persistent working goal & outcome breadcrumbs:* carry a 1–2 line per-bot working goal
       (~50 tokens, prefix-stable) and a 1-line last-action failure/interruption breadcrumb across
       wakes, while live HP/mana/position/quest progress remain strictly in the fresh snapshot
       (live-state authority).
     - *Lightweight prompt/context inspector:* CLI/log dump of the exact assembled prompt and token
       breakdown for any bot wake to diagnose drift or bad decisions quickly.
   - **`B2. Upkeep without stalls & smart gearing`:** Full bags, broken gear, and trainable spells
     lead to nearest vendor/repair/trainer options from `sObjectMgr` (partly done), plus smart
     gear-upgrade evaluation across quest/dungeon/raid loot; add a soak check that no bot sits
     still for `>= 5 min`.
   - **`B3. Pull danger & soloing elite quests`:** Show pull danger in the snapshot (number of
     linked mobs, elites, level difference) so the brain can decline bad pulls or use cooldowns,
     kiting, and consumables to solo elite quests.
5. **Phase 5 — Model bake-off (`C4`) & fine-tuning exploration (`C3`, Decision #3: keep as TODO)**
   *(Done when: a significant bench gain at the same latency)*:
   - **`C4. Model choice bake-off`:** Try at least one other base model on `tools/wlb test eval`,
     `perf`, and the boss bench (same 4 arms, `n = 16`) before investing in `C3`, so the base model
     choice is settled with data. Candidate GGUFs pre-downloaded in `models/`:
     `gemma-4-E4B-it-qat-IQ4_XS-imatrix.gguf` and `Qwen3.8-27B-IQ4_XS.gguf`.
   - **`C3. Train the model on its own good decisions (TODO — Decision #3: evaluate cost/feasibility on one RTX 5090 first)`:**
     Once `C1` and `C2` produce a few thousand graded decisions, evaluate cost and benefit of:
     (a) a small LoRA fine-tune of Gemma on plans that led to kills; (b) preference pairs (good vs
     bad plan on the same snapshot), gated on offline eval plus the boss bench.
6. **Human-likeness with human traces (`D`, `docs/plans/self-improving-loop.md` — deprioritized behind mechanical gameplay & ICC per Decision #4):**
   - Already built/planned: record human play sessions across all situations (5 Hz + events, ~30%
     held out), run the parity and pairwise scorer, and run `tools/bench/blind_rate.py` batches +
     `llm-agree`.
   - AI-side additions when resumed: draw reaction delays, pauses, and pacing from human trace
     distributions instead of constants; compare bot choices against human choices in matched
     situations (pulls, looting, town visits) and turn mismatches into `C1` eval cases; keep chat
     reply rate and style rare and matched to logs.
7. **Named room landmarks** (`docs/specs/triggers.md`): 132 of the 218 unsayable boss actions move
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
- **Human trace GM-speed cleanup:** `2026-11-22.1.jsonl` in `~/.local/share/wow-llm-bots/human_baseline/human/Hele/` was cleaned of `> 10 yd/s` GM-fly/speed segments; strip `> 10 yd/s` segments from the still-open `2026-11-22.jsonl` after the play server stops.
- **Deliberately skipped benchmark speedup proposals (Items 4, 5, adaptive 6):** left unbuilt on
  purpose to preserve valid boss immunity/kite phases (Item 4: `20 s` zero-damage abort would
  cut `Gravity Lapse`/`Planar Shift` and censor `llm` bugs via survivorship bias), cross-boss DB
  isolation + startup-bound `LlmBrain.TraceDir` (`scorer.sha256` lock) (Item 5: cross-boss
  server reuse without `test_databases_reset()` saves only `~44 s/boss`), and unbiased fixed-`n`
  statistics (adaptive Item 6: stopping at `n=3` has a `42.2%` false-negative rate on a `75%` arm;
  details in [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md) §6).

## Done (newest first, keep about ten)

- 2026-11-22: **Linked `private/BigFootAddOns` (3.3.5a / Interface 30300 BigFoot English port):**
  created private repo `superliuxz/BigFootAddOns` (`14e639a` baseline, `e20e957` `enUS` port: all missing `BigFoot/Config/*.lua` English tooltips, `MerInspect` un-gated & `enUS` localization + `Buff.lua` nil guard, `RaidAlerter/Localization-enUS.lua`, `BFClassMods/Aspects.lua`, `SellerHelperFrame.xml`, `SpellID/core.lua`, `QuestInfo/Notes.lua`, `BigFoot.toc` casing, and `enUS` font fallbacks; `32e3fca` updates `DBM-*` to `10.1.13_alpha` backport, `GearScoreLite` `3x05`, `BlizzMove` `v0.7`, `Omen` `3.0.9`, and `Bagnon_Forever` `1.1.2` with `GS_Toggle`/`DBM.Options` guards; syntax-checked with `luajit -bl` and XML-parsed; live in-game WoW 3.3.5a load not yet tested) and linked it as a submodule at `private/BigFootAddOns` (`private`).

- 2026-11-22: **Review `6d547b7` fixes (Medium 1–2, Low 1–3) & absolute `--trace-dir` resolution:**
  - *`A6` spell-specific `SpellQueue` & cast-state helper (`M1`, `L2`):* shared `IsSpellCastPending(const Player*, uint32_t)` in [`option_executor.{h,cc}`](../server_module/src/options/option_executor.cc) filtering `SpellQueue` by `!req.cancelInProgress && req.spellId == spell_id`, checking `SPELL_STATE_PREPARING` for `CURRENT_GENERIC_SPELL` (excluding `SPELL_STATE_DELAYED`) and `!= SPELL_STATE_FINISHED` for `CURRENT_CHANNELED_SPELL`; [`llm_brain_world_script.cc`](../server_module/src/llm_brain_world_script.cc) removes the duplicated `IsCastingItemSpell`, uses `IsSpellCastPending`, and resets `active_item_spell_id = 0` at all 5 `active_option.reset()` sites.
  - *`C2` exact `orders.wlb` validator, tests, & absolute `trace_dir` (`M2`, `L1`, `L3`):* [`notes_hill_climb.py`](../tools/bench/notes_hill_climb.py) matches `harness/grammar/orders.wlb` + `wlb_runtime.cc` (`_is_name`, `_is_num`, `_is_int_in`, `_is_thing`, `_is_place`, `_is_check`, exact single-space literals, strict `away` bounds) and resolves `out_dir` to an absolute path; [`bench_bosses.py`](../tools/bench/bench_bosses.py) resolves `trace_dir` to an absolute path so `worldserver` writes traces where `_TraceTailer` reads them; [`notes_hill_climb_test.py`](../tools/bench/notes_hill_climb_test.py) (`7/7`) tests all Medium 2 negatives, unsorted/multi-file shard sorting, `enable_thinking: False` request payload, and absolute `trace_dir`; updated `docs/TODO.md` raid smoke-run details (`L3`). Verified via `tools/wlb build server --test` and `tools/wlb check` (`ALL CHECKS PASSED`).

- 2026-11-22: **Review `ed78f6c` fixes (Medium 1–2, Low 1–2) & `C2` `enable_thinking: False`:**
  - *`A6` `SpellQueue`, specific spell tracking, & safe post-use lookup (`M1`, `L1`):* [`option_executor.{h,cc}`](../server_module/src/options/option_executor.cc) adds `ExecuteResult::pending_spell_id`, checks `Player::SpellQueue` growth and `IsCastingSpecificSpell(bot, item_spell_id)` (excluding `CURRENT_AUTOREPEAT_SPELL`), and re-fetches the post-use `Item*` via `GetItemByPos` + GUID verification; [`llm_brain_world_script.cc`](../server_module/src/llm_brain_world_script.cc) tracks `active_item_spell_id` + `SpellQueue` in `OptionTargetProgress` and early-returns for `"item:"` in `StepActiveOption`.
  - *`C2` `orders.wlb` validation, chronological sample sort, & Gemma 4 thinking toggle (`M2`, `L2`):* [`notes_hill_climb.py`](../tools/bench/notes_hill_climb.py) enforces case-sensitive `orders.wlb` `Who`/`Icon`/`State`/`MyState` (`in vehicle`, `nearest to it`)/`Digit`/`Num` (`1dp`)/no-space `<`/`>`/`away` arg bounds, sorts `samples_by_bot` by `t_ms`, and passes `chat_template_kwargs.enable_thinking=False`; [`notes_hill_climb_test.py`](../tools/bench/notes_hill_climb_test.py) cross-checks all `>300` audited `fits` rows in [`docs/specs/trigger-conditions.tsv`](specs/trigger-conditions.tsv) plus 20 negative cases. Updated `docs/TODO.md` raid wipe figures and `Last updated` (`L2`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and `tools/wlb build server --test`.

- 2026-11-22: **Review `f6def2d` fixes (High 1, Medium 1–3, Low 1–2):**
  - *`A6` cast-time & charged bag items (`H1`, `M1`):* [`option_executor.cc`](../server_module/src/options/option_executor.cc) skips depleted charged bag stacks when resolving `item:<entry>`, detects item use via count/charges/cast/cooldown/GCD/aura/mana/vehicle changes, and returns `used = !cast_started`; [`llm_brain_world_script.cc`](../server_module/src/llm_brain_world_script.cc) `OptionTargetProgress` holds `kUse` `"item:<entry>"` options `pending()` while casting until the cast finishes.
  - *`C2` & `bench_bosses.py` (`M2`, `M3`, `L1`, `L2`):* [`notes_hill_climb.py`](../tools/bench/notes_hill_climb.py) bounds event timelines using real `FormatTraceSample` `"inst"` transitions (no fabricated `"inst"` on events), validates both LHS `When` conditions and RHS zero-arg / enum (`stop`, `pet`) / argument acts strictly against `orders.wlb`, and logs `conf_score` (+ `initial_score`) on failed confirmation; [`bench_bosses.py`](../tools/bench/bench_bosses.py) scopes `--trace-dir` per boss tag when `len(keys) > 1`. Tested in [`notes_hill_climb_test.py`](../tools/bench/notes_hill_climb_test.py) (`5/5`, incl. sample-inst bounding, confirmation keep/fail, `>15%` DPS guard, strict `orders.wlb` rejections) and [`dodge_timeline_test.py`](../tools/bench/dodge_timeline_test.py) (`19/19`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and `tools/wlb build server --test`.

- 2026-11-21: **Review `4d362db` fixes (High 1, 3, 4, Medium 1, 3, 4, Low 1–2; High 2 & Medium 2 partly — see Review `f6def2d` above):**
  - *`PlanRunner` (`H1`):* [`plan_runner.cc`](../harness/src/plan/plan_runner.cc) strips `"assist:"` before `FindUnitByGuid` in `IsTargetDone` and combat HP-drop timeout tracking, and keeps `kUse` `"item:<entry>"` options active until `done_command_id` arrives (tested in [`plan_runner_unittest.cc`](../harness/src/plan/plan_runner_unittest.cc)).
  - *`A6` bag items (`H2` partial, `L2`):* [`snapshot.h`](../snapshot/snapshot.h) changes `OptionFacts_Item::out_of_combat_only` to default `false` (fixing Glaze JSON round-trip tested in [`options_unittest.cc`](../server_module/src/options/options_unittest.cc)); [`option_executor.cc`](../server_module/src/options/option_executor.cc) filters `INVTYPE_NON_EQUIP`, `SpellId > 0`, depleted `SpellCharges`, all-spell combat restrictions, and `StopMoving()` on cast-time items (cast-time completion in `OptionTargetProgress` and fuller instant/charged success checks remain open per Review `f6def2d` H1/M1).
  - *`bench_bosses.py` & `C2` (`H3`, `H4`, `M1`, `M2` partial, `M3`, `L2`):* [`bench_bosses.py`](../tools/bench/bench_bosses.py) adds `strip_strat="icc"` to `marrowgar`, reads `--notes-file` before `LlamaServer.__enter__()`, and adds `--trace-dir`; [`notes_hill_climb.py`](../tools/bench/notes_hill_climb.py) isolates per-round `trace_dir`, holds one persistent `LlamaServer` in `main()`, guards against death increases / DPS collapse and supports `--confirm-runs`, and records rejected standing orders in `rejected_history` (real-trace sample-based `inst` bounding and strict `orders.wlb` condition/verb validation remain open per Review `f6def2d` M2/M3).
  - *`screen_renderer.cc` & `TODO.md` (`L1`, `M4`):* guarded `hurt` with `std::isfinite(*u.health_pct)` before `std::lround` and aligned `docs/TODO.md` `Loose ends` / `Next` / `Done`. Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and `tools/wlb build server --test`.

- 2026-11-21: **Review `f644f1b` fixes (High 1–3, Medium 1–3, Low 1–3) + Phase 3 `A6` (usable bag items) & `C2` (`notes_hill_climb.py` + ICC `marrowgar`):**
  - *Review `f644f1b` (`A5` & `bench_bosses.py`):* [`snapshot_builder.cc`](../server_module/src/snapshot/snapshot_builder.cc) populates `base.self.unit.group` (`FillSelf`) and `OptionFacts::party_targets` (`FillOptionFacts`); [`options.cc`](../server_module/src/options/options.cc) resolves assist options by exact `target_guid`, emits distinct synthetic `option.guid = "assist:" + best->guid` (tracked in `plan_runner.cc` in the follow-up fix above), and skips `m.name == snapshot->bot`; [`screen_renderer.cc`](../harness/src/screen/screen_renderer.cc) includes `self` in `tank_names`, suppresses `loose mobs:` when no tank is designated (preventing non-LFG 5-man inversion), includes pulled `NEUTRAL` mobs, removes the hardcoded 40 yd heal-range verdict from `party hurt:` (pure distance + LoS), and checks `std::lround(*u.health_pct) < 100`; [`bench_bosses.py`](../tools/bench/bench_bosses.py) rejects empty `--arms`, aligns `_needs_llm()` with `run_boss_waves()`, and scopes `trace_dir` by `--arms`; expanded unit tests in [`screen_renderer_unittest.cc`](../harness/src/screen/screen_renderer_unittest.cc), [`options_unittest.cc`](../server_module/src/options/options_unittest.cc), and [`dodge_timeline_test.py`](../tools/bench/dodge_timeline_test.py); corrected `docs/TODO.md` benchmark/draft-notes honesty (`H3`, `M1`, `L2`).
  - *Phase 3 `A6` & `C2`:* added `OptionFacts_Item` ([`snapshot.h`](../snapshot/snapshot.h)), `CollectUsableBagItems` & `ExecuteUseItemOption` ([`option_executor.{h,cc}`](../server_module/src/options/option_executor.cc)), and `Use <item>` bag options ([`options.cc`](../server_module/src/options/options.cc)); added [`tools/bench/notes_hill_climb.py`](../tools/bench/notes_hill_climb.py) + [`tools/bench/notes_hill_climb_test.py`](../tools/bench/notes_hill_climb_test.py) (`4/4`, wired into [`tools/wlblib/check.py`](../tools/wlblib/check.py)), `--notes-file`, and `marrowgar` (ICC 10-man) in [`bench_bosses.py`](../tools/bench/bench_bosses.py). Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and `tools/wlb build server --test`.

- 2026-11-21: **User-approved scorer lock update (`951d4cc17f25...`):**
  recorded user-approved [`config/scorer-lock.sha256`](../config/scorer-lock.sha256) digest (`951d4cc17f25...`) after interactive `hill_climb.py lock-scorer --force-relock` for Review 12 Medium 1 and Review `ede3f6c` scorer fixes, verified via `hill_climb._verify_all_locks()`.

- 2026-11-21: **Review `1ba5b4b` & `97cbca4` fixes (`97cbca4`, follow-up Low 1) & `tools/wlb review --watch` (`1ba5b4b`):**
  [`.agents/hooks/post_commit_review.py`](../.agents/hooks/post_commit_review.py) bumps `--print-timeout` to `600s`, adds `--watch`, reports `FAILED` on `AGY_ERROR` or case-insensitive `error:` in `.err` regardless of partial stdout size (`M1`/`M2`), reports `FAILED (empty_output)` on 0-byte `.md` files (`M2`), and rejects unknown trailing args after `--status` (`M3`); [`tools/wlb`](../tools/wlb) routes `review [--watch]` through `parse()` so `--help` works and unknown flags fail (`M3`); [`.agents/hooks/post_commit_review_test.py`](../.agents/hooks/post_commit_review_test.py) (`5/5`) tests `600s`, exact partial-output byte count / capital-`Error:` / empty-output `FAILED` states, `_watch_review_status()` with captured stdout assertions, and CLI flag validation (`L1`); updated `docs/TODO.md` (`M4`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and live `tools/wlb review --help` / `tools/wlb review`.

- 2026-11-21: **Review `d568a0f` fixes (`deee315`; Medium 1, Low 1–2) & post-commit reviewer visibility (`tools/wlb review`):**
  [`tools/wlblib/remote.py`](../tools/wlblib/remote.py) `status()` `/proc` probe now uses `pwd -P` (symlinked repos), requires a path boundary in `$args` (`*"$REPO_ABS/"*|*"$REPO_ABS "*`, avoiding sibling-prefix matches), and skips `$PPID` (`M1`); [`tools/wlblib/remote_test.py`](../tools/wlblib/remote_test.py) (`22/22`) now executes the generated `status()`, `_kill_remote_pgid()`, and `_invalidate_remote_stamp()` bash snippets directly against real subprocesses in a `TemporaryDirectory` (symlinked repo, sibling prefix, invalid PGIDs `0`/`1`/`-1`/`abc`, single `KeyboardInterrupt` mock, `L1`) and records unverified live `Ctrl-C`/SSH-drop paths in `Loose ends` (`L2`); [`.agents/hooks/post_commit_review.py`](../.agents/hooks/post_commit_review.py) records spawned reviewer PID/SHA/start time in `var/review/active.json`, emits a `systemMessage` on spawn, and exposes `--status` wired as `tools/wlb review` in [`tools/wlb`](../tools/wlb) (tested in [`.agents/hooks/post_commit_review_test.py`](../.agents/hooks/post_commit_review_test.py) `4/4`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and live `tools/wlb review`.

- 2026-11-21: **Review `e6fa063` fixes (`d568a0f`; High 1, Medium 1–2, Low 1–3):**
  [`tools/wlblib/remote.py`](../tools/wlblib/remote.py) switches `run_remote()` to `setsid --wait bash -c ...` with an in-shell `EXIT` trap for `var/.active_remote_pgid` (propagating non-zero exit codes cleanly and avoiding a redundant second SSH round-trip on normal exit, `High 1` / `M2`), kills the remote PGID on SSH transport drop (`rc == 255`, `M2`), validates `$PGID > 1` before `kill -TERM/-KILL -"$PGID"` (`M2`), scopes `status()` process detection via `/proc/[0-9]*/{cwd,exe,cmdline}` so relative `./worldserver` and `bench_bosses.py` processes are detected (`M1`), and fixes `_invalidate_remote_stamp()` to only skip a missing repo dir (`L3`); [`tools/wlb`](../tools/wlb) removes the unreachable `elif a.command == "remote":` branch (`L3`); [`tools/wlblib/remote_test.py`](../tools/wlblib/remote_test.py) expands to `21/21` tests adding `status()`, `_kill_remote_pgid()`, `_invalidate_remote_stamp()`, `_rsync_to_remote()`, SSH-drop `255`, atomic fresh `os.open(..., 0o600)` DB password creation, and real `git worktree` `install_hook()` (`L2`); scrubbed personal worker name from `docs/TODO.md` (`L1`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and live remote worker checks (`exit 7` propagation, `--pull` round-trip, and `status`).

- 2026-11-21: **Review `693eb3a` fixes (`e6fa063`; High 1, Medium 1–5, Low 1–5):**
  [`tools/wlblib/remote.py`](../tools/wlblib/remote.py) now invalidates remote `var/.built_stamp` before pushing (`High 1`), tracks remote jobs in a `setsid` process group (`var/.active_remote_pgid`) and kills the full remote PGID on `KeyboardInterrupt` before releasing the local `flock` (`M3`), guards one-time map-data push with a post-transfer `out/server/data/.synced` sentinel (`M4`), passes `-e "ssh -o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=30 -o ServerAliveCountMax=3"` and `--` to all `rsync` calls while validating `--pull` paths before running (`M5`), replaces `argparse.REMAINDER` ambiguity with strict `parse_remote_argv()` / `dispatch_cli()` routed directly in [`tools/wlb`](../tools/wlb) (`L1`), excludes `/reference/`, `/.cache/`, `__pycache__/`, `*.pyc`, and unanchored `.git` (`L2`), runs remote scripts under `bash -euo pipefail -c` (`L3`), removes unused `import sys`, makes `install_hook()` worktree-safe (`git rev-parse --git-path`) using `repo / "var"`, and scopes `status()` process listing to `spec.repo` (`L4`); [`tools/wlblib/server.py`](../tools/wlblib/server.py) `chmod(0o700)`s `PLAY.run` and `SECRETS`, migrates legacy `.acore-db-password` before atomically creating `DB_PASSWORD` (`0o600`, `M1`), and runs idempotent `_ensure_acore_users()` on every `db_start()` (`M2`); [`tools/wlblib/remote_test.py`](../tools/wlblib/remote_test.py) expanded to `18/18` tests (`L5`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and live remote worker `sync` + `status` (`UP TO DATE`).

- 2026-11-21: **Self-bootstrapping remote CPU worker support (`tools/wlb remote`, `693eb3a`):**
  added [`tools/wlblib/remote.py`](../tools/wlblib/remote.py), wired `tools/wlb remote list | <name> sync|status|run|pull|stop|install-hook` in [`tools/wlb`](../tools/wlb), added unit tests in [`tools/wlblib/remote_test.py`](../tools/wlblib/remote_test.py) (wired into [`tools/wlblib/check.py`](../tools/wlblib/check.py)), updated [`tools/wlblib/server.py`](../tools/wlblib/server.py) so fresh/`.git`-less remote slaves auto-initialize a clean test-only MySQL datadir and fall back to `var/.golden_submodules`, documented optional remote workers generically in [`CONTRIBUTING.md`](../CONTRIBUTING.md) & `.agents/rules/contributing.md`, and stored personal worker config in `private/remotes.json`. Verified via `tools/wlb check` (`ALL CHECKS PASSED`) and a live 4-party `mixed_all` 1-boss `ikiss` smoke run on a remote worker (pulled JSON was deleted after inspection; no `var/` artifact retained for that smoke run).

- 2026-11-21: **Review `ede3f6c` fixes (`ada34f2`; High 1–4, Medium 5–20, and Lows; scorer relock pending explicit user OK):**
  - *Acts & Orders (`High 1`, `M5–M8`, `Lows`):* `act_executor.cc` stops `away`/`out of` from auto-walking back (resumes attack target only; returns only on explicit `back`), adds bot combat reach to `out of` (`act_geometry.{h,cc}`), and removes `CanEscapeBeforeImpact` C++ judgment; `plan.wlb`/`plan.{h,cc}`/`bot_loop.cc` add explicit `orders: none` to clear standing orders (otherwise keeping orders consistently in/out of combat) and stop mid-fight plans from overwriting the pre-pull cache; `content/RULES.md` lists `in`, `out of`, `back`, `orders: none`; `snapshot.h` bumps `kWireVersion` to `4`.
  - *Invite & Chat (`High 2`, `M14–M16`, `Lows`):* `bot_manager.cc` attaches `BlockAutoInviteListener` to block Playerbots' `"accept invitation"` so the brain decides (`tools/wlblib/cases/e2e_invite.py` live-verified `PASS`); `chat_scripts.cc` delivers `Say`/`Yell` via `Cell::VisitObjects` (same map, instance, phase, thread); `speak.cc` routes `PARTY`/`RAID`/`GENERAL` through `OnPlayerCanUseChat`; `bot_loop.cc` restricts `tell` to whisperers and party members; `llm_brain_world_script.cc` adds outbound chat rate limiting (5/10 s, 1 s min spacing) and logs byte length instead of full chat text; `behavior_trace_scripts.cc` renames `cast_start` → `spell_cast`.
  - *Notes, Mined Evals, Bench (`M9–M13`):* `extract_boss_notes.py` & `orders.cc` drop bare-keyword templates, resistance/safe-area false positives, blind `away boss 10` fallbacks, digit-range junk, and non-game headings (regenerated draft `content/notes/`); `mine_traces.py` & `eval_plans.py` strip standing-order answers from notes, enforce `must_order_act`, attribute damage/deaths to the casting boss, and infer class (`4/4` mined evals passing `tools/wlb test eval`); `dodge_timeline.py` requires in-range + casting/attacking for `BackMs`; re-ran all 5 bosses on one codebase (`n = 8`, 160 valid trials, `0` invalid: `llm` recovered from `10/40` to `24/40`, `Murmur 7/8`, `Loken 6/8`; [`docs/benchmarks/2026-11-21-phase1-phase2-five-boss.md`](benchmarks/2026-11-21-phase1-phase2-five-boss.md)).
  - *Scorer & Checks (`High 3–4`, `M17–M20`, `Lows`):* `trace_parity.py` preserves raw pre-resampling snap-turn rates and strips chat text in anonymized clips; `pairwise_ranker.py` formats human/bot social views identically as `<len=N>` and raises default `check_judge_agreement` `min_pairs` to `30`; `blind_rate.py` requires `>= 10` human-vs-bot pairs for `llm-agree`; `adversary_suite.py` computes raw reaction times before 200 ms grid snapping; `soak_scorer.py` raises judge pairs to `72` (focused on `candidate` vs `current`) and requires `>= 20` complete blind ratings; `scorer_calibration.py` requires a `25` Elo margin and relative `bad_caught`; `hill_climb.py` adds pending-candidate + `score-candidate` workflow, read-only (`0444`) baseline traces, `_RelockAuth` token instead of bare `confirmed=True`, and scoped `git add --` with `check=True`; `tools/wlblib/check.py` runs all 9 Python test suites under `-I`. Verified via `tools/wlb check` (`ALL CHECKS PASSED`), `tools/wlb test e2e invite whisper orders` (`3/3`), `tools/wlb test eval` (`4/4` mined), and 160-trial 5-boss benchmark.

- 2026-11-21: **Offline 3.3.5a Wiki & Atlas SQLite databases + candidate bake-off models:**
  pre-downloaded `gemma-4-E4B-it-qat-IQ4_XS-imatrix.gguf` (`4.7 GB`) and
  `Qwen3.8-27B-IQ4_XS.gguf` (`15 GB`, both with `.sha256`) into untracked `models/` for `C4`;
  extracted full pre-Cataclysm (`<= 2010-10-11T23:59:59Z`) Warcraft Wiki from Archive.org
  `warcraft.wiki.gg-20231113-history.xml.zst` (`637 MB`) into `var/wiki-335a/wiki_335a.sqlite`
  (`252.2 MB`, `91,152` articles / `80,325` non-redirects incl. `9,527` `{{questbox` quest pages
  and 3.3.5a boss tactics, FTS5 `0.3 ms`); exported local `acore_world` MySQL into
  `var/atlas-335a/atlas.sqlite` (`35.9 MB`, `9,464` quests with `7,300` linked to
  `wiki_335a.sqlite`, `9,762` objectives, `16,262` quest starter/ender relations, `18,801` POI
  centroids, `150,176` creature + `96,677` GO spawns, `120,005` item drops, `4,110` service NPCs,
  FTS5); and audited `acore_world` `quest_poi` coverage (`8,965/9,464 = 94.7%`; remaining `499`
  are `346` unobtainable + `153` mostly indoor dungeon quests covered by spawn/loot tables).

- 2026-11-21: **Review 12 fixes (Medium 1, passphrase doc honesty, & post-commit review hook Lows 1–6):**
  updated [`tools/bench/hill_climb.py`](../tools/bench/hill_climb.py) `_verify_all_locks` to always verify against tracked [`config/scorer-lock.sha256`](../config/scorer-lock.sha256) even when `--lock-file` is overridden (tested in [`tools/bench/hill_climb_test.py`](../tools/bench/hill_climb_test.py) `8/8`); stated plainly in [`CONTRIBUTING.md`](../CONTRIBUTING.md), `.agents/rules/contributing.md`, and [`docs/plans/self-improving-loop.md`](plans/self-improving-loop.md) that the external passphrase file is not a technical barrier against an agent running as the same Unix user and that protection comes from the rule + tracked digest diff; hardened [`.agents/hooks/post_commit_review.py`](../.agents/hooks/post_commit_review.py) to review `last_reviewed..HEAD` only on strict descendant moves (skipping checkout/reset/amend), pass a minimal sanitized env, guard concurrent spawns with `fcntl.flock`, update `last_reviewed_sha` only after `Popen` succeeds, document plan-mode/untrusted-diff & `jetski-hub` loop-guard boundaries and `WLB_SKIP_COMMIT_REVIEW=1` opt-out, and added unit tests in [`.agents/hooks/post_commit_review_test.py`](../.agents/hooks/post_commit_review_test.py) (`3/3`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`).

- 2026-11-21: **Phase 1 (`A1` + `A2`) & Phase 2 (`A3` + `A4` + `C1`) initial draft (`24b4743`, `221b706`, `d7b54f4` — superseded/broken per Review `ede3f6c`):**
  implemented initial `Cast.radius_yd`/`impact_pos` + trigger-NPC perception, `in`/`out of`/`back` grammar & `act_executor`, `dodge_timeline.py` metrics, `extract_boss_notes.py` (draft), `A4` pre-pull orders, `mine_traces.py` (draft), and `gear_quality=2`. Review `ede3f6c` identified High 1 (`away` auto-walking back cut kills `10/40` vs `24/40`, `p = 0.003`), Mediums 5–13 (combat reach, order clear/cache, C++ judgment, keyword-template notes, leaking mined evals, broken `BackMs`, mixed-commit report); all tracked in `In progress`.

- 2026-11-21: **External relock passphrase file, `n = 16` roadmap numbers, accurate eviction comments & post-commit subagent review hook:**
  replaced AI process/cgroup/env-var detection in
  [`tools/bench/hill_climb.py`](../tools/bench/hill_climb.py) (`AGENT_ENV_VARS`,
  `AGENT_PROCESS_NAMES`, `_has_agent_ancestor`, `_has_agent_cgroup`, `_is_agent_run`,
  `_resolve_lock_file`) with an external passphrase file
  (`~/.local/share/wow-llm-bots/relock.passphrase`, supporting plaintext or 64-hex SHA-256 digest,
  verified interactively via separate `"r"`/`"w"` handles on `/dev/tty` and tested against a real
  `pty.openpty()` slave character device in
  [`tools/bench/hill_climb_test.py`](../tools/bench/hill_climb_test.py)); updated
  [`CONTRIBUTING.md`](../CONTRIBUTING.md) and `.agents/rules/contributing.md`; updated `docs/TODO.md`
  roadmap and `Loose ends` from `n = 8` figures to `n = 16` (`d7b3e13`, `64` valid trials/boss);
  removed generous `O(1)` claims from cap-eviction comments in
  [`behavior_trace.{h,cc}`](../server_module/src/snapshot/behavior_trace.h),
  [`behavior_trace_unittest.cc`](../server_module/src/snapshot/behavior_trace_unittest.cc), and
  `docs/TODO.md`; re-locked `~/.local/share/wow-llm-bots/scorer.sha256` and
  [`config/scorer-lock.sha256`](../config/scorer-lock.sha256) (`11b686bde7f5...`, all 28
  measurement locks verified); and added `.agents/hooks.json` +
  [`.agents/hooks/post_commit_review.py`](../.agents/hooks/post_commit_review.py) + Rule #7 to
  spawn a detached, read-only `agy --print --mode plan` reviewer in a separate top-level
  conversation after every commit (`var/review/review_<short_sha>.md` / `var/review/latest.md`).

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
  cap-eviction scans under `mu_` with timestamp-ordered intrusive `all`/`idle` index lists in
  `RoleSampleTracker` so cap eviction checks only list heads (appending at `tail` on monotonic
  timestamps and walking back from `tail` on out-of-order timestamps; tested in
  `EvictsIdleTrackedCharactersAndReportsCapOverflow` with `1,000` overflow samples & out-of-order
  stale eviction). Verified via `tools/wlb check` (`ALL CHECKS PASSED`:
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
