# Four-Boss × Four-Arm Parallel Benchmark Report (`trace-all-events`)

**Date:** `2026-11-20` – `2026-11-21` (branch `trace-all-events`, `main` untouched at `2834756`)  
**Runner:** [`tools/bench/bench_bosses.py`](../../tools/bench/bench_bosses.py) (`--mode mixed_all --runs 8 --parallel 4`)  
**Valid scored cohorts:** Two **64-trial cohorts** (`8` valid runs per arm per boss, **`0` invalid trials**, **`0` wave errors** in the final cohort datasets):  
- **Cohort A (`523ab4c`):** `Kael'thas` (`32` valid) + `Ick` (`32` valid), using the `3.0 s` entrance sleep + `0.2 s`/bot pull stagger.  
- **Cohort B (`aac0546`):** `Murmur` (`32` valid) + `Ikiss` (`32` valid), after cutting entrance settle to `~0.2 s` (`_wait_harness_connected` + `0.05 s`/bot move) and fixing three multi-wave bugs (re-run from scratch after discarding the broken `523ab4c` waves below).

> [!NOTE]
> The single-commit 5-boss × 4-arm × 16-runs benchmark (`devourer,loken,kaelthas,murmur,ikiss`, `320` planned valid trials) replacing `ick` is currently in progress; the tables below record the prior `n = 8` two-cohort baseline.

---

## 1. Experimental Setup, Cohorts & Discarded Runs

Every boss ran in `mixed_all` mode across 4 simultaneous 5-man parties (`--parallel 4`) inside a single persistent test `worldserver` (`out/server-test`) with identical hardware/server contention:

- **Party composition (`COMP_5`):** Protection Warrior (`tank`, `spec_tab=2`), Holy Priest (`heal`, `spec_tab=1`), Frost Mage (`dps`, `spec_tab=2`), Destruction Warlock (`dps`, `spec_tab=2`), Marksmanship Hunter (`dps`, `spec_tab=1`), geared to boss level in `ITEM_QUALITY_EPIC` via `PlayerbotFactory::Randomize` + `InitTalentsBySpecNo`.
- **Four arms rotated every wave** via `_wave_arms` (two-cycle Latin rotation so every arm ran in every party slot `Load..Pfou` twice across 8 waves):
  - **`llm`**: `harness run` + `llama-server` (`gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf`, `--slots 16 --temperature 0`) with boss `notes.md` (standing orders + tactics); playerbots hardcoded boss strategy stripped via `llmtest setstrat ... -<strip_strat>`.
  - **`llm_no_notes`**: `harness run` + `llama-server` **without** `notes.md`; playerbots boss strategy stripped.
  - **`no_brain`**: pure playerbots (excluded from `--only-bots`), playerbots boss strategy stripped.
  - **`baseline`**: pure playerbots (excluded from `--only-bots`), playerbots hardcoded C++ boss strategy **ON** (`+<strip_strat>`).
