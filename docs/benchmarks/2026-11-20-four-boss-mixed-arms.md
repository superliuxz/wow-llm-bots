# Five-Boss × Four-Arm Parallel Benchmark Report (`trace-all-events`)

**Date:** `2026-11-21` (branch `trace-all-events` at commit [`d7b3e13`](https://github.com/superliuxz/wow-llm-bots/commit/d7b3e13), `main` untouched at `2834756`)  
**Runner:** [`tools/bench/bench_bosses.py`](../../tools/bench/bench_bosses.py) (`--only devourer,loken,kaelthas,murmur,ikiss --mode mixed_all --runs 16 --parallel 4`)  
**Headline dataset:** **`320` valid scored trials** under **one identical commit (`d7b3e13`)** (`64` per boss, `16` per arm across all 5 bosses), with **`0` `no_pull` failures**, **`0` wave errors**, and only **`1` `pre_pull_combat` invalid trial** (`murmur` Wave 5 `no_brain`) plus `3` extra overflow trials across `81` total waves (`110.8 min` end-to-end).

---

## 1. Experimental Setup, Single-Commit Provenance & Superseded Runs

Every boss ran in `mixed_all` mode across 4 simultaneous 5-man parties (`--parallel 4`) inside a single persistent test `worldserver` (`out/server-test`) with identical hardware and `llama-server` contention:

- **Boss selection (`Ick` replaced):** In the earlier `n = 8` cohort, **Ick** (`Pit of Saron`, L80) was a `100%` ceiling (`8/8` kills in all 4 arms, `p = 1.00`). We replaced `ick` with two WotLK mechanic bosses — **Devourer of Souls** (`The Forge of Souls`, `map=632`, `wotlk-fos`) and **Loken** (`Halls of Lightning`, `map=602`, `wotlk-hol`) — alongside **Kael'thas Sunstrider** (`Magisters' Terrace`, `map=585`, `tbc-mgt`), **Murmur** (`Shadow Labyrinth`, `map=555`), and **Talon King Ikiss** (`Sethekk Halls`, `map=556`, `tbc-seth`).
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
To keep provenance 100% intact, all earlier runs remain saved on disk in `var/` and are **not** mixed into Sections 2–5 below:
1. **`n = 8` two-cohort baseline (`523ab4c` / `aac0546`, `var/bench_boss_10_mixed_all_8runs.json`):** `Kael'thas` (`llm` `6/8`, `llm_no_notes` `6/8`, `no_brain` `6/8`, `baseline` `7/8`), `Ick` (`8/8` everywhere), `Murmur` (`6/8`, `8/8`, `7/8`, `8/8`), `Ikiss` (`6/8`, `6/8`, `3/8`, `2/8`), plus earlier discarded `523ab4c` `Murmur`/`Ikiss` waves before multi-wave reset fixes.
2. **First `n = 16` attempt (`f553eba`, `var/bench_boss_10_mixed_all_f553eba.json`):** `devourer` (`13/16`, `15/16`, `16/16`, `14/16`, `2` `pre_pull_combat`, `6` extra), `kaelthas` (`3/16`, `4/16`, `3/16`, `7/16`), `murmur` (`6/16`, `13/16`, `10/16`, `13/16`, `2` `pre_pull_combat`, `2` extra), `ikiss` (`5/16`, `3/16`, `2/16`, `1/16`). **Superseded because `loken` hit `38` `no_pull` invalid trials in Waves 2–20** (`llm` `16/16`, `llm_no_notes` `16/16`, `no_brain` `1/1`, `baseline` `1/1`): `loken`'s initial `spawn_xyz=(1162.0, 33.83, 60.81)` was `24.47 yd` from Loken (`(1186.47, 33.8305, 60.8132)`), outside `creature_template.detection_range = 20.0`. In Wave 1 only, newly created Warlocks cast `Summon Imp`, pulling Loken; in Waves 2–20 `resetbot` preserved the Imp, so non-LLM arms with `-follow,-grind,-new rpg` never aggroed. Moving `loken` `spawn_xyz` inside `20.0 yd` (`1170.0`, `16.47 yd`) fixed this in [`e5297af`](https://github.com/superliuxz/wow-llm-bots/commit/e5297af) (`combat_start = 12–27 ms` across Wave 1 & Wave 2).
3. **Second `n = 16` attempt (`e5297af`, `var/bench_boss_10_mixed_all_e5297af.json`):** `devourer` (`11/16`, `16/16`, `16/16`, `15/16`, `5` invalid, Wave 8 failed with `0 LLM decisions`), `loken` (`10/16`, `16/16`, `16/16`, `16/16`), `kaelthas` (`4/16`, `6/16`, `5/16`, `12/16`, `1` invalid), `murmur` (`7/16`, `15/16`, `15/16`, `12/16`, `2` invalid), `ikiss` (`7/16`, `5/16`, `6/16`, `3/16`). **Superseded because trace auditing uncovered a cross-wave slice bug in `_wait_fight`:** line `1065` sliced traces from `party_pull_ms - 5000`. Once Item 3 cut inter-wave setup from `47.6 s` down to `~1.1 s`, `-5000 ms` reached back into Wave `W-1` and tripped false `KILLED in 5.9–7.3 s!` exits (`> 60` times across all 5 bosses) whenever the slower `llm` party had finished Wave `W-1` shortly before Wave `W` pulled — terminating `harness` mid-fight while `llm` was still alive at `0` deaths (`6/6` `loken` and `5/5` `devourer` `llm` "wipes"). Commit [`d7b3e13`](https://github.com/superliuxz/wow-llm-bots/commit/d7b3e13) fixed `_slice_traces_after` in [`bench_bosses.py`](../../tools/bench/bench_bosses.py) to slice strictly from `stage_ms` and filter samples by `inst == party_instances[p_idx]`, verified by [`test_slice_traces_after_excludes_prior_wave_kill_across_fast_reset`](../../tools/bench/dodge_timeline_test.py#L575-L589) (`0` short-kill bleeds in a 3-wave live smoke test and `0` in the `320`-trial run).

---

## 2. Executive Summary (Kills / 16 Valid Trials, Wilson 95% CIs & Fisher Tests)

All results below are from **single commit `d7b3e13` (`n = 16` valid trials per arm per boss)**. Confidence intervals are Wilson `95%` intervals; `p`-values are exact two-sided Fisher tests. The 5-boss aggregate row sums identical balanced strata (`16` trials per boss × `5` bosses = `80` per arm); note that per-boss heterogeneity makes the per-boss rows primary.

| Boss (`map`, Lvl, `strip_strat`) | `llm` | `llm_no_notes` | `no_brain` | `baseline` | Exact Fisher `p` |
|---|---:|---:|---:|---:|---|
| **Devourer of Souls** (`632`, L80, `wotlk-fos`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | `p = 1.00` everywhere (WotLK ceiling at L80 epic) |
| **Loken** (`602`, L80, `wotlk-hol`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | 16/16 (100.0%, `[80.6–100.0%]`) | `p = 1.00` everywhere (WotLK ceiling; strong AoE separation in §4) |
| **Kael'thas Sunstrider** (`585`, L70, `tbc-mgt`) | 8/16 (50.0%, `[28.0–72.0%]`) | **10/16 (62.5%, `[38.6–81.5%]`)** | 5/16 (31.2%, `[14.2–55.6%]`) | **10/16 (62.5%, `[38.6–81.5%]`)** | `llm` vs `llm_no_notes`/`baseline` `p = 0.72`, vs `no_brain` `p = 0.47`; `baseline`/`llm_no_notes` vs `no_brain` `p = 0.16`; pooled LLM `18/32` vs PB `15/32` `p = 0.62` |
| **Murmur** (`555`, L70, *(none)*) | 10/16 (62.5%, `[38.6–81.5%]`) | 12/16 (75.0%, `[50.5–89.8%]`) | **13/16 (81.2%, `[57.0–93.4%]`)** | 12/16 (75.0%, `[50.5–89.8%]`) | `llm` vs `llm_no_notes`/`baseline` `p = 0.70`, vs `no_brain` `p = 0.43`; pooled LLM `22/32` vs PB `25/32` `p = 0.57` |
| **Talon King Ikiss** (`556`, L68, `tbc-seth`) | **8/16 (50.0%, `[28.0–72.0%]`)** | 6/16 (37.5%, `[18.5–61.4%]`) | 1/16 (6.2%, `[1.1–28.3%]`) | 2/16 (12.5%, `[3.5–36.0%]`) | **`llm` vs `no_brain` `p = 0.015`**, `llm` vs `baseline` `p = 0.054`, `llm` vs `llm_no_notes` `p = 0.72`; **`pooled LLM` (`14/32`) vs `PB` (`3/32`) `p = 0.0038`** (`llm` vs PB `p = 0.0031`) |
| **5-Boss Aggregate (`d7b3e13`, 80/arm)** | 58/80 (72.5%, `[61.9–81.1%]`) | **60/80 (75.0%, `[64.5–83.2%]`)** | 51/80 (63.8%, `[52.8–73.4%]`) | 56/80 (70.0%, `[59.2–78.9%]`) | `llm` vs `llm_no_notes` `p = 0.86`, vs `no_brain` `p = 0.31`, vs `baseline` `p = 0.86`; pooled LLM `118/160` vs PB `107/160` `p = 0.22` |

---

## 3. Detailed Per-Boss Results (`16` Valid Trials per Arm under `d7b3e13`)

### 3A. Devourer of Souls (The Forge of Souls, L80, `strip_strat = wotlk-fos`)
`64/64` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 16/16 (100.0%) | 13,045.2 `[9,126.9–14,785.5]` | 1,677.5 `[837.0–3,278.0]` | 0.0 `[0.0–2.0]` | 47.9 s `[42.1–65.9]` | 287.4 ms / 985.6 ms (1,250.0) | 309 |
| `llm_no_notes` | 16/16 (100.0%) | 15,625.2 `[11,970.5–17,790.1]` | 1,717.8 `[849.9–3,079.7]` | 0.0 `[0.0–2.0]` | 40.4 s `[35.7–51.2]` | 383.1 ms / 972.0 ms (1,222.6) | 223 |
| `no_brain` | 16/16 (100.0%) | **15,856.8** `[11,576.2–17,310.8]` | **1,877.4** `[935.3–3,628.1]` | 0.0 `[0.0–2.0]` | **39.5 s** `[35.8–54.0]` | — | 0 |
| `baseline` | 16/16 (100.0%) | 13,191.9 `[11,999.1–14,829.9]` | 1,397.4 `[1,018.5–2,832.8]` | **0.0** `[0.0–0.0]` | 47.2 s `[42.8–52.6]` | — | 0 |

### 3B. Loken (Halls of Lightning, L80, `strip_strat = wotlk-hol`)
`64/64` valid trials (`0` invalid, `0` wave errors, `0` deaths across all `64` trials).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 16/16 (100.0%) | 11,537.7 `[6,930.4–13,644.6]` | **3,175.0** `[2,727.4–3,727.9]` | 0.0 `[0.0–0.0]` | 34.2 s `[28.9–56.5]` | 555.8 ms / 947.6 ms (1,259.1) | 163 |
| `llm_no_notes` | 16/16 (100.0%) | 15,508.0 `[13,345.1–16,974.7]` | 2,962.1 `[2,511.0–3,688.9]` | 0.0 `[0.0–0.0]` | 25.4 s `[23.2–29.3]` | 388.1 ms / 951.5 ms (1,232.3) | 210 |
| `no_brain` | 16/16 (100.0%) | **15,886.1** `[14,756.8–18,363.0]` | 2,921.8 `[2,351.3–3,318.0]` | 0.0 `[0.0–0.0]` | **24.9 s** `[21.3–26.5]` | — | 0 |
| `baseline` | 16/16 (100.0%) | 11,505.8 `[9,030.4–14,135.0]` | 1,411.8 `[1,057.7–3,085.1]` | 0.0 `[0.0–0.0]` | 34.4 s `[27.9–43.4]` | — | 0 |

### 3C. Kael'thas Sunstrider (Magisters' Terrace, L70, `strip_strat = tbc-mgt`)
`64/64` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 8/16 (50.0%) | 2,018.7 `[535.7–2,340.4]` | 1,398.1 `[647.4–2,056.9]` | 5.0 `[0.0–5.0]` | **78.2 s** `[48.2–116.6]` | 352.1 ms / 1,197.0 ms (1,305.2) | 987 |
| `llm_no_notes` | **10/16 (62.5%)** | **2,117.8** `[867.4–2,838.4]` | **1,586.5** `[847.6–2,128.6]` | 2.5 `[0.0–5.0]` | 81.6 s `[59.1–114.7]` | 331.0 ms / 1,220.1 ms (1,321.3) | 883 |
| `no_brain` | 5/16 (31.2%) | 1,971.7 `[679.7–3,342.1]` | 1,223.2 `[625.0–2,029.5]` | 5.0 `[2.0–5.0]` | 80.7 s `[44.3–117.6]` | — | 0 |
| `baseline` | **10/16 (62.5%)** | 1,238.8 `[485.9–2,124.1]` | 791.4 `[225.2–1,358.7]` | **2.0** `[0.0–6.0]` | 116.7 s `[53.2–197.7]` | — | 0 |

### 3D. Murmur (Shadow Labyrinth, L70, `strip_strat = ""`)
`64/64` valid trials (`1` `pre_pull_combat` invalid trial on Wave 5 `no_brain`, `3` extra overflow trials in Wave 17, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 10/16 (62.5%) | 2,280.5 `[1,079.0–3,381.3]` | 842.0 `[229.4–1,657.5]` | 3.0 `[0.0–5.0]` | 70.8 s `[50.2–157.9]` | 358.4 ms / 1,241.9 ms (1,610.2) | 876 |
| `llm_no_notes` | 12/16 (75.0%) | 2,405.5 `[816.5–3,429.4]` | 658.4 `[309.6–2,002.2]` | 2.0 `[0.0–5.0]` | 70.2 s `[50.2–210.0]` | 440.2 ms / 1,226.3 ms (1,635.1) | 764 |
| `no_brain` | **13/16 (81.2%)** | 3,080.1 `[1,802.0–4,694.4]` | **1,038.1** `[240.2–1,904.6]` | 2.0 `[0.0–5.0]` | **53.5 s** `[33.3–93.7]` | — | 0 |
| `baseline` | 12/16 (75.0%) | **3,119.8** `[2,032.9–4,258.5]` | 885.5 `[244.9–2,041.9]` | **1.5** `[0.0–5.0]` | 54.6 s `[33.0–79.9]` | — | 0 |

### 3E. Talon King Ikiss (Sethekk Halls, L68, `strip_strat = tbc-seth`)
`64/64` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 (max) | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | **8/16 (50.0%)** | **981.5** `[635.0–1,308.7]` | **599.4** `[335.2–1,078.0]` | **4.5** `[2.0–6.0]` | 75.1 s `[40.3–111.5]` | 239.5 ms / 988.4 ms (1,247.7) | 581 |
| `llm_no_notes` | 6/16 (37.5%) | 971.8 `[652.1–1,404.0]` | 588.4 `[398.8–1,278.7]` | 5.0 `[3.0–6.0]` | 73.3 s `[52.9–87.3]` | 335.1 ms / 1,067.3 ms (1,228.9) | 458 |
| `no_brain` | 1/16 (6.2%) | 908.0 `[660.6–1,484.9]` | 578.5 `[294.2–1,210.4]` | 5.0 `[3.0–6.0]` | **62.2 s** `[52.4–84.5]` | — | 0 |
| `baseline` | 2/16 (12.5%) | 647.5 `[454.5–954.7]` | 514.2 `[244.1–914.2]` | 5.0 `[2.0–5.0]` | 67.1 s `[39.5–148.5]` | — | 0 |

---

## 4. Standing-Order Dispatch, AoE Dodge Timelines & Boss-Specific Mechanics

Computed across the `320` scored trials in `d7b3e13` (per-trial window `[pull_ms - 500, pull_ms + duration_s*1000 + 800]` with `sample.inst == trial.instance_id`), using [`dodge_timeline.py`](../../tools/bench/dodge_timeline.py).

### Standing-Order Actions (`act_sent`) in Scored Trials
- **`no_brain` & `baseline` (all 5 bosses):** `0` `act_sent` events.
- **`llm_no_notes`:** `0` `act_sent` events on `devourer`, `kaelthas`, `murmur`, and `ikiss`; `2` LLM-issued `act_sent` events (`AWAY` `1`, `TO` `1`) on `loken`.
- **`llm`:**
  - `devourer` (`827` acts): `STOP` (`307`), `TARGET Devourer of Souls` (`276`), `TO Devourer of Souls` (`149`), `TARGET` (`34`), `PET_ATTACK Devourer of Souls` (`22`), `USE Devourer of Souls [err: unknown spell or item: Devourer of Souls]` (`15`), `USE Phantom Blast [err: ...]` (`5`), `AVOID Well of Souls` (`4`), `FACE Devourer of Souls` (`2`), `PET_ATTACK` (`2`), `USE Mirrored Soul [err: ...]` (`2`), `TO` (`2`).
  - `loken` (`318` acts): `AWAY Loken` (`134`), `TO Loken` (`115`), `AVOID Pulsing Shockwave` (`24`), `TARGET Loken` (`15`), `AVOID Lightning Nova` (`13`), `STOP` (`8`), `PET_ATTACK Loken` (`5`), `FACE Loken` (`4`).
  - `kaelthas` (`242` acts): `TARGET Phoenix Egg` (`83`), `AVOID Flame Strike` (`64`), `TARGET Kael'thas Sunstrider` (`42`), `TARGET Phoenix` (`25`), `TO` (`8`), `TARGET Coilskar Witch` (`5`), `TARGET Phoenix Egg [err: you are dead]` (`4`), `TARGET Sunblade Warlock` (`2`), `TO Phoenix Egg` (`1`), `STOP` (`1`), `TARGET Arcane Sphere [err: ...]` (`1`), `TARGET Ethereum Smuggler` (`1`).
  - `murmur` (`172` acts): `AWAY Murmur` (`55`), `STOP` (`54`), `AWAY` (`33`), `TO` (`14`), `TO Murmur` (`7`), `TARGET Murmur` (`4`), `TARGET Cabal Spellbinder` (`1`), `PET_ATTACK Murmur` (`1`), `AWAY Murmur [err: you are dead]` (`1`), `TO Ptwoa` (`1`), `TO Pthrb` (`1`).
  - `ikiss` (`147` acts): `AWAY Talon King Ikiss` (`94`), `TO` (`18`), `STOP` (`11`), `TARGET Talon King Ikiss` (`9`), `TARGET` (`5`), `TO Talon King Ikiss` (`4`), `USE Polymorph [err: cannot cast Polymorph now]` (`2`), `FACE Talon King Ikiss` (`1`), `AWAY` (`1`), `USE Pfoua [err: ...]` (`1`), `TARGET Polymorph` (`1`).

### AoE Dodge Timeline Table (`Loken`, `Murmur`, `Ikiss`)
**Independence note:** Multiple cast exposures inside one fight are correlated, so **independent trials (`n = 16`/arm) with `>= 1` safe-at-impact exposure** is the primary unit for Fisher `p`-values; exposure counts and distances are descriptive context. Because `TraceSample` runs at `~5 Hz` (`200 ms`) while `act_sent` logs on the exact server tick, `act_delay_ms` relative to the first sampled `target_casting` row `s0` is slightly negative (`-71.0 ms` to `-6.0 ms`) when the standing order dispatches between samples.

| Boss & Spell (Radius) | Arm | Trials w/ Dodge (`cast_trials` / `16`) | Exposures Safe at Impact | Impact Dist (Med / p75 / Max) | Med `act_delay_ms` (`n`) | Impact Dmg Med (Mean) | Exact Fisher `p` (`llm` vs arm, trial-level) |
|---|---|---:|---:|---|---|---:|---|
| **Loken** — `Lightning Nova` (`20.0 yd` DBC) | `llm` | 15/16 (`15/16`) | 72/155 (46.5%) | 18.5 / 24.5 / 52.1 yd | -54.0 ms (`131`) | 2,619 (4,367) | Baseline comparator |
| | `llm_no_notes` | 13/16 (`13/16`) | 20/80 (25.0%) | 13.1 / 19.5 / 37.7 yd | — | 8,020 (6,467) | `p = 0.60` vs `llm` |
| | `no_brain` | 10/16 (`10/16`) | 16/80 (20.0%) | 12.9 / 18.4 / 26.3 yd | — | 8,117 (6,895) | `p = 0.083` vs `llm` |
| | `baseline` | **16/16 (`16/16`)** | **139/150 (92.7%)** | **22.0** / 22.0 / 64.8 yd | — | **1,488 (1,417)** | `p = 1.00` vs `llm` (`wotlk-hol` steps precisely to `22.0 yd`) |
| **Murmur** — `Sonic Boom` (`34.0 yd` notes*) | `llm` | **12/16 (`12/16`)** | **19/87 (21.8%)** | **22.4** / 33.2 / 58.0 yd | -71.0 ms (`52`) | 7,095 (6,699), 18 deaths | Baseline comparator |
| | `llm_no_notes` | 0/16 (`0/16`) | 0/90 (0.0%) | 7.5 / 16.1 / 28.5 yd | — | 7,094 (6,039), 14 deaths | **`p = 1.61e-05`** vs `llm` |
| | `no_brain` | 0/16 (`0/16`) | 0/85 (0.0%) | 7.5 / 13.5 / 28.4 yd | — | 7,144 (7,208), 19 deaths | **`p = 1.61e-05`** vs `llm` |
| | `baseline` | 0/16 (`0/16`) | 0/83 (0.0%) | 7.5 / 11.6 / 32.1 yd | — | 7,345 (6,982), 18 deaths | **`p = 1.61e-05`** vs `llm` |
| **Ikiss** — `Arcane Explosion` (`35.0 yd` DBC) | `llm` | **10/16 (`10/16`)** | **27/179 (15.1%)** | 12.3 / 29.3 / 53.8 yd | -6.0 ms (`64`) | 0 (536), 6 deaths | Baseline comparator |
| | `llm_no_notes` | 1/16 (`1/16`) | 1/158 (0.6%) | 7.2 / 8.8 / 38.1 yd | — | 0 (154), 2 deaths | **`p = 0.0021`** vs `llm` |
| | `no_brain` | 0/15 (`0/16`) | 0/123 (0.0%) | 6.5 / 8.9 / 22.0 yd | — | 0 (368), 6 deaths | **`p = 0.0002`** vs `llm` |
| | `baseline` | 0/16 (`0/16`) | 0/125 (0.0%) | 17.2 / 21.4 / 33.4 yd | — | 0 (1,031), 11 deaths | **`p = 0.0002`** vs `llm` |

\*`Sonic Boom` emits `target_cast_radius = -1.0` in `TraceSample`; evaluated at `--radius 34` in `dodge_timeline.py`. Note on **Ikiss** median impact damage = `0`: bots remaining inside `35 yd` take `Arcane Explosion` damage cleanly on the tick after cast completion or only if not line-of-sighted behind pillars (`11` impact-window deaths across `125` exposures on `baseline` vs `6` across `179` exposures on `llm`).

### Devourer of Souls & Kael'thas Trial-Level Mechanics (`n = 16`/arm)
- **Kael'thas Sunstrider (`Flame Strike`, `Phoenix Egg`, `Arcane Sphere`):** `Flame Strike` is cast by `Flame Strike Trigger (Kael - 5Man)`, so boss `target_casting` shows `0` `Flame Strike` windows. At the trial level (`n = 16`/arm):
  - **`baseline` (`tbc-mgt`) dominated `Flame Strike` avoidance:** **`14/16` trials kept `Flame Strike` damage `<= 1,000`** (`13/16` at `0` dmg; median **`0.0`** `[0–7,359]`, total **`13,126`**), compared with **`2/16`** in `llm` (`1/16` at `0`; median **`4,777.0`** `[0–33,240]`, total **`128,571`**, **`p = 4.88e-05`**), **`2/16`** in `llm_no_notes` (`0/16` at `0`; median **`8,197.0`** `[845–25,471]`, total **`168,394`**, **`p = 4.88e-05`**), and **`4/16`** in `no_brain` (`3/16` at `0`; median **`7,511.5`** `[0–41,700]`, total **`139,233`**, **`p = 0.0010`**).
  - **`Arcane Sphere` deaths & `Phoenix Egg` kills:** `baseline` also suffered far fewer `Arcane Sphere` deaths (**`6`** vs **`27`** `llm`, **`28`** `llm_no_notes`, **`43`** `no_brain`) while destroying `23` deduped `Phoenix Egg` spawns (`101` bot `mob_kill` credits) vs `17` (`72` credits) in `llm`, `19` (`90`) in `llm_no_notes`, and `19` (`86`) in `no_brain`. Nearby trash packs affected every arm (`12–16/16` trials across arms).
- **Devourer of Souls (`Well of Souls`, `Mirrored Soul` / `Wailing Souls`):**
  - `Well of Souls` standing orders rarely fired (`AVOID Well of Souls`: **`4`** total acts across 16 trials), and `llm` took more `Well of Souls` damage (`1/16` at `0`, `5/16 <= 5,000`, median **`13,311.0`** `[0–68,541]`, total **`387,294`**) than `llm_no_notes` (`5/16` at `0`, `9/16 <= 5,000`, median **`2,995.0`** `[0–54,758]`, total **`148,905`**), `no_brain` (`2/16` at `0`, `6/16 <= 5,000`, median **`7,413.5`**, total **`225,145`**), and `baseline` (`2/16` at `0`, `7/16 <= 5,000`, median **`5,096.5`**, total **`223,050`**).
  - `Mirrored Soul` `STOP` standing orders (`307` acts in `llm`) reduced sustained party DPS (`13,045.2` / `47.9 s` vs `15,625.2` / `40.4 s` `llm_no_notes` and `15,856.8` / `39.5 s` `no_brain`) without lowering non-tank boss damage (`42,214.0` median / `649,080` total in `llm` vs `37,928.5` / `589,554` `llm_no_notes`, `31,068.5` / `435,974` `no_brain`, `25,626.5` / `445,899` `baseline`), and `llm` clipped entrance `Spectral Warden` trash (`2/16` trials: Waves `7` & `13`, `10,135` total dmg, `0/16` elsewhere).

---

## 5. Key Takeaways & Honest Caveats

1. **LLM arms significantly outperform playerbots on Ikiss (`p = 0.0038` pooled; `llm` vs `no_brain` `p = 0.015`), even on clean pulls:**
   - **Ikiss (`8/16` `llm`, `6/16` `llm_no_notes` vs `1/16` `no_brain`, `2/16` `baseline`):** Pooled LLM (`14/32 = 43.8%`) beat pooled playerbots (`3/32 = 9.4%`) at **`p = 0.0038`**, and `llm` (`50.0%`) beat `no_brain` (`6.2%`) at **`p = 0.015`** (`llm` vs `baseline` `p = 0.054`). Because **`no_brain` had `0/16` trash pulls (`1/16` kills)** and **`llm_no_notes` had `0/16` trash pulls (`6/16` kills, `p = 0.083`)**, LLM combat decisions clearly help on Ikiss independently of trash pulls; on `llm`, clean pulls went **`6/9` (`66.7%`)** while `AWAY Talon King Ikiss 35` standing orders pulled nearby Sethekk trash in `7/16` trials (`2k/5w`), vs `baseline` (`tbc-seth`) pulling trash in `15/16` trials (`2k/13w`; `0/1` clean).
2. **`notes.md` standing orders produce large, highly significant live AoE movement differences (`p <= 0.0021`), but naive run-out rules carry real DPS and secondary-mechanic tradeoffs:**
   - **Murmur (`Sonic Boom`):** `llm` dodged `>= 34 yd` in **`12/16` trials vs `0/16` in all other arms (`p = 1.61e-05`)**, yet killed fewer times (`10/16` vs `13/16` `no_brain`, `p = 0.43`, n.s.) because `AWAY` + `54` `STOP` commands dropped median DPS (`2,280.5` / `70.8 s` vs `3,080.1` / `53.5 s`) and drew heavier `Cabal` room-trash damage (`253,676` non-Murmur mob dmg in `llm` vs `90,214` `no_brain`; `8` non-Murmur mob deaths + `1` environmental death vs `0` in `no_brain`).
   - **Ikiss (`Arcane Explosion`):** `llm` cleared `>= 35 yd` on `>= 1` bot in **`10/16` trials vs `1/16` `llm_no_notes` (`p = 0.0021`) and `0/16` `no_brain`/`baseline` (`p = 0.0002`)**.
   - **Loken (`Lightning Nova` + `Pulsing Shockwave`):** ALL 64 trials were `100%` kills (`0` deaths), and `llm` cleared `>= 20 yd` in `15/16` trials (cutting `Nova` impact damage from `8,020–8,117` to `2,619`). However, `Loken`'s `Pulsing Shockwave` scales linearly with distance (`spell_loken_pulsing_shockwave`), so `llm`'s `25+ yd` runouts (`134` `AWAY Loken` acts, `34.2 s` med fight vs `24.9 s` `no_brain`) more than **doubled non-tank boss damage (`1,739,938` vs `951,402` `no_brain` and `776,553` `baseline`)** and doubled healing required (`3,175.0` vs `1,411.8` `baseline`), whereas `baseline` (`wotlk-hol`) stacked close cleanly and stepped right outside `20 yd` (`22.0 yd` med, `139/150` safe exposures).
   - **Kael'thas (`Flame Strike`):** `baseline` (`tbc-mgt`) kept `Flame Strike` damage `<= 1,000` in **`14/16` trials (`p <= 0.0010` vs all 3 non-scripted arms)** and suffered only `6` `Arcane Sphere` deaths (`vs 27–43`), matching `llm_no_notes` at `10/16` kills (`62.5%`). Adding `AVOID Flame Strike` in `llm` (`64` acts) did not prevent ground-trigger damage (`128,571` tot) or lower kill time (`8/16`, `p = 0.72`).
3. **Why overall `5`-boss kill rates were `72.5%` / `75.0%` vs `63.8%` / `70.0%` (`p = 0.22–0.86`, n.s.):**
   - Both WotLK 5-man bosses (`Devourer` and `Loken`) are `100%` kill-rate ceilings for L80 epic parties (`32/32` everywhere). Separating `notes.md` kill effects in WotLK requires harder encounters (e.g., heroic/hard-mode tuning, undergeared fixtures, or raid encounters).
   - Where `llm` clearly beats `llm_no_notes` and `baseline` is **trial-level AoE evasion (`Murmur` `p = 1.61e-05`, `Ikiss` `p <= 0.0021`)** and **higher Ikiss kill rate (`p = 0.015` vs `no_brain`, `p = 0.0038` pooled)**; where `baseline` C++ scripts beat LLMs is **scripted ground-trigger evasion (`Kael'thas` `Flame Strike`, `p = 4.88e-05`)** and **exact mechanic-tuned step-out distance (`Loken`, `139/150` at `22.0 yd`)**; where **notes-based runouts hurt throughput (`Murmur`, `Devourer`)**, raw `no_brain`/`llm_no_notes` zerg finishes `~8–17 s` faster.

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
