# TODO: what is open

The one list of unfinished work. Detail lives in the document each item points to; this file says
what exists, what comes next and in which order. **Keep it current** (CONTRIBUTING.md, "Keep
docs/TODO.md current"): anything unfinished, deferred or decided-but-not-built goes here, not only
in a conversation.

Last updated 2026-11-22.

## In progress

**Open (awaiting user decision / external data)**
- [ ] **M3d — Lock `notes_hill_climb.py` and `bench_bosses.py` in `scorer.sha256`:** flags + SHA-256 hashes + git HEAD are now logged in `ledger.jsonl`, but adding both scripts to `hill_climb.py` lock manifest requires user-approved relock of `scorer.sha256` / `config/scorer-lock.sha256` (Rule #6).
- [ ] **M4 — Phase 1–3 sign-off decision (user's call):** all three runs (`v1` `23/40` vs `22/40`, `v2` `21/40` vs `19/40`, `v3` `52/80` vs `35/80`; combined `96/160` vs `76/160`, `p_raw = 0.0330`, `p_holm3 = 0.0990` n.s.) and the Kael'thas `-20.2%` DPS trade-off are now reported honestly in [`docs/benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md`](benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md); awaiting user decision on sign-off.
- [ ] **Unconfirmed — Raw-turn fix:** may read smooth human mouse turns as snaps; needs one human trace check to settle.

## Next, in order

**Priority direction (2026-11-21):** Mechanical WoW competence (dungeons, raids/ICC, clearing quests,
soloing elite quests, smart gearing) takes priority over `"human-like"` mimicry (`D`). **Phase 1–3
sign-off is pending the user's decision** (see M4 above and
[`docs/benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md`](benchmarks/2026-11-22-phase1-phase3-five-boss-n16.md):
v1 `23/40` vs `22/40`, v2 `21/40` vs `19/40`, v3 `52/80` vs `35/80`; combined `p_raw = 0.0330`, `p_holm3 = 0.0990` n.s.). **Phase 4
(`B1–B3`) is next once review items are closed and sign-off is decided.**

1. **Phase 4 — Mechanical open-world competence: questing, solo elite quests, smart gearing & zero stalls (`B1–B3`)**
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
- **Human trace GM-speed cleanup:** `2026-11-22.1.jsonl` in `~/.local/share/wow-llm-bots/human_baseline/human/Hele/` was cleaned of `> 10 yd/s` GM-fly/speed segments; strip `> 10 yd/s` segments from the still-open `2026-11-22.jsonl` after the play server stops.
- **Deliberately skipped benchmark speedup proposals (Items 4, 5, adaptive 6):** left unbuilt on
  purpose to preserve valid boss immunity/kite phases (Item 4: `20 s` zero-damage abort would
  cut `Gravity Lapse`/`Planar Shift` and censor `llm` bugs via survivorship bias), cross-boss DB
  isolation + startup-bound `LlmBrain.TraceDir` (`scorer.sha256` lock) (Item 5: cross-boss
  server reuse without `test_databases_reset()` saves only `~44 s/boss`), and unbiased fixed-`n`
  statistics (adaptive Item 6: stopping at `n=3` has a `42.2%` false-negative rate on a `75%` arm;
  details in [`docs/benchmarks/2026-11-20-four-boss-mixed-arms.md`](benchmarks/2026-11-20-four-boss-mixed-arms.md) §6).

## Done (newest first, keep about ten)

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

- 2026-11-22: **Review `a1b6dfe` fixes (Low 1–3):**
  added `ResolveHiddenTriggerByGuid` inside `ResolveThing(..., allow_hidden_triggers=true)` in [`act_executor.cc`](../server_module/src/options/act_executor.cc) so `"trigger:<guid>"` pinning works consistently for `FACE`/`FACE_AWAY` (`L1`); switched `nearby.areas` sort sentinel from `999.0f` to `std::numeric_limits<float>::infinity()` in [`screen_renderer.cc`](../harness/src/screen/screen_renderer.cc) and asserted `flame_pos < broken_pos` in [`screen_renderer_unittest.cc`](../harness/src/screen/screen_renderer_unittest.cc) (`L2`); clarified Kael'thas all-trial vs wipe-only median durations in `docs/TODO.md` (`L3`). Rebuilt `out/server-test` and passed `tools/wlb check` (`ALL CHECKS PASSED`; `ResolveHiddenTriggerByGuid` in `act_executor.cc` is compile/source-verified only).

- 2026-11-22: **Review `3b5cae8` fixes (High 1–2, Medium 1–3, Low 1–3):**
  - *`world_util.{h,cc}` (`H1`, `M2`, `L3`):* `SpellLocalizedName` returns `std::string_view` with explicit `nullptr`/empty checks on `SpellInfo::SpellName` (`char const*`); removed unused raw `const SpellInfo* spell` field from `TriggerHazardInfo`; `InspectTriggerHazard` ranks periodic-trigger / non-zero-radius auras above visual-only dummy auras.
  - *`act_executor.cc` (`H2`, `L1`, `L2`):* `ResolveThing` gates hidden-trigger lookup behind `allow_hidden_triggers = false` (enabled only in `ResolveMoveAnchor`, `ResolveHazard`, and `FACE`/`FACE_AWAY`, matching only player-visible `hz->name` — never `trigger->GetName()`, so `TARGET`/`MARK`/`USE`/`INTERACT`/`BOARD`/`PET_ATTACK` never hit hidden triggers); extracted `MakeTriggerHazardInfo(Creature*)` and `CollectHostileAreas()` so `StartReturnToInterrupted` scans the grid once instead of per candidate.
  - *`screen_renderer.cc`, tests & `TODO.md` (`M1`, `M3`, `L3`):* guarded `nearby.areas` `std::stable_sort` comparator with `std::isfinite(dist_yd)`; expanded `ScreenRendererTest.RendersNearbyAreasNearestFirst` in [`screen_renderer_unittest.cc`](../harness/src/screen/screen_renderer_unittest.cc) (nameless skip, `spell->name` fallback, friendly area, `NaN` `dist_yd`/`lands_in_s`, missing `rel`, 6-area cap); narrowed `3b5cae8` live-verification scope and added `n = 8` statistical caveats (`L3`). Rebuilt `out/server-test` and passed `tools/wlb check` (`ALL CHECKS PASSED`; C++ changes are compile/unit-test verified, not yet re-benchmarked live).