- **Note on Murmur (`Shadow Labyrinth`, `map=555`):** `strip_strat=""` because `mod-playerbots` has no dedicated Murmur/Shadow Labyrinth strategy. On Murmur, `no_brain` and `baseline` therefore act as two independent 8-trial control samples of unscripted playerbots AI.
- **Per-trial verification & isolation:** Verified `role=` and stored `specNo=` at setup; between waves reset on `map=0` via `llmtest resetbot`, re-verified party grouping via `llmtest group`, staged at dungeon entrances (`areatrigger_teleport`) before starting `harness`, verified `alive=1` + distinct never-reused `instance_id`s, verified per-bot `llmtest checkstrat`, moved to `spawn_xyz` (`0` pre-pull combat, `0` zero-pull trials in final cohorts), and sliced traces per party `[pull_ms - 1000, pull_ms + duration_s*1000 + 3000]` matching each trial's `instance_id`.
- **Pre-first-plan pull handicap on LLM arms:** [`bench_bosses.py`](../../tools/bench/bench_bosses.py) teleports parties straight into boss aggro range at `spawn_xyz` as soon as `harness` connects to `brain.sock` (`~0.2 s` in Cohort B; Cohort A's `3.0 s` settle sat at the dungeon entrance out of boss line-of-sight). Thus **`llm` and `llm_no_notes` are pulled before their first boss-room LLM decision — and in Cohort B before their initial `notes.md` standing-order compilation — arrives (`first_line_ms` p50 `242–479 ms`, p95 `997–1,400 ms`, plus full-reply generation)**, whereas `baseline`'s C++ boss triggers are active from tick `0`.
- **Execution provenance & Cohort A (`523ab4c`) vs Cohort B (`aac0546`):**
  - `kaelthas` and `ick` completed cleanly at `523ab4c` (`32/32` valid each, `0` invalid, `0` wave errors).
  - **Discarded pre-`aac0546` runs on `murmur` and `ikiss`:** the initial `523ab4c` attempt (`12` waves each) hit three multi-wave harness bugs and was discarded in full and re-run cleanly at `aac0546`:
    - **Discarded `Murmur` (`523ab4c`, `12` waves):** `15` partial valid trials (`llm` `3/3` kills, `llm_no_notes` `4/4`, `no_brain` `4/4`, `baseline` `4/4`), **`1` `pre_pull_combat` invalid trial (`llm`)**, and **`8` failed waves** (`Wave 3`: `Loadb` died upon `Spirit of Redemption` removal; `Waves 6–12`: `Loadd` disbanded after `death count >= 5` `RandomPlayerbotMgr::Revive` and entered a separate instance).
    - **Discarded `Ikiss` (`523ab4c`, `12` waves):** `8` partial valid trials (`llm` `1/1` kill, `llm_no_notes` `0/1`, `no_brain` `1/3`, `baseline` `0/3`), **`4` `pre_pull_combat` invalid trials (`2` `llm`, `2` `llm_no_notes` during the `3.0 s` entrance sleep)**, and **`9` failed waves** (`Waves 4–12`: `Pfoud` disbanded after `death count >= 5` and entered a separate instance).
  - Fixing those three harness bugs in `aac0546` ([`test_commands.cc`](../../server_module/src/test/test_commands.cc) and [`bench_bosses.py`](../../tools/bench/bench_bosses.py), with **no** changes to the LLM brain, snapshot, options, or scorer) stripped `SPELL_AURA_SPIRIT_OF_REDEMPTION` before resurrecting, reset `"death count"` / `"dead"` / `"revive"` timers and re-checked party grouping, and replaced the `3.0 s` entrance wait with `_wait_harness_connected` (`~0.2 s`) + `0.05 s`/bot move — cutting LLM arms' entrance pre-pull window from `~3 s` to `~0.2 s` between Cohort A and Cohort B.

---

## 2. Executive Summary (Kills / 8 Valid Trials, Wilson 95% CIs & Fisher Tests)

Because Cohorts A and B used different pre-pull timings (`3.0 s` vs `~0.2 s`), results are reported by cohort with **no pooled total across commits**. All `p`-values are two-sided Fisher's exact tests (`n = 8` per arm).

| Cohort / Boss (`inst`, Lvl, `strip_strat`) | `llm` | `llm_no_notes` | `no_brain` | `baseline` | Exact Fisher `p` (`llm` vs others) |
|---|---:|---:|---:|---:|---|
| **A (`523ab4c`): Kael'thas** (`585`, L70, `tbc-mgt`) | 6/8 (75.0%, `[40.9–92.9%]`) | 6/8 (75.0%, `[40.9–92.9%]`) | 6/8 (75.0%, `[40.9–92.9%]`) | 7/8 (87.5%, `[52.9–97.8%]`) | `p = 1.00` vs all 3 arms |
| **A (`523ab4c`): Ick** (`658`, L80, `wotlk-pos`) | 8/8 (100.0%, `[67.6–100.0%]`) | 8/8 (100.0%, `[67.6–100.0%]`) | 8/8 (100.0%, `[67.6–100.0%]`) | 8/8 (100.0%, `[67.6–100.0%]`) | `p = 1.00` (ceiling: too easy to separate) |
| **B (`aac0546`): Murmur** (`555`, L70, *(none)*) | 6/8 (75.0%, `[40.9–92.9%]`) | 8/8 (100.0%, `[67.6–100.0%]`) | 7/8 (87.5%, `[52.9–97.8%]`) | 8/8 (100.0%, `[67.6–100.0%]`) | `p = 0.47` vs `8/8`, `p = 1.00` vs `7/8` (not significant) |
| **B (`aac0546`): Talon King Ikiss** (`556`, L68, `tbc-seth`) | 6/8 (75.0%, `[40.9–92.9%]`) | 6/8 (75.0%, `[40.9–92.9%]`) | 3/8 (37.5%, `[13.7–69.4%]`) | 2/8 (25.0%, `[7.1–59.1%]`) | Pairwise `p = 0.31` vs `no_brain`, `p = 0.13` vs `baseline` *(suggestive only at `n=8`; pooled `12/16` vs `5/16` `p = 0.032`, confounded by `baseline` trash pulls)* |

---

## 3. Detailed Per-Boss Results (`8` Valid Trials per Arm)

### 3A. Kael'thas Sunstrider (Magisters' Terrace, L70, `strip_strat = tbc-mgt`)
`32/32` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 6/8 (75.0%) | 2,055.1 `[1,699.3–2,357.0]` | 1,496.1 `[1,059.3–1,954.6]` | 2.0 `[1–6]` | 82.6 s `[70.2–99.0]` | 417.2 ms / 1,068.6 ms | 578 |
| `llm_no_notes` | 6/8 (75.0%) | 2,158.2 `[1,321.2–2,473.0]` | 1,351.3 `[693.0–1,848.7]` | 3.0 `[0–6]` | 81.1 s `[68.8–97.7]` | 409.1 ms / 996.7 ms | 524 |
| `no_brain` | 6/8 (75.0%) | **2,190.7** `[1,761.7–2,487.3]` | 1,483.0 `[920.5–1,828.0]` | 2.5 `[1–6]` | **73.9 s** `[63.8–97.4]` | — | 0 |
| `baseline` | **7/8 (87.5%)** | 1,901.8 `[1,474.2–2,056.4]` | 981.5 `[523.5–1,358.1]` | **0.0** `[0–6]` | 91.1 s `[70.4–117.8]` | — | 0 |

### 3B. Ick (Pit of Saron, L80, `strip_strat = wotlk-pos`)
`32/32` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 8/8 (100.0%) | 7,329.4 `[5,342.7–9,493.4]` | 3,609.2 `[2,317.1–5,067.5]` | 0.0 `[0–3]` | 82.7 s `[63.8–112.7]` | 272.8 ms / 1,150.8 ms | 341 |
| `llm_no_notes` | 8/8 (100.0%) | 7,483.8 `[5,299.2–9,583.5]` | 3,624.5 `[2,456.7–5,273.0]` | 0.0 `[0–0]` | 81.7 s `[62.7–113.3]` | 393.7 ms / 1,023.0 ms | 281 |
| `no_brain` | 8/8 (100.0%) | **7,589.4** `[5,792.9–12,441.3]` | 2,968.5 `[1,651.1–3,793.9]` | 0.0 `[0–0]` | **80.0 s** `[48.4–104.7]` | — | 0 |
| `baseline` | 8/8 (100.0%) | 7,359.9 `[4,737.5–9,323.7]` | 3,456.4 `[2,821.8–5,927.5]` | 0.0 `[0–2]` | 81.7 s `[65.4–127.1]` | — | 0 |

### 3C. Murmur (Shadow Labyrinth, L70, `strip_strat = ""`)
`32/32` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | 6/8 (75.0%) | 2,845.1 `[1,704.1–3,513.0]` | 1,153.2 `[495.5–1,687.3]` | 1.0 `[0–5]` | 58.4 s `[47.5–74.3]` | 344.4 ms / 1,399.8 ms | 421 |
| `llm_no_notes` | **8/8 (100.0%)** | 2,837.8 `[2,186.4–3,689.6]` | 1,268.9 `[1,022.0–1,766.7]` | 1.0 `[0–2]` | 59.9 s `[46.0–77.8]` | 479.1 ms / 1,323.1 ms | 402 |
| `no_brain` | 7/8 (87.5%) | **4,346.1** `[3,100.7–4,787.2]` | 1,870.0 `[932.3–2,959.6]` | **0.5** `[0–5]` | **39.1 s** `[36.3–49.0]` | — | 0 |
| `baseline` | **8/8 (100.0%)** | 3,654.0 `[3,038.0–5,062.4]` | 1,828.4 `[1,069.9–2,171.3]` | **0.5** `[0–3]` | 46.8 s `[34.0–56.5]` | — | 0 |

### 3D. Talon King Ikiss (Sethekk Halls, L68, `strip_strat = tbc-seth`)
`32/32` valid trials (`0` invalid, `0` wave errors).

| Arm | Kills | Med DPS `[min–max]` | Med HPS `[min–max]` | Med Deaths `[min–max]` | Med Dur `[min–max]` | `first_line_ms` p50 / p95 | LLM Calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| `llm` | **6/8 (75.0%)** | **1,229.7** `[824.6–1,440.9]` | **1,069.2** `[660.6–1,568.8]` | **3.0** `[1–6]` | 63.7 s `[54.2–76.1]` | 241.8 ms / 1,233.3 ms | 288 |
| `llm_no_notes` | **6/8 (75.0%)** | 1,152.8 `[943.9–1,384.8]` | 851.9 `[576.0–1,201.7]` | 4.0 `[3–6]` | 64.4 s `[45.1–76.9]` | 295.8 ms / 1,117.1 ms | 272 |
| `no_brain` | 3/8 (37.5%) | 1,068.5 `[791.4–1,363.8]` | 725.5 `[579.3–1,519.8]` | 5.5 `[3–6]` | 55.5 s `[39.3–96.0]` | — | 0 |
| `baseline` | 2/8 (25.0%) | 731.3 `[562.1–1,062.3]` | 564.9 `[235.0–981.5]` | 5.0 `[1–5]` | 72.6 s `[45.2–100.0]` | — | 0 |

---

## 4. Standing-Order Dispatch & AoE Dodge Timeline Metrics

Computed via [`dodge_timeline.py`](../../tools/bench/dodge_timeline.py) across the 32 scored trials per boss (matching each party's 5 members and `instance_id`).

### Standing-Order Actions (`act_sent`) in Scored Trials
- **`llm_no_notes` (all 4 bosses):** `0` standing-order `act_sent` events (no `notes.md` standing orders provided).
- **`llm`:**
  - `kaelthas`: `AVOID Flame Strike` (27), `TARGET Phoenix Egg` (25), `TARGET Phoenix` (11), `TARGET Kael'thas Sunstrider` (11), `TO Kael'thas Sunstrider` (2), `TO Phoenix` (1), `TARGET Sunblade Blood Knight` (1)
  - `ick`: `AVOID Toxic Waste` (120), `TO Ick` (16), `AWAY` (10), `AWAY Ick` (8), `STOP` (7), `KITE Ick` (5), `TO` (4), `TO Krick` (3), `TO Ptwod` (3)
  - `murmur`: `AWAY Murmur` (25), `STOP` (19), `AWAY` (16), `TO Murmur` (7), `TO` (6), `TARGET Cabal Spellbinder` (4), `TARGET Murmur` (3), `TARGET Cabal Summoner` (2)
  - `ikiss`: `AWAY Talon King Ikiss` (58), `TO` (19), `TARGET Talon King Ikiss` (10), `STOP` (4), `TO Talon King Ikiss` (3), `FACE Talon King Ikiss` (3), `TARGET Sethekk Prophet` (1), `PET_ATTACK Talon King Ikiss` (1)

### AoE Dodge Timeline Table
**Independence note:** Multiple cast exposures (and multiple party members) inside the same trial are correlated, so **trials with `>= 1` safe-at-impact dodge** (`n = 8` independent trials/arm) is the primary statistical unit for two-sided Fisher tests; exposure counts below are descriptive context only. Also note `act_delay_ms`: `TraceSample` runs at ~`5 Hz` (`200 ms`), whereas `act_sent` is logged on the exact world tick when a standing order fires, so `act_delay_ms` relative to the first sampled `target_casting` row `s0` can be slightly negative (`-76.5 ms` / `-10.0 ms`) when the order dispatched between samples.

| Boss & Spell (DBC Radius) | Arm | Trials w/ Dodge (of cast trials / of 8) | Exposures Safe at Impact | Med Impact Dist | Med `act_delay_ms` | Fisher `p` (`llm` vs arm, trial-level) |
|---|---|---:|---:|---:|---:|---|
| **Ick** — `Poison Nova` (`15.0 yd`) | `llm` | 1/1 (`1/8` all) | 1/5 (20.0%) | 9.9 yd | 2,402.0 ms (2) | `llm` also dispatched 120 `AVOID Toxic Waste` acts |
| | `llm_no_notes` | 1/2 (`1/8` all) | 2/10 (20.0%) | 10.2 yd | — | `p = 1.00` (`1/1` vs `1/2`; `1/8` vs `1/8`) |
| | `no_brain` | 0/0 (`0/8` all) | — | — | — | `p = 1.00` (`1/8` vs `0/8`) |
| | `baseline` | 4/4 (`4/8` all) | 25/25 (100.0%) | 31.5 yd | — | `p = 0.28` (`1/8` vs `4/8`, hardcoded `wotlk-pos`) |
| **Murmur** — `Sonic Boom` (no DBC radius*; `34.0 yd` per notes) | `llm` | **6/7** (**85.7%**, `6/8` all) | 17/40 (42.5%) | 26.4 yd (p75 `43.2`, max `58.0`) | -76.5 ms (26) | Cut med impact dmg to **5,323** (`mean 5,193`) |
| | `llm_no_notes` | 1/8 (`12.5%`) | 1/40 (2.5%) | 7.7 yd (p75 `18.2`, max `37.7`) | — | **`p = 0.010`** on cast trials (`p = 0.041` all 8); med dmg `7,894` |
| | `no_brain` | 0/8 (`0.0%`) | 0/40 (0.0%) | 7.6 yd (p75 `15.4`, max `31.8`) | — | **`p = 0.0014`** on cast trials (`p = 0.0070` all 8); med dmg `7,780` |
| | `baseline` | 0/8 (`0.0%`) | 0/40 (0.0%) | 7.7 yd (p75 `18.1`, max `28.5`) | — | **`p = 0.0014`** on cast trials (`p = 0.0070` all 8); med dmg `7,769` |
| **Ikiss** — `Arcane Explosion` (`35.0 yd`) | `llm` | **5/8** (**62.5%**) | 12/110 (10.9%) | 10.2 yd (max `42.8`) | -10.0 ms (48) | Hunter `5/21`, Tank `4/23`, Priest `3/22` exposures |
| | `llm_no_notes` | 0/8 (`0.0%`) | 0/87 (0.0%) | 7.2 yd (max `31.9`) | — | **`p = 0.026`** vs `llm` |
| | `no_brain` | 0/8 (`0.0%`) | 0/68 (0.0%) | 7.1 yd (max `22.6`) | — | **`p = 0.026`** vs `llm` |
| | `baseline` | 0/8 (`0.0%`) | 0/65 (0.0%) | 18.5 yd (max `32.9`) | — | **`p = 0.026`** vs `llm` (`tbc-seth` pulled room trash) |

\*`Sonic Boom` emits `target_cast_radius = -1.0` in `TraceSample` (`CalcRadius()` on the self-cast entry returns `0`); the `>= 34.0 yd` column above evaluates Murmur's `34 yd` blast radius using `--radius 34` in `dodge_timeline.py`.  
On **Kael'thas**, `Flame Strike` is cast by `Flame Strike Trigger (Kael - 5Man)` rather than on Kael'thas's own `target_casting` bar (`0` `Flame Strike` boss-cast windows matched by `target_casting`). At the **trial level** (`n = 8`/arm), `baseline` (`tbc-mgt`) kept `Flame Strike` damage `<= 1,000` on **`8/8` trials** (median `0.0` `[0–845]`, total **`845`**), compared with **`0/8`** on `llm` (median `6,666.0` `[3,791–25,133]`, total **`84,834`**, Fisher `p = 0.0002`), **`0/8`** on `llm_no_notes` (median `5,227.5` `[2,009–27,944]`, total **`80,253`**, `p = 0.0002`), and **`1/8`** on `no_brain` (median `13,053.5` `[0–24,803]`, total **`93,652`**, `p = 0.0014`). Party `Phoenix Egg` kill credits were steady across all four arms (`37–41` over 8 trials).

---

## 5. Key Takeaways & Honest Caveats

1. **Kill-rate differences are not statistically significant pairwise at `n = 8` (and Ikiss is suggestive only):**
   - **Ikiss (`6/8` `llm` & `6/8` `llm_no_notes` vs `3/8` `no_brain` & `2/8` `baseline`):** pairwise differences are **suggestive, not statistically significant at `n = 8`** (Fisher `p = 0.31` vs `no_brain`, `p = 0.13` vs `baseline`). Even the pooled comparison (`12/16` LLM arms vs `5/16` playerbots arms, `p = 0.032`) is confounded because `baseline` (`tbc-seth`) body-pulled nearby `-x` Sethekk room trash (`49–65 yd` from `spawn_xyz`) in **`7/8` trials** and **`6/6` wipes**, depressing throughput (`731.3` DPS, `564.9` HPS); `llm` pulled trash in `2/8` trials (its `2/2` wipes) and `llm_no_notes` had `0/8` trash pulls.
   - **Kael'thas (`6/8` vs `7/8`, `p = 1.00`), Murmur (`6/8` vs `8/8`, `p = 0.47`), and Ick (`8/8` everywhere, `p = 1.00`):** none shows a statistically significant kill-rate difference at `n = 8`, and Ick at L80 in epic gear is too easy to separate the arms.
2. **What the data does support — `notes.md` standing orders change AoE dodge behavior live (`p <= 0.026` at trial level), while `baseline` avoids `Flame Strike` much better (`p = 0.0002`):**
   - On **Murmur**, `llm` dodged `Sonic Boom` (`>= 34 yd` at impact) in **`6/7` (`85.7%`) cast trials (`6/8` overall)** vs **`1/8`** on `llm_no_notes` (`p = 0.010`) and **`0/8`** on `no_brain`/`baseline` (`p = 0.0014`), cutting median impact damage by ~32% (`5,323` vs `7,769–7,894`) while trading off sustained DPS (`2,845.1` / `58.4 s` vs `4,346.1` / `39.1 s` for pure tank-and-spank `no_brain`) and pulling deeper room trash (`6/8` `llm`, `5/8` `llm_no_notes`, `0/8` `no_brain`/`baseline`).
   - On **Ikiss**, `llm` cleared `>= 35 yd` on `>= 1` bot in **`5/8` trials** vs **`0/8`** on all other arms (`p = 0.026`), though only `12/110` individual exposures cleared the full `35 yd` radius.
   - On **Kael'thas**, `baseline` (`tbc-mgt`) avoided almost all `Flame Strike` damage (`845` total, `0.0` median/trial vs `5,227.5–13,053.5` median/trial in the non-scripted arms, `p <= 0.0014`); `Arcane Sphere` during `Gravity Lapse` caused the majority of deaths (`50` total across the 32 trials).
3. **Methodology caveats & follow-up:**
   - **Immediate pull handicap on LLM arms:** pulling at `spawn_xyz` immediately upon connect puts `llm` and `llm_no_notes` into combat before their first boss-room LLM decision (and in Cohort B before initial `notes.md` standing-order compilation) finishes (`first_line_ms` p50 `242–479 ms`, p95 `997–1,400 ms`), whereas `baseline` starts with its C++ scripts active from tick `0`.
   - **Room trash & environmental death:** nearby trash affects all 4 arms on each pull (`kaelthas`: 6-mob pack at `34–41 yd`; `ick`: Plagueborn Horrors at `21–25 yd`; `murmur`: 3 Cabal Spellbinders at `28–30 yd`; `ikiss`: Sethekk packs at `49–65 yd`). One live environmental death (`event="died"`, `detail="environment"`) was recorded in `murmur` (`Pfoua` after post-fight `tele 0` when `Murmur's Touch` triggered `Shockwave` fall damage `3,579`).
   - **Next experiment (in progress):** single-commit 5-boss × 4-arm × 16-runs benchmark (`devourer,loken,kaelthas,murmur,ikiss`, `320` total valid trials) replacing `ick` (`8/8` ceiling) with `devourer` and `loken` and testing `notes.md` kill-rate effects on `kaelthas`, `murmur`, and `ikiss`.

---

## 6. Runner Speedup Decisions & Skipped Proposals

### Measured Wall-Clock Breakdown & Control-Arm Impact
Across the two 64-trial cohorts (`kaelthas` + `ick` at `523ab4c`, `murmur` + `ikiss` at `aac0546`, `32` waves total) at `--parallel 4` (measured directly from `pull_ms` and per-wave `max(duration_s)` in `var/bench_boss_10_mixed_all.json`):
- **Boss combat (`43.5 min` / `2,608.3 s` across 32 waves):** `kaelthas` `762.1 s` (`12.7 m`), `ick` `731.0 s` (`12.2 m`), `murmur` `495.3 s` (`8.3 m`), `ikiss` `619.9 s` (`10.3 m`).
- **Control arms (`no_brain` & `baseline`) were NOT the bottleneck:** they use **0 `llama-server` GPU slots** and added only **`+2.2 min` (`132.1 s`, `+5%`)** total across all 32 waves while yielding 64 matched control trials (`sum(max_all_4) = 2,608.3 s` vs `sum(max_llm_2) = 2,476.2 s`: `+46.9 s` `kaelthas`, `+18.7 s` `ick`, `+0.0 s` `murmur`, `+66.5 s` `ikiss`). The true ceiling on throughput is the `16`-slot single-GPU `llama-server` capacity (~10 combat-rate LLM bots in `mixed_all`, or up to 3 LLM parties = 15 bots via `--mode mixed --parallel 3`) multiplied by 8 sequential waves.
- **Between-wave staging (`20.9 min` / `1,255.1 s` across the 28 inter-wave gaps):** median gap between pull end and next wave pull was `51.6 s` (`kaelthas`), `45.6 s` (`ick`), `34.9 s` (`murmur`), and `47.6 s` (`ikiss`), dominated by fixed per-command `wait_s` sleeps (~140 console commands/wave) and `5.0 s` trace polling.
- **Per-boss server reset + gear:** `~85 s/boss` (`~5.7 min` across 4 bosses before speedups; `44.10 s` on Ikiss after).

### Implemented Validity-Preserving Speedups (Items 1–3)
1. **Item 1 — Explicit `phase_timings` telemetry:** [`bench_bosses.py`](../../tools/bench/bench_bosses.py) now records `setup_s`, `teardown_s`, `wall_s`, and per-wave `reset_s`, `stage_s`, `strat_s`, `connect_s`, `fight_s`, `score_s`, and `total_s` in the output JSON.
2. **Item 2 — Auto `--parallel 4` in `mixed_all`:** Defaults `--parallel` to `4` when `--mode mixed_all` (1 party per arm, holding LLM load at 10 bots on 16 `llama-server` slots) and `3` otherwise.
3. **Item 3 — Fast-polled serialized console dispatch, `_wait_where`, and `1.0 s` fight poll:**
   - `_console_batch` sends one tokenized command at a time (`wait_s=0.0`) and polls `worldserver.out` every `5 ms` until the `\n`-terminated reply and `AC> ` prompt appear (preventing GNU `readline` echo on `CliThread` from interleaving into `CliHandler::SendSysMessage` `utf8print` output).
   - `_wait_where` polls worldport arrival in two stages (party leaders first, then followers so `InstanceSaveMgr` group binds exist) instead of sleeping blindly.
   - Active fight monitoring polls `_TraceTailer` every `1.0 s` (down from `5.0 s`) with `0.5 s` post-completion settle (down from `2.0 s`).
   - **Live verification on `out/server-test`** (`bench_bosses.py --only ikiss --mode mixed_all --runs 2`, auto `--parallel 4`, `8/8` valid, `0` invalid, `0` wave errors, fresh instances `[1..4]` & `[5..8]`): `setup_s = 44.10 s`, Wave 1 pre-fight overhead **`1.05 s`** (`reset_s=0.39 s`, `stage_s=0.25 s`, `strat_s=0.21 s`, `connect_s=0.20 s`), Wave 2 **`1.06 s`** (`reset_s=0.41 s`, `stage_s=0.24 s`, `strat_s=0.21 s`, `connect_s=0.20 s`), `teardown_s = 1.10 s` — a **`~45×` reduction** vs `47.6 s` previously on Ikiss, cutting a 128-trial 4-boss run from `~70 min` to `~47 min` (`>90%` combat).

### Deliberately Skipped Proposals (Items 4, 5, and Adaptive Item 6)
- **Item 4 (abort fight after `20 s` of zero boss damage and mark `invalid`):** Skipped because real boss mechanics legitimately produce `>= 20 s` of zero or near-zero boss damage (`Kael'thas` `Gravity Lapse` `30 s`, `Ick` `Explosive Barrage` `18 s` + `Pursuit` `12 s`, `Eregos` `Planar Shift` `18 s`), so an in-flight `20 s` cutoff would abort valid pulls mid-mechanic. Worse, if an LLM bug causes bots to stop attacking (`STOP`, bad target switch, over-kiting out of range), aborting and re-rolling as `invalid` censors real `llm` failures and introduces survivorship bias. True staging failures without combat are already caught post-hoc by `no_pull` (`total_dps == 0.0 and total_deaths == 0 and not killed`).
- **Item 5 (reuse one `worldserver` across all bosses without DB reset / "gear once"):** Skipped because full `server_stop` + `test_databases_reset` + `server_start` + `gear` takes only `~44 s/boss` (`~3 min` across 4 bosses, `< 7%` of total runtime), whereas the 10-boss suite spans `L50`–`L80` and 5-man vs 10-man (`COMP_10` on `patchwerk`). Down-leveling bots in place without `test_databases_reset()` risks leaking higher-rank spells, `attune_quests`, and `role_items` across bosses, and `LlmBrain.TraceDir` is bound once at server startup in `scorer.sha256`-locked `llm_brain_world_script.cc`.
- **Adaptive Item 6 (drop `--runs` from 8 to 3 after 3 matching outcomes):** Skipped because early stopping at `n = 3` on a true `75%` arm has a **`42.2%` false-stopping rate** (`0.75^3 = 0.422` — misclassifying a `6/8` arm as `3/3` `"100%"` over two-fifths of the time) and produces unequal sample sizes across bosses in aggregate tables. Subset and run count remain explicit via `--only` and `--runs` chosen up front.

---

## 7. Saved Artifacts
- Raw summary JSON: `var/bench_boss_10_mixed_all.json` (untracked)
- Unified 5 Hz + event traces:
  - `var/bench_boss_kaelthas_mgt_mixed_all/bot/`
  - `var/bench_boss_ick_kite_mixed_all/bot/`
  - `var/bench_boss_murmur_boom_mixed_all/bot/`
  - `var/bench_boss_ikiss_blink_mixed_all/bot/`
