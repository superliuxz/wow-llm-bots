# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-21.

## In progress

- **Scorer lock relock (awaiting explicit user OK):** `tools/bench/hill_climb.py` (in `LOCKED_PATTERNS`) was modified for Review 12 Medium 1; `~/.local/share/wow-llm-bots/scorer.sha256` and `config/scorer-lock.sha256` are left untouched pending user approval.
- **Phase 1 & Phase 2 remaining benchmark sign-off criteria** (implementation + unit/eval verification complete; see [`docs/benchmarks/2026-11-21-phase1-phase2-five-boss.md`](benchmarks/2026-11-21-phase1-phase2-five-boss.md) for `n = 8` 5-boss results):
  - **Done & verified:** `A1` dodge-then-return (`med_back_ms = 11–313 ms`), `A2` server-radius-aware `out of it` + `Cast.radius_yd`/`impact_pos` + trigger-area perception (`Flame Strike` `24666`/`44191`), `Kael'thas` `Flame Strike` gap **closed** (`llm` `7,460` total / `6/8 <= 1k` vs `baseline` `11,229` / `5/8 <= 1k`), DPS within `10%` of `llm_no_notes` on `kaelthas` (`-9.6%`), `ikiss` (`+5.2%`), and `devourer` (`-7.1%`), `A3` playerbots-script-to-notes extractor (`41` instances / `647` triggers → `content/notes/`) + grammar parity (`in <mech>`, `out of <thing>`, `back`), `A4` pre-pull standing-order compilation on instance entry + cache + mid-fight order preservation, `C1` trace-to-eval miner (`4/4` mined boss eval cases passing `tools/wlb test eval`), and `gear_quality=2` on `loken`/`devourer` breaking the `16/16` ceiling.
  - **Still open for full Phase 1/2 sign-off:** (1) `Murmur` `0/8` (`-37.5%` DPS) because tank running 38 yd away on `Sonic Boom` triggers `Resonance` raid-wide wipe (needs role-aware notes so tank stays in melee); (2) `Loken` `2/8` (`-38.9%` DPS) because ranged bots stay at 25–30 yd eating distance-scaled `Pulsing Shockwave` instead of stacking between `Lightning Nova`s; (3) `llm` boss kills beating `no_brain` significantly after Holm–Bonferroni correction (`10/40` vs `19/40`, `p_pool = 0.062`).

## Next, in order

**Priority direction (2026-11-21):** Mechanical WoW competence (dungeons, raids/ICC, clearing quests,
soloing elite quests, smart gearing) takes priority over `"human-like"` mimicry (`D`). Where we
stand after the Phase 1 (`A1–A2`) & Phase 2 (`A3–A4`, `C1`) 160-trial (`n = 8` per arm) 5-boss
benchmark ([`docs/benchmarks/2026-11-21-phase1-phase2-five-boss.md`](benchmarks/2026-11-21-phase1-phase2-five-boss.md)):
`A1` dodge-then-return (`med_back_ms = 11–313 ms`), `A2` server-radius-aware `out of it` +
trigger-area perception, `A3` script-generated notes (`41` instances / `647` triggers) + grammar
parity (`in`, `out of`, `back`), `A4` pre-pull standing-order compilation + cache, and `C1`
trace-mined eval cases (`4/4` passing `tools/wlb test eval`) are implemented and verified;
`Kael'thas` `Flame Strike` parity vs `baseline` is **closed** (`7,460` / `6/8 <= 1k` vs `11,229` /
`5/8 <= 1k`); `gear_quality=2` on `Loken` and `Devourer` broke their `16/16` ceilings without
dropping bosses; and `llm` DPS is within `10%` of `llm_no_notes` on `kaelthas` (`-9.6%`), `ikiss`
(`+5.2%`), and `devourer` (`-7.1%`). Remaining Phase 1/2 gaps before Phase 3: `Murmur` `0/8`
(`-37.5%` DPS, tank leaving melee triggers `Resonance` wipe), `Loken` `2/8` (`-38.9%` DPS, ranged
bots eat distance-scaled `Pulsing Shockwave` instead of stacking), and `llm` kills vs `no_brain`
after Holm–Bonferroni correction (`10/40` vs `19/40`).

1. **Close Phase 1 & Phase 2 boss-kill sign-off gaps (`Murmur` `Resonance`, `Loken` `Pulsing Shockwave`, `Ikiss` LoS):**
   *(Implementation of `A1–A4` and `C1` is done; done when: `llm` DPS is within `10%` of
   `llm_no_notes` on all 5 bosses and `llm` boss kills beat `no_brain` significantly after
   Holm–Bonferroni correction)*:
   - Role-aware standing-order notes for `Murmur` (keep tank in melee during `Sonic Boom` so
     `Resonance` never wipes the party) and `Loken` (stack close between `Lightning Nova` casts to
     minimize `Pulsing Shockwave`), then re-verify on the 5-boss benchmark.
