# Five-Boss × Four-Arm Parallel Benchmark Report (`trace-all-events`)

**Date:** `2026-11-21` (branch `trace-all-events` at commit [`d7b3e13`](https://github.com/superliuxz/wow-llm-bots/commit/d7b3e13), `main` untouched at `2834756`)  
**Runner:** [`tools/bench/bench_bosses.py`](../../tools/bench/bench_bosses.py) (`--only devourer,loken,kaelthas,murmur,ikiss --mode mixed_all --runs 16 --parallel 4`)  
**Headline dataset:** **`320` valid scored trials** under **one identical commit (`d7b3e13`)** (`64` per boss, `16` per arm across all 5 bosses), with **`0` `no_pull` failures**, **`0` wave errors**, **`1` `pre_pull_combat` invalid trial** (`murmur` Wave 5 `no_brain`), and `3` extra overflow trials across `81` waves (`110.8 min` end-to-end).

**Main conclusion (with Holm–Bonferroni correction):**
- **Behaviour differences are strong and survive multiple-comparison correction (`m = 24` tests):** `notes.md` standing orders reliably make bots dodge AoE mechanics they are told about (`Murmur` `Sonic Boom` `12/16` vs `0/16`, `p_raw = 1.61e-05`, **`p_holm = 0.00039`**; `Ikiss` `Arcane Explosion` `10/16` vs `0/16`, `p_raw = 2.48e-04`, **`p_holm = 0.0047`**, and vs `1/16`, `p_raw = 0.0021`, **`p_holm = 0.033`**), while playerbots' hardcoded C++ script (`tbc-mgt`) still dodges ground-targeted `Kael'thas` `Flame Strike` much better (`14/16 <= 1,000` dmg vs `2–4/16`, `p_raw = 4.88e-05`, **`p_holm = 0.0010`**).
- **Kill-rate differences do not yet survive correction (`m = 30` tests, Bonferroni threshold `0.00167`):** `Devourer` and `Loken` hit `16/16` across every arm, `Ikiss` `llm` (`8/16`) vs `no_brain` (`1/16`) is **suggestive only** (`p_raw = 0.0155`, **`p_holm = 0.465`**; post-hoc pooled LLM `14/32` vs playerbots `3/32`, `p_raw = 0.0038 > 0.00167`), and same-arm kill counts swing `3–5` kills across `n = 16` reruns (`Ikiss` `no_brain`: `2, 6, 1 / 16`; `Kael'thas` `baseline`: `7, 12, 10 / 16`). Meanwhile, naive dodging carries a clear DPS cost (`Loken` `llm` `11.5k` vs `15.5k` `llm_no_notes`).

---

## 1. Experimental Setup, Single-Commit Provenance & Superseded Runs

Every boss ran in `mixed_all` mode across 4 simultaneous 5-man parties (`--parallel 4`) inside a single persistent test `worldserver` (`out/server-test`) with identical hardware and `llama-server` contention:

- **Boss selection (`Ick` replaced):** In the earlier `n = 8` cohort, **Ick** (`Pit of Saron`, L80) was a `100%` ceiling (`8/8` kills in all 4 arms, `p = 1.00`). We replaced `ick` with two WotLK mechanic bosses — **Devourer of Souls** (`The Forge of Souls`, `map=632`, `wotlk-fos`) and **Loken** (`Halls of Lightning`, `map=602`, `wotlk-hol`) — alongside **Kael'thas Sunstrider** (`Magisters' Terrace`, `map=585`, `tbc-mgt`), **Murmur** (`Shadow Labyrinth`, `map=555`), and **Talon King Ikiss** (`Sethekk Halls`, `map=556`, `tbc-seth`). Note that `Devourer` and `Loken` still finished `16/16` in all 4 arms for L80 epic parties.
- **Party composition (`COMP_5`):** Protection Warrior (`tank`, `spec_tab=2`), Holy Priest (`heal`, `spec_tab=1`), Frost Mage (`dps`, `spec_tab=2`), Destruction Warlock (`dps`, `spec_tab=2`), Marksmanship Hunter (`dps`, `spec_tab=1`), geared to boss level in `ITEM_QUALITY_EPIC` via `PlayerbotFactory::Randomize` + `InitTalentsBySpecNo`.
- **Four arms rotated every wave** via `_wave_arms` (four-cycle Latin rotation across 16+ waves so every arm ran in every party slot `Load..Pfou` evenly):
  - **`llm`**: `harness run` + `llama-server` (`gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf`, `--slots 16 --temperature 0`) with boss `notes.md` (standing orders + tactics); playerbots hardcoded boss strategy stripped via `llmtest setstrat ... -<strip_strat>`.
  - **`llm_no_notes`**: `harness run` + `llama-server` **without** `notes.md`; playerbots boss strategy stripped.
  - **`no_brain`**: pure playerbots (excluded from `--only-bots`), playerbots boss strategy stripped.
  - **`baseline`**: pure playerbots (excluded from `--only-bots`), playerbots hardcoded C++ boss strategy **ON** (`+<strip_strat>`).
- **Note on Murmur (`Shadow Labyrinth`, `map=555`):** `strip_strat=""` because `mod-playerbots` has no dedicated Murmur/Shadow Labyrinth strategy. On Murmur, `no_brain` and `baseline` act as two independent 16-trial control samples of unscripted playerbots AI.
- **Per-trial verification & wave isolation (`d7b3e13`):**
  - Between waves: teleported all 20 bots to `map=0`, verified `map=0`, re-verified party groups, ran `llmtest resetbot`, recorded `stage_ms`, staged leaders then followers at dungeon entrances, verified `alive=1` and four distinct never-reused `instance_id`s, verified `llmtest checkstrat`, connected `harness` (`~0.2 s`), moved all 4 parties to `spawn_xyz`, and verified zero pre-pull combat before `pull_ms`.
  - **Pre-first-plan pull handicap on LLM arms:** [`bench_bosses.py`](../../tools/bench/bench_bosses.py) teleports parties straight into boss `detection_range` (`20.0 yd`) as soon as `harness` connects (`~0.2 s`), so `llm` and `llm_no_notes` enter combat before their first boss-room LLM decision (and before initial `notes.md` standing-order compilation) finishes (`first_line_ms` p50 `239.5–555.8 ms`, p95 `947.6–1,241.9 ms`, plus completion tokens), whereas `baseline`'s C++ triggers are active from tick `0`.
  - **Valid/invalid breakdown in `d7b3e13`:**
    - `devourer`: `64` valid (`16`/arm), `0` invalid, `0` extra, `0` wave errors (`16` waves).
    - `loken`: `64` valid (`16`/arm), `0` invalid, `0` extra, `0` wave errors (`16` waves).
    - `kaelthas`: `64` valid (`16`/arm), `0` invalid, `0` extra, `0` wave errors (`16` waves).
    - `murmur`: `64` valid (`16`/arm), **`1` `pre_pull_combat` invalid trial** (`Wave 5` `party 3 (no_brain)`), **`3` extra overflow trials** (`Wave 17`), `0` wave errors (`17` waves). Note: `Wave 8` `baseline` ended as a `4`-death wipe at `74.2 s` (`Holy Priest` died in `Spirit of Redemption` at `65.4 s` and remaining combat dropped/ended while `no_brain`/`llm` waves continued up to `210.0 s` when `Pthrc` kited a `Cabal Executioner`).
    - `ikiss`: `64` valid (`16`/arm), `0` invalid, `0` extra, `0` wave errors (`16` waves).

### Transparent Disclosure of Superseded Runs
To keep provenance 100% intact, all earlier runs remain saved on disk in `var/` and are **not** mixed into Sections 2–5 below (except in Section 3 to illustrate cross-rerun variance across independent `n = 16` attempts):
1. **`n = 8` two-cohort baseline (`523ab4c` / `aac0546`, `var/bench_boss_10_mixed_all_8runs.json`):** `Kael'thas` (`llm` `6/8`, `llm_no_notes` `6/8`, `no_brain` `6/8`, `baseline` `7/8`), `Ick` (`8/8` everywhere), `Murmur` (`6/8`, `8/8`, `7/8`, `8/8`), `Ikiss` (`6/8`, `6/8`, `3/8`, `2/8`), plus earlier discarded `523ab4c` `Murmur`/`Ikiss` waves before multi-wave reset fixes.
2. **First `n = 16` attempt (`f553eba`, `var/bench_boss_10_mixed_all_f553eba.json`):** `devourer` (`13/16`, `15/16`, `16/16`, `14/16`, `2` `pre_pull_combat`, `6` extra), `kaelthas` (`3/16`, `4/16`, `3/16`, `7/16`), `murmur` (`6/16`, `13/16`, `10/16`, `13/16`, `2` `pre_pull_combat`, `2` extra), `ikiss` (`5/16`, `3/16`, `2/16`, `1/16`). **Superseded because `loken` hit `38` `no_pull` invalid trials in Waves 2–20** (`llm` `16/16`, `llm_no_notes` `16/16`, `no_brain` `1/1`, `baseline` `1/1`): `loken`'s initial `spawn_xyz=(1162.0, 33.83, 60.81)` was `24.47 yd` from Loken (`(1186.47, 33.8305, 60.8132)`), outside `creature_template.detection_range = 20.0`. Moving `loken` `spawn_xyz` inside `20.0 yd` (`1170.0`, `16.47 yd`) fixed this in [`e5297af`](https://github.com/superliuxz/wow-llm-bots/commit/e5297af).
3. **Second `n = 16` attempt (`e5297af`, `var/bench_boss_10_mixed_all_e5297af.json`):** `devourer` (`11/16`, `16/16`, `16/16`, `15/16`, `5` invalid, Wave 8 failed with `0 LLM decisions`), `loken` (`10/16`, `16/16`, `16/16`, `16/16`), `kaelthas` (`4/16`, `6/16`, `5/16`, `12/16`, `1` invalid), `murmur` (`7/16`, `15/16`, `15/16`, `12/16`, `2` invalid), `ikiss` (`7/16`, `5/16`, `6/16`, `3/16`). **Superseded because `_wait_fight` sliced from `party_pull_ms - 5000`**, reaching back into Wave `W-1` after `~1.1 s` resets and terminating slower `llm` fights mid-fight. Commit [`d7b3e13`](https://github.com/superliuxz/wow-llm-bots/commit/d7b3e13) fixed [`_slice_traces_after`](../../tools/bench/bench_bosses.py#L662-L677) to slice from `stage_ms` with `inst == party_instances[p_idx]`.

---

## 2. Headline Results: Behaviour & Mechanics (Holm–Bonferroni Corrected)

Computed across the `320` scored trials in `d7b3e13` (per-trial window `[pull_ms - 500, pull_ms + duration_s*1000 + 800]` with `sample.inst == trial.instance_id`) using [`dodge_timeline.py`](../../tools/bench/dodge_timeline.py).

**Multiple-comparison scope (`m = 24` behaviour tests):** There are `4` non-ceiling evasion/damage-threshold mechanics (`Loken` `Lightning Nova`, `Murmur` `Sonic Boom`, `Ikiss` `Arcane Explosion`, `Kael'thas` `Flame Strike <= 1,000` dmg) × `6` arm pairs = **`24` pairwise exact two-sided Fisher tests** (Bonferroni threshold `0.05 / 24 = 0.00208`). Even if all `6` pairs of `Devourer` `Well of Souls <= 5,000` dmg (`m = 30`, threshold `0.00167`) are included, every significant `Murmur`, `Ikiss`, and `Kael'thas` behaviour finding below remains **`p_holm < 0.05`**.

### AoE Dodge Timeline (`Loken`, `Murmur`, `Ikiss`)
**Independence note:** Multiple cast exposures inside one fight are correlated, so **independent trials (`n = 16`/arm) with `>= 1` safe-at-impact exposure** is the primary unit for Fisher tests; exposure counts and distances are descriptive context. Because `TraceSample` runs at `~5 Hz` (`200 ms`) while `act_sent` logs on the exact server tick, `act_delay_ms` relative to the first sampled `target_casting` row `s0` is slightly negative (`-71.0 ms` to `-6.0 ms`) when standing orders dispatch between samples.

| Boss & Spell (Radius) | Arm | Trials w/ Dodge (`cast_trials` / `16`) | Exposures Safe at Impact | Impact Dist (Med / p75 / Max) | Med `act_delay_ms` (`n`) | Impact Dmg Med (Mean) | Exact Fisher `p_raw` / `p_holm` (`m = 24`) |
|---|---|---:|---:|---|---|---:|---|
| **Murmur** — `Sonic Boom` (`34.0 yd` notes*) | `llm` | **12/16 (`12/16`)** | **19/87 (21.8%)** | **22.4** / 33.2 / 58.0 yd | -71.0 ms (`52`) | 7,095 (6,699), 18 deaths | Notes arm |
| | `llm_no_notes` | 0/16 (`0/16`) | 0/90 (0.0%) | 7.5 / 16.1 / 28.5 yd | — | 7,094 (6,039), 14 deaths | `p_raw = 1.61e-05` / **`p_holm = 0.00039`** vs `llm` |
| | `no_brain` | 0/16 (`0/16`) | 0/85 (0.0%) | 7.5 / 13.5 / 28.4 yd | — | 7,144 (7,208), 19 deaths | `p_raw = 1.61e-05` / **`p_holm = 0.00039`** vs `llm` |
| | `baseline` | 0/16 (`0/16`) | 0/83 (0.0%) | 7.5 / 11.6 / 32.1 yd | — | 7,345 (6,982), 18 deaths | `p_raw = 1.61e-05` / **`p_holm = 0.00039`** vs `llm` |
| **Ikiss** — `Arcane Explosion` (`35.0 yd` DBC) | `llm` | **10/16 (`10/16`)** | **27/179 (15.1%)** | 12.3 / 29.3 / 53.8 yd | -6.0 ms (`64`) | 0 (536), 6 deaths | Notes arm |
| | `no_brain` | 0/15 (`0/16`) | 0/123 (0.0%) | 6.5 / 8.9 / 22.0 yd | — | 0 (368), 6 deaths | `p_raw = 2.48e-04` / **`p_holm = 0.0047`** vs `llm` |
| | `baseline` | 0/16 (`0/16`) | 0/125 (0.0%) | 17.2 / 21.4 / 33.4 yd | — | 0 (1,031), 11 deaths | `p_raw = 2.48e-04` / **`p_holm = 0.0047`** vs `llm` |
| | `llm_no_notes` | 1/16 (`1/16`) | 1/158 (0.6%) | 7.2 / 8.8 / 38.1 yd | — | 0 (154), 2 deaths | `p_raw = 0.00205` / **`p_holm = 0.0329`** vs `llm` |
| **Loken** — `Lightning Nova` (`20.0 yd` DBC) | `baseline` | **16/16 (`16/16`)** | **139/150 (92.7%)** | **22.0** / 22.0 / 64.8 yd | — | **1,488 (1,417)** | `wotlk-hol` C++ script |
| | `llm` | 15/16 (`15/16`) | 72/155 (46.5%) | 18.5 / 24.5 / 52.1 yd | -54.0 ms (`131`) | 2,619 (4,367) | `p_raw = 1.00` / `p_holm = 1.00` vs `baseline` |
| | `llm_no_notes` | 13/16 (`13/16`) | 20/80 (25.0%) | 13.1 / 19.5 / 37.7 yd | — | 8,020 (6,467) | `p_raw = 0.226` / `p_holm = 1.00` vs `baseline` (`p_raw = 0.60`, `p_holm = 1.00` vs `llm`) |
| | `no_brain` | 10/16 (`10/16`) | 16/80 (20.0%) | 12.9 / 18.4 / 26.3 yd | — | 8,117 (6,895) | `p_raw = 0.0177` / `p_holm = 0.265` vs `baseline` (`p_raw = 0.083`, `p_holm = 1.00` vs `llm`) |

\*`Sonic Boom` emits `target_cast_radius = -1.0` in `TraceSample`; evaluated at `--radius 34` in `dodge_timeline.py`. Note on **Ikiss** median impact damage = `0`: bots remaining inside `35 yd` take `Arcane Explosion` damage cleanly on the tick after cast completion or only if not line-of-sighted behind pillars (`11` impact-window deaths across `125` exposures on `baseline` vs `6` across `179` exposures on `llm`).

### Ground Mechanics, C++ Script Comparison & DPS Cost of Dodging
- **Kael'thas `Flame Strike` (`baseline` `tbc-mgt` script dominates):** `Flame Strike` is cast by `Flame Strike Trigger (Kael - 5Man)`, so boss `target_casting` shows `0` windows. At the trial level (`n = 16`/arm):
  - **`baseline` (`tbc-mgt`) kept `Flame Strike` damage `<= 1,000` in `14/16` trials** (`13/16` at `0` dmg; median **`0.0`** `[0–7,359]`, total **`13,126`**), surviving Holm–Bonferroni cleanly against **`llm`** (`2/16`, `1/16` at `0`, med **`4,777.0`**, tot **`128,571`**, `p_raw = 4.88e-05`, **`p_holm = 0.00102`**), **`llm_no_notes`** (`2/16`, `0/16` at `0`, med **`8,197.0`**, tot **`168,394`**, `p_raw = 4.88e-05`, **`p_holm = 0.00102`**), and **`no_brain`** (`4/16`, `3/16` at `0`, med **`7,511.5`**, tot **`139,233`**, `p_raw = 9.65e-04`, **`p_holm = 0.0164`**). Adding `AVOID Flame Strike` (`64` acts) in `llm` did not stop trigger damage.
  - **`Arcane Sphere` deaths & `Phoenix Egg` kills:** `baseline` suffered **`6`** `Arcane Sphere` deaths vs **`27`** (`llm`), **`28`** (`llm_no_notes`), and **`43`** (`no_brain`), while destroying `23` deduped `Phoenix Egg` spawns (`101` bot `mob_kill` credits) vs `17` (`72`), `19` (`90`), and `19` (`86`). Nearby trash packs affected `12–16/16` trials across all arms.
- **Loken `Pulsing Shockwave` & DPS cost of naive `AWAY` runouts:** On `Loken`, `llm`'s `134` `AWAY Loken` standing orders dropped median party DPS by **`25.6%`** (**`11,537.7`** `[6,930.4–13,644.6]` / `34.2 s` vs **`15,508.0`** `[13,345.1–16,974.7]` / `25.4 s` in `llm_no_notes` and **`15,886.1`** / `24.9 s` in `no_brain`). Because `Loken`'s `Pulsing Shockwave` scales linearly with distance (`spell_loken_pulsing_shockwave`), `25+ yd` runouts also **increased non-tank boss damage by `+83%` vs `no_brain` (`1,739,938` vs `951,402`) and more than doubled `baseline` (`776,553`)** and doubled healing required (`3,175.0` vs `1,411.8` `baseline`), whereas `baseline` (`wotlk-hol`) stacked close cleanly and stepped right outside `20 yd` (`22.0 yd` median, `139/150` safe exposures).
- **Devourer of Souls (`Well of Souls`, `Mirrored Soul`):** `AVOID Well of Souls` rarely fired (**`4`** acts across 16 trials), and `llm` took more `Well of Souls` damage (`5/16 <= 5,000`, med **`13,311.0`**, tot **`387,294`**) than `llm_no_notes` (`9/16 <= 5,000`, med **`2,995.0`**, tot **`148,905`**; pairwise `p_raw = 0.289`, `p_holm = 1.00`), `no_brain` (`6/16`, med **`7,413.5`**), or `baseline` (`7/16`, med **`5,096.5`**). Meanwhile, `307` `Mirrored Soul` `STOP` acts reduced `llm` median DPS (`13,045.2` / `47.9 s` vs `15,625.2` / `40.4 s` `llm_no_notes` and `15,856.8` / `39.5 s` `no_brain`) without lowering non-tank boss damage (`42,214.0` med / `649,080` tot vs `37,928.5` / `589,554` `llm_no_notes` and `31,068.5` / `435,974` `no_brain`), and `llm` clipped entrance `Spectral Warden` trash in `2/16` trials (`10,135` dmg vs `0/16` elsewhere).
- **Murmur room-trash cost:** `llm`'s `AWAY` (`88` acts) + `STOP` (`54` acts) dropped median DPS (`2,280.5` / `70.8 s` vs `3,080.1` / `53.5 s` `no_brain`) and pulled room trash (`253,676` non-Murmur mob dmg + `8` mob deaths + `1` env death in `llm` vs `90,214` and `0` in `no_brain`).

### Standing-Order Actions (`act_sent`) in Scored Trials
- **`no_brain` & `baseline` (all 5 bosses):** `0` `act_sent` events.
- **`llm_no_notes`:** `0` `act_sent` events on `devourer`, `kaelthas`, `murmur`, and `ikiss`; `2` LLM-issued `act_sent` events (`AWAY` `1`, `TO` `1`) on `loken`.
- **`llm`:**
  - `devourer` (`827` acts): `STOP` (`307`), `TARGET Devourer of Souls` (`276`), `TO Devourer of Souls` (`149`), `TARGET` (`34`), `PET_ATTACK Devourer of Souls` (`22`), `USE Devourer of Souls [err: unknown spell or item: Devourer of Souls]` (`15`), `USE Phantom Blast [err: ...]` (`5`), `AVOID Well of Souls` (`4`), `FACE Devourer of Souls` (`2`), `PET_ATTACK` (`2`), `USE Mirrored Soul [err: ...]` (`2`), `TO` (`2`).
  - `loken` (`318` acts): `AWAY Loken` (`134`), `TO Loken` (`115`), `AVOID Pulsing Shockwave` (`24`), `TARGET Loken` (`15`), `AVOID Lightning Nova` (`13`), `STOP` (`8`), `PET_ATTACK Loken` (`5`), `FACE Loken` (`4`).
  - `kaelthas` (`242` acts): `TARGET Phoenix Egg` (`83`), `AVOID Flame Strike` (`64`), `TARGET Kael'thas Sunstrider` (`42`), `TARGET Phoenix` (`25`), `TO` (`8`), `TARGET Coilskar Witch` (`5`), `TARGET Phoenix Egg [err: you are dead]` (`4`), `TARGET Sunblade Warlock` (`2`), `TO Phoenix Egg` (`1`), `STOP` (`1`), `TARGET Arcane Sphere [err: ...]` (`1`), `TARGET Ethereum Smuggler` (`1`).
  - `murmur` (`172` acts): `AWAY Murmur` (`55`), `STOP` (`54`), `AWAY` (`33`), `TO` (`14`), `TO Murmur` (`7`), `TARGET Murmur` (`4`), `TARGET Cabal Spellbinder` (`1`), `PET_ATTACK Murmur` (`1`), `AWAY Murmur [err: you are dead]` (`1`), `TO Ptwoa` (`1`), `TO Pthrb` (`1`).
  - `ikiss` (`147` acts): `AWAY Talon King Ikiss` (`94`), `TO` (`18`), `STOP` (`11`), `TARGET Talon King Ikiss` (`9`), `TARGET` (`5`), `TO Talon King Ikiss` (`4`), `USE Polymorph [err: cannot cast Polymorph now]` (`2`), `FACE Talon King Ikiss` (`1`), `AWAY` (`1`), `USE Pfoua [err: ...]` (`1`), `TARGET Polymorph` (`1`).

---

## 3. Kill Rates, Multiple-Comparison Correction & Cross-Rerun Variance

All results below are from **single commit `d7b3e13` (`n = 16` valid trials per arm per boss)**. Confidence intervals are Wilson `95%` intervals; **`p_raw`** is the exact two-sided Fisher `p`-value and **`p_holm`** applies step-down Holm–Bonferroni across all **`30` pairwise kill tests (`5` bosses × `6` arm pairs, Bonferroni threshold `0.05 / 30 = 0.00167`)**.

| Boss (`map`, Lvl, `strip_strat`) | `llm` | `llm_no_notes` | `no_brain` | `baseline` | Exact Fisher `p_raw` (`p_holm`, `m = 30`) |
|---|---:|---:|---:|---:|---|
| **Devourer of Souls** (`632`, L80, `wotlk-fos`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | `p_raw = 1.00` (`p_holm = 1.00`) everywhere (WotLK L80 epic ceiling) |
| **Loken** (`602`, L80, `wotlk-hol`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | `p_raw = 1.00` (`p_holm = 1.00`) everywhere (WotLK L80 epic ceiling) |
| **Kael'thas Sunstrider** (`585`, L70, `tbc-mgt`) | 8/16 (50.0%, `[28.0–72.0%]`) | **10/16 (62.5%, `[38.6–81.5%]`)** | 5/16 (31.2%, `[14.2–55.6%]`) | **10/16 (62.5%, `[38.6–81.5%]`)** | `baseline`/`llm_no_notes` vs `no_brain` `p_raw = 0.156` (`p_holm = 1.00`); `llm` vs `no_brain` `p_raw = 0.473` (`p_holm = 1.00`); `llm` vs `baseline`/`llm_no_notes` `p_raw = 0.722` (`p_holm = 1.00`); pooled LLM `18/32` vs PB `15/32` `p_raw = 0.617` |
| **Murmur** (`555`, L70, *(none)*) | 10/16 (62.5%, `[38.6–81.5%]`) | 12/16 (75.0%, `[50.5–89.8%]`) | **13/16 (81.2%, `[57.0–93.4%]`)** | 12/16 (75.0%, `[50.5–89.8%]`) | `llm` vs `no_brain` `p_raw = 0.433` (`p_holm = 1.00`); `llm` vs `llm_no_notes`/`baseline` `p_raw = 0.704` (`p_holm = 1.00`); pooled LLM `22/32` vs PB `25/32` `p_raw = 0.572` |
| **Talon King Ikiss** (`556`, L68, `tbc-seth`) | **8/16 (50.0%, `[28.0–72.0%]`)** | 6/16 (37.5%, `[18.5–61.4%]`) | 1/16 (6.2%, `[1.1–28.3%]`) | 2/16 (12.5%, `[3.5–36.0%]`) | **`llm` vs `no_brain` `p_raw = 0.0155` (`p_holm = 0.465`, suggestive only)**; `llm` vs `baseline` `p_raw = 0.0538` (`p_holm = 1.00`); `llm_no_notes` vs `no_brain` `p_raw = 0.0829` (`p_holm = 1.00`); post-hoc pooled LLM `14/32` vs PB `3/32` `p_raw = 0.0038` (`> 0.00167`) |
| **5-Boss Aggregate (`d7b3e13`, 80/arm)** | 58/80 (72.5%, `[61.9–81.1%]`) | **60/80 (75.0%, `[64.5–83.2%]`)** | 51/80 (63.8%, `[52.8–73.4%]`) | 56/80 (70.0%, `[59.2–78.9%]`) | `llm` vs `no_brain` `p_raw = 0.309`; `llm` vs `llm_no_notes` `p_raw = 0.858`; `llm` vs `baseline` `p_raw = 0.861`; pooled LLM `118/160` vs PB `107/160` `p_raw = 0.221` |

### Why Kill-Rate Evidence Remains Suggestive Only
1. **No pairwise kill test survives multiple-comparison correction:** Across `30` pairwise kill comparisons (`5` bosses × `6` pairs), the Bonferroni cutoff is `0.00167`. The smallest raw `p`-value (`Ikiss` `llm` `8/16` vs `no_brain` `1/16`, `p_raw = 0.0155`) adjusts to **`p_holm = 0.465`**. Even post-hoc pooling (`Ikiss` LLM `14/32` vs playerbots `3/32`, `p_raw = 0.0038`) sits above `0.00167` (and was not pre-registered before the run). On clean Ikiss pulls (`0/16` trash pulls on `no_brain` and `llm_no_notes`; `llm` `6/9` clean vs `baseline` `0/1` clean / `15/16` trash), the direction favours LLM arms, so **Ikiss kill results are suggestive rather than statistically decisive**.
2. **Single-run differences of `3–5` kills are within rerun noise:** Across our three independent `n = 16` attempts (`f553eba`, `e5297af`, `d7b3e13`) where non-LLM arms on `Ikiss` and `Kael'thas` suffered no `no_pull` or LLM slice bugs:
   - **`Ikiss` `no_brain`:** **`2/16` (`f553eba`), `6/16` (`e5297af`), `1/16` (`d7b3e13`)** (`baseline`: `1/16`, `3/16`, `2/16`).
   - **`Kael'thas` `baseline`:** **`7/16` (`f553eba`), `12/16` (`e5297af`), `10/16` (`d7b3e13`)** (`no_brain`: `3/16`, `5/16`, `5/16`).
   Future runs should pre-register `1–2` primary hypotheses before launching rather than testing all `30` pairs post hoc.
3. **2 of 5 bosses cannot separate arms on kill rate:** Both WotLK bosses (`Devourer` and `Loken`) finished **`16/16` in every arm**, giving `0` kill-rate signal for L80 epic parties.

---

## 4. Detailed Per-Boss Throughput & Latency Tables (`16` Valid Trials per Arm under `d7b3e13`)

### 4A. Devourer of Souls (The Forge of Souls, L80, `strip_strat = wotlk-fos`)
`64/64` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 16/16 (100.0%) | 13,045.2 `[9,126.9–14,785.5]` | 1,677.5 `[837.0–3,278.0]` | 0.0 `[0.0–2.0]` | 47.9 s `[42.1–65.9]` | 287.4 ms / 985.6 ms (1,250.0) | 309 |
| `llm_no_notes` | 16/16 (100.0%) | 15,625.2 `[11,970.5–17,790.1]` | 1,717.8 `[849.9–3,079.7]` | 0.0 `[0.0–2.0]` | 40.4 s `[35.7–51.2]` | 383.1 ms / 972.0 ms (1,222.6) | 223 |
| `no_brain` | 16/16 (100.0%) | **15,856.8** `[11,576.2–17,310.8]` | **1,877.4** `[935.3–3,628.1]` | 0.0 `[0.0–2.0]` | **39.5 s** `[35.8–54.0]` | — | 0 |
| `baseline` | 16/16 (100.0%) | 13,191.9 `[11,999.1–14,829.9]` | 1,397.4 `[1,018.5–2,832.8]` | **0.0** `[0.0–0.0]` | 47.2 s `[42.8–52.6]` | — | 0 |

### 4B. Loken (Halls of Lightning, L80, `strip_strat = wotlk-hol`)
`64/64` valid trials (`0` invalid, `0` wave errors, `0` deaths across all `64` trials).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 16/16 (100.0%) | 11,537.7 `[6,930.4–13,644.6]` | **3,175.0** `[2,727.4–3,727.9]` | 0.0 `[0.0–0.0]` | 34.2 s `[28.9–56.5]` | 555.8 ms / 947.6 ms (1,259.1) | 163 |
| `llm_no_notes` | 16/16 (100.0%) | 15,508.0 `[13,345.1–16,974.7]` | 2,962.1 `[2,511.0–3,688.9]` | 0.0 `[0.0–0.0]` | 25.4 s `[23.2–29.3]` | 388.1 ms / 951.5 ms (1,232.3) | 210 |
| `no_brain` | 16/16 (100.0%) | **15,886.1** `[14,756.8–18,363.0]` | 2,921.8 `[2,351.3–3,318.0]` | 0.0 `[0.0–0.0]` | **24.9 s** `[21.3–26.5]` | — | 0 |
| `baseline` | 16/16 (100.0%) | 11,505.8 `[9,030.4–14,135.0]` | 1,411.8 `[1,057.7–3,085.1]` | 0.0 `[0.0–0.0]` | 34.4 s `[27.9–43.4]` | — | 0 |

### 4C. Kael'thas Sunstrider (Magisters' Terrace, L70, `strip_strat = tbc-mgt`)
`64/64` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 8/16 (50.0%) | 2,018.7 `[535.7–2,340.4]` | 1,398.1 `[647.4–2,056.9]` | 5.0 `[0.0–5.0]` | **78.2 s** `[48.2–116.6]` | 352.1 ms / 1,197.0 ms (1,305.2) | 987 |
| `llm_no_notes` | **10/16 (62.5%)** | **2,117.8** `[867.4–2,838.4]` | **1,586.5** `[847.6–2,128.6]` | 2.5 `[0.0–5.0]` | 81.6 s `[59.1–114.7]` | 331.0 ms / 1,220.1 ms (1,321.3) | 883 |
| `no_brain` | 5/16 (31.2%) | 1,971.7 `[679.7–3,342.1]` | 1,223.2 `[625.0–2,029.5]` | 5.0 `[2.0–5.0]` | 80.7 s `[44.3–117.6]` | — | 0 |
| `baseline` | **10/16 (62.5%)** | 1,238.8 `[485.9–2,124.1]` | 791.4 `[225.2–1,358.7]` | **2.0** `[0.0–6.0]` | 116.7 s `[53.2–197.7]` | — | 0 |

### 4D. Murmur (Shadow Labyrinth, L70, `strip_strat = ""`)
`64/64` valid trials (`1` `pre_pull_combat` invalid trial on Wave 5 `no_brain`, `3` extra overflow trials in Wave 17, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 10/16 (62.5%) | 2,280.5 `[1,079.0–3,381.3]` | 842.0 `[229.4–1,657.5]` | 3.0 `[0.0–5.0]` | 70.8 s `[50.2–157.9]` | 358.4 ms / 1,241.9 ms (1,610.2) | 876 |
| `llm_no_notes` | 12/16 (75.0%) | 2,405.5 `[816.5–3,429.4]` | 658.4 `[309.6–2,002.2]` | 2.0 `[0.0–5.0]` | 70.2 s `[50.2–210.0]` | 440.2 ms / 1,226.3 ms (1,635.1) | 764 |
| `no_brain` | **13/16 (81.2%)** | 3,080.1 `[1,802.0–4,694.4]` | **1,038.1** `[240.2–1,904.6]` | 2.0 `[0.0–5.0]` | **53.5 s** `[33.3–93.7]` | — | 0 |
| `baseline` | 12/16 (75.0%) | **3,119.8** `[2,032.9–4,258.5]` | 885.5 `[244.9–2,041.9]` | **1.5** `[0.0–5.0]` | 54.6 s `[33.0–79.9]` | — | 0 |

### 4E. Talon King Ikiss (Sethekk Halls, L68, `strip_strat = tbc-seth`)
`64/64` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | **8/16 (50.0%)** | **981.5** `[635.0–1,308.7]` | **599.4** `[335.2–1,078.0]` | **4.5** `[2.0–6.0]` | 75.1 s `[40.3–111.5]` | 239.5 ms / 988.4 ms (1,247.7) | 581 |
| `llm_no_notes` | 6/16 (37.5%) | 971.8 `[652.1–1,404.0]` | 588.4 `[398.8–1,278.7]` | 5.0 `[3.0–6.0]` | 73.3 s `[52.9–87.3]` | 335.1 ms / 1,067.3 ms (1,228.9) | 458 |
| `no_brain` | 1/16 (6.2%) | 908.0 `[660.6–1,484.9]` | 578.5 `[294.2–1,210.4]` | 5.0 `[3.0–6.0]` | **62.2 s** `[52.4–84.5]` | — | 0 |
| `baseline` | 2/16 (12.5%) | 647.5 `[454.5–954.7]` | 514.2 `[244.1–914.2]` | 5.0 `[2.0–5.0]` | 67.1 s `[39.5–148.5]` | — | 0 |

---

## 5. Key Takeaways & Where This Leaves the Project

1. **Notes reliably change behaviour (`p_holm = 0.00039` to `0.033`):** Bots execute `notes.md` standing orders and dodge cast-bar AoEs they are told about (`Murmur` `Sonic Boom` `12/16` vs `0/16` everywhere, `p_raw = 1.61e-05`, `p_holm = 0.00039`; `Ikiss` `Arcane Explosion` `10/16` vs `0/16` playerbots `p_holm = 0.0047` & `1/16` `llm_no_notes` `p_holm = 0.0329`; `Loken` `Lightning Nova` `15/16` with median impact dmg cut from `8,020–8,117` to `2,619`).
2. **Notes do not yet reliably change kill rates:** Across all `30` pairwise kill comparisons (`5` bosses × `6` pairs), nothing survives Holm–Bonferroni correction (threshold `0.00167`). Only `Ikiss` hints at a kill-rate gain (`llm` `8/16` vs `no_brain` `1/16`, `p_raw = 0.0155`, **`p_holm = 0.465`**, **suggestive** only; pooled LLM `14/32` vs PB `3/32`, `p_raw = 0.0038 > 0.00167`). Same-arm rerun variance is `3–5` kills (`Ikiss` `no_brain`: `2, 6, 1 / 16`; `Kael` `baseline`: `7, 12, 10 / 16`), and `Devourer` + `Loken` were `16/16` across all 4 arms.
3. **Dodging costs DPS (and can backfire on secondary mechanics):** On `Loken`, `llm` did **`11.5k` (`11,537.7`) vs `15.5k` (`15,508.0`)** median DPS for `llm_no_notes` (while `25+ yd` runouts doubled `Pulsing Shockwave` non-tank damage); on `Devourer`, `Mirrored Soul` `STOP` orders dropped DPS from `15.6k` to `13.0k`; on `Murmur`, `AWAY` + `STOP` dropped DPS from `3.08k` (`no_brain`) to `2.28k` and pulled room trash.
4. **The playerbots C++ scripts are still better at the mechanics they cover:** `Kael'thas` `Flame Strike` (`14/16 <= 1,000` dmg in `baseline` vs `2/16` `llm`/`llm_no_notes` `p_holm = 0.0010` and `4/16` `no_brain` `p_holm = 0.016`) and `Loken` (`139/150` safe exposures at exact `22.0 yd` step-out vs `72/155` `llm`).

**Where to focus next:** The next gains are in **`notes.md` and the acts** (dodge only when it is worth it, step right outside dangerous radii or time the exit cleanly, then return promptly to DPS), **not** in further benchmark runner engineering; also drop `Devourer` and `Loken` — or lower their fixture gear — so every boss in future runs can separate arms on kill rate (with `1–2` pre-registered primary comparisons before running).

---

## 6. Runner Speedup Telemetry, Per-Wave Slice Fix & Skipped Proposals

### Measured Wall-Clock Breakdown on `d7b3e13` (`320` Valid Trials, `81` Waves)
Measured directly from `phase_timings` in [`var/bench_boss_10_mixed_all.json`](../../var/bench_boss_10_mixed_all.json):

| Boss | Waves | `setup_s` | Med Wave Overhead | Med `fight_s` | `teardown_s` | `wall_s` | `sum(max_all_4)` | `sum(max_llm_2)` | Control Delta |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `devourer` | 16 | 27.67 s | 1.08 s | 50.09 s | 1.10 s | 861.2 s (`14.4 m`) | 805.4 s | 788.0 s | +17.4 s |
| `loken` | 16 | 27.57 s | 1.06 s | 36.68 s | 1.10 s | 661.6 s (`11.0 m`) | 599.2 s | 572.5 s | +26.7 s |
| `kaelthas` | 16 | 27.59 s | 1.21 s | 124.28 s | 2.10 s | 2,119.5 s (`35.3 m`) | 2,051.7 s | 1,423.5 s | +628.2 s |
| `murmur` | 17 | 43.46 s | 0.90 s | 74.08 s | 2.00 s | 1,462.8 s (`24.4 m`) | 1,386.8 s | 1,355.6 s | +31.2 s |
| `ikiss` | 16 | 43.46 s | 1.09 s | 84.65 s | 2.10 s | 1,541.0 s (`25.7 m`) | 1,460.9 s | 1,290.2 s | +170.7 s |
| **Total** | **81** | **169.75 s** | **~1.08 s** | — | **8.40 s** | **6,646.1 s (`110.8 m`)** | **6,304.0 s** | **5,429.8 s** | **+874.2 s (`+13.9%`)** |

- **Inter-wave overhead (`reset + stage + strat + connect + score`):** held steady at **`0.90–1.21 s`** across all `81` waves (~`44×` faster than `47.6 s` prior to Item 3).
- **Control-arm wall-clock impact (`no_brain` & `baseline`):** uses `0` `llama-server` slots and added **`+14.6 min` (`874.2 s`, `+13.9%`)** over `llm`-only wave durations across `81` waves — dominated by `Kael'thas` (`+628.2 s` from long `116.7 s` `baseline` runs), while adding only **`+4.1 min` (`246.0 s`)** total across the other 4 bosses (`65` waves).
- **Multi-wave runner correctness fixes landed (`e5297af` & `d7b3e13`):**
  - Moving `loken` `spawn_xyz` inside `20.0 yd` `detection_range` (`16.47 yd`) eliminated multi-wave `no_pull` stalls on non-LLM arms.
  - Slicing [`_slice_traces_after`](../../tools/bench/bench_bosses.py#L662-L677) from `stage_ms` with `inst == party_instances[p_idx]` eliminated cross-wave `mob_kill` bleed caused by fast (`~1.1 s`) wave transitions under the old `-5000 ms` lookback.

### Deliberately Skipped Proposals (Items 4, 5, and Adaptive Item 6)
- **Item 4 (abort fight after `20 s` of zero boss damage and mark `invalid`):** Skipped because real mechanics produce `>= 20 s` of zero boss damage (`Kael'thas` `Gravity Lapse` `30 s`, `Ick` `Explosive Barrage`/`Pursuit`, `Eregos` `Planar Shift` `18 s`), and aborting real LLM stalls (`STOP`, bad switches) as `invalid` introduces survivorship bias.
- **Item 5 (reuse one `worldserver` across all bosses without DB reset):** Skipped because per-boss setup is only `27.6–43.5 s` (`2.8 min` total across 5 bosses, `2.6%` of wall time), while resetting guarantees zero cross-boss leak of `attune_quests`, `role_items`, spells, and `LlmBrain.TraceDir`.
- **Adaptive Item 6 (drop `--runs` early after `3` matching outcomes):** Skipped because early stopping at `n = 3` on a true `75%` arm has a `42.2%` false-stop rate (`0.75^3`) and breaks uniform sample counts across bosses.

---

## 7. Saved Artifacts
- Headline `d7b3e13` summary JSON (`320` trials): `var/bench_boss_10_mixed_all.json` (untracked)
- Unified 5 Hz + event traces (`d7b3e13`):
  - `var/bench_boss_devourer_coord_mixed_all/bot/`
  - `var/bench_boss_loken_nova_mixed_all/bot/`
  - `var/bench_boss_kaelthas_mgt_mixed_all/bot/`
  - `var/bench_boss_murmur_boom_mixed_all/bot/`
  - `var/bench_boss_ikiss_blink_mixed_all/bot/`
- Superseded provenance JSON backups (untracked):
  - `var/bench_boss_10_mixed_all_e5297af.json`
  - `var/bench_boss_10_mixed_all_f553eba.json`
  - `var/bench_boss_10_mixed_all_8runs.json`