2. **Phase 3 — Targeting, roles, items/vehicles, self-revising notes & raids / ICC (`A5`, `A6`, `C2`)**
   *(Promoted ahead of open-world / human-likeness per Decision #4. Done when: kills on Kael and
   Murmur up, Patchwerk with a proper comp, and raid / ICC encounter progression working)*:
   - **`A5. Targeting and roles`:** Add an `"Attack <name/role>'s target"` option and show `"main
     assist"` on screen; show healers who is taking damage and who is in range; show the tank loose
     mobs (who isn't targeting it). Covers the Loken self-target death and Kael adds without C++
     guessing.
   - **`A6. Items and vehicles`:** Expose usable bag items as options (`Use <item>`,
     `docs/specs/options.md`) so Eregos drakes, potions, quest items, and raid consumables/vehicles
     are possible.
   - **`C2. Let the brain revise its own `notes.md` (competence hill-climb loop)`:** After a wipe,
     run a slower reflection call over the fight timeline (what hit whom, when, and which orders
     fired) to propose a `notes.md` edit; keep the edit only if a short bench re-run doesn't get
     worse (hill-climb loop aimed at notes, scored by **kills and damage taken**, not
     human-likeness).
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
  - *(Fixed by `A1` hold-until-cast-ends and `A4` mid-fight empty-order preservation; remaining edge
    case:)* Gemma 4 occasionally rewrites a working `Lightning Nova cast` order into `Loken cast`
    or `Lightning Nova appears` when a mid-fight wake emits non-empty `order:` lines.
- **Not individually live-triggered after final edits yet:** `llmtest questcomplete` when a quest
  is already active in a bot's quest log (`CompleteQuest` + `RewardQuest` branch in
  `test_commands.cc`, source-verified), plus Review 10 single-arm `_wait_checkstrat` +
  `+{strip_strat}` (`tools/bench/bench_bosses.py`) and sorted list-head cap eviction
  (`server_module/src/snapshot/behavior_trace.{h,cc}`), which are unit-tested and compiled into
  `out/server-test` without starting a new live benchmark run (per user instruction).
  *(Environmental/self death `died` with `detail="environment"` caveat closed per Review 9 —
  live-triggered on `murmur`, and entrance staging/pull coordinates for `devourer`, `loken`,
  `kaelthas`, `murmur`, and `ikiss` live-verified across `64` valid trials (`n = 16` per arm)
  each.)*
- **Deliberately skipped benchmark speedup proposals (Items 4, 5, adaptive 6):** left unbuilt on
  purpose to preserve valid boss immunity/kite phases (Item 4: `20 s` zero-damage abort would
  cut `Gravity Lapse`/`Planar Shift` and censor `llm` bugs via survivorship bias), cross-boss DB
  isolation + startup-bound `LlmBrain.TraceDir` (`scorer.sha256` lock) (Item 5: cross-boss
  server reuse without `test_databases_reset()` saves only `~44 s/boss`), and unbiased fixed-`n`
  statistics (adaptive Item 6: stopping at `n=3` has a `42.2%` false-negative rate on a `75%` arm;
  details in [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md) §6).

## Done (newest first, keep about ten)

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

- 2026-11-21: **Phase 1 (`A1` + `A2`) & Phase 2 (`A3` + `A4` + `C1`) implementation, `Flame Strike` parity closed, & 5-boss (`n = 8`) benchmark (`24b4743`, `221b706`, `d7b54f4`):**
  implemented `Cast.radius_yd`/`impact_pos` + hidden trigger-NPC visible aura perception (`24666`/`44191` `Flame Strike`) in `nearby_builder.cc`; added `in <mech>`, `out of <thing>`, and `back` to `harness/grammar/orders.wlb`, `order_evaluator.cc`, `act_geometry.{h,cc}`, `act_executor.{h,cc}` (radius+margin escape toward outside group/tank, already-outside & unreachable-before-impact skip, hold-until-cast-ends + auto-resume interrupted activity), and `docs/specs/triggers.md`; added `dps_before`/`dps_during`/`dps_lost`/`time_back_in_range_ms` to `dodge_timeline.py`; added `tools/notes/extract_boss_notes.py` (`41` playerbots instances / `647` triggers → `content/notes/`), `A4` pre-pull standing-order compilation on instance entry + `(bot, map, notes)` cache + mid-fight order preservation in `bot_loop.{h,cc}`, `tools/eval/mine_traces.py` (`4/4` trace-mined boss eval cases passing `tools/wlb test eval`), and optional `quality` on `llmtest gear` (`gear_quality=2` breaking `16/16` ceilings on `loken`/`devourer`). Verified via `tools/wlb check` (`ALL CHECKS PASSED`: `60 + 121 + 226` C++ tests + `4 + 7 + 9 + 18 + 5` + `11` Python tests), `build.server(test=True)`, `tools/wlb test eval` (`4/4`), and `160`-trial live 5-boss benchmark ([`docs/benchmarks/2026-11-21-phase1-phase2-five-boss.md`](benchmarks/2026-11-21-phase1-phase2-five-boss.md)): `Kael'thas` `Flame Strike` gap closed (`llm` `7,460` / `6/8 <= 1k` vs `baseline` `11,229` / `5/8 <= 1k`), DPS within `10%` of `llm_no_notes` on `kaelthas` (`-9.6%`), `ikiss` (`+5.2%`), and `devourer` (`-7.1%`), `med_back_ms = 11–313 ms`; remaining kill-rate / `Murmur` `Resonance` / `Loken` `Pulsing Shockwave` gaps tracked honestly under `In progress`.

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
