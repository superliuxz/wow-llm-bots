# Four-Boss × Four-Arm Parallel Benchmark Report (`trace-all-events`)

**Date:** `2026-11-20` – `2026-11-21` (branch `trace-all-events`, `main` untouched at `2834756`)  
**Runner:** [`tools/bench/bench_bosses.py`](../../tools/bench/bench_bosses.py) (`--mode mixed_all --runs 8 --parallel 4`)  
**Total valid scored trials:** **128** (`4` bosses × `4` arms × `8` valid runs per arm; **`0` invalid trials**, **`0` wave errors**)

---

## 1. Experimental Setup & Hygiene

Every boss ran in `mixed_all` mode across 4 simultaneous 5-man parties (`--parallel 4`) inside a single persistent test `worldserver` (`out/server-test`) with identical hardware/server contention:

- **Party composition (`COMP_5`):** Protection Warrior (`tank`, `spec_tab=2`), Holy Priest (`heal`, `spec_tab=1`), Frost Mage (`dps`, `spec_tab=2`), Destruction Warlock (`dps`, `spec_tab=2`), Marksmanship Hunter (`dps`, `spec_tab=1`), geared to boss level in `ITEM_QUALITY_EPIC` via `PlayerbotFactory::Randomize` + `InitTalentsBySpecNo`.
- **Four arms rotated every wave** via `_wave_arms` (two-cycle Latin rotation so every arm ran in every party slot `Load..Pfou` twice across 8 waves):
  - **`llm`**: `harness run` + `llama-server` (`gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf`, `--slots 16 --temperature 0`) with boss `notes.md` (standing orders + tactics); playerbots hardcoded boss strategy stripped via `llmtest setstrat ... -<strip_strat>`.
  - **`llm_no_notes`**: `harness run` + `llama-server` **without** `notes.md`; playerbots boss strategy stripped.
  - **`no_brain`**: pure playerbots (excluded from `--only-bots`), playerbots boss strategy stripped.
  - **`baseline`**: pure playerbots (excluded from `--only-bots`), playerbots hardcoded C++ boss strategy **ON** (`+<strip_strat>`).
- **Note on Murmur (`Shadow Labyrinth`, `map=555`):** `strip_strat=""` because `mod-playerbots` has no dedicated Murmur/Shadow Labyrinth strategy. On Murmur, `no_brain` and `baseline` therefore act as two independent 8-trial control samples of unscripted playerbots AI.
- **Per-trial verification & isolation:** Verified `role=` and stored `specNo=` at setup; between waves reset on `map=0` via `llmtest resetbot`, re-verified party grouping via `llmtest group`, staged at dungeon entrances (`areatrigger_teleport`) before starting `harness`, verified `alive=1` + distinct never-reused `instance_id`s, verified per-bot `llmtest checkstrat`, moved to `spawn_xyz` immediately once `harness` connected (`0` pre-pull combat, `0` zero-pull trials), and sliced traces per party `[pull_ms - 1000, pull_ms + duration_s*1000 + 3000]` matching each trial's `instance_id`.
- **Execution provenance:** `kaelthas` and `ick` completed cleanly at `523ab4c` (`32/32` valid each, `0` invalid, `0` wave errors). Before completing `murmur` and `ikiss` with `--append`, three multi-wave test-harness bugs exposed during long waves were fixed in [`test_commands.cc`](../../server_module/src/test/test_commands.cc) and [`bench_bosses.py`](../../tools/bench/bench_bosses.py) (with **no** changes to the LLM brain, snapshot, options, or scorer):
  1. Strip `SPELL_AURA_SPIRIT_OF_REDEMPTION` before resurrecting in `HandleResetBot` (removing `27827` while alive triggers `setDeathState(DeathState::JustDied)`).
  2. Reset playerbots' `"death count"` and random-bot `"dead"`/`"revive"` timers in `HandleResetBot` (plus re-ensure party grouping before each wave reset) so bots hitting `death count >= 5` cannot invoke `RandomPlayerbotMgr::Revive` → `LeaveOrDisbandGroup()`.
  3. Replace the 3.0 s entrance sleep and 0.2 s per-bot move delay with `_wait_harness_connected` (`~0.2 s` total) + `0.05 s` move delay (clearing non-combat target/attack on move) so LLM bots do not engage entrance trash before moving to `spawn_xyz`.

---

## 2. Executive Summary (Kills / 8 Valid Trials)

| Boss (Instance, Level, `strip_strat`) | `llm` | `llm_no_notes` | `no_brain` | `baseline` |
|---|---:|---:|---:|---:|
| **Kael'thas Sunstrider** (`585`, L70, `tbc-mgt`) | 6/8 (75.0%) | 6/8 (75.0%) | 6/8 (75.0%) | **7/8 (87.5%)** |
| **Ick** (`658`, L80, `wotlk-pos`) | **8/8 (100.0%)** | **8/8 (100.0%)** | **8/8 (100.0%)** | **8/8 (100.0%)** |
| **Murmur** (`555`, L70, *(none)*) | 6/8 (75.0%) | **8/8 (100.0%)** | 7/8 (87.5%) | **8/8 (100.0%)** |
| **Talon King Ikiss** (`556`, L68, `tbc-seth`) | **6/8 (75.0%)** | **6/8 (75.0%)** | 3/8 (37.5%) | 2/8 (25.0%) |
| **Total across all 128 valid trials** | **26/32 (81.2%)** | **28/32 (87.5%)** | **24/32 (75.0%)** | **25/32 (78.1%)** |

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
Note on `act_delay_ms`: `TraceSample` runs at ~`5 Hz` (`200 ms`), whereas `act_sent` is logged on the exact world tick when a standing order fires, so `act_delay_ms` relative to the first sampled `target_casting` row `s0` can be slightly negative (`-76.5 ms` / `-10.0 ms`) when the order dispatched between samples.

| Boss & Spell (DBC Radius) | Arm | Cast Windows | Safe at Impact | Med Impact Dist | Med `act_delay_ms` | Notes |
|---|---|---:|---:|---:|---:|---|
| **Ick** — `Poison Nova` (`15.0 yd`) | `llm` | 1 | 1/5 (20.0%) | 9.9 yd | 2,402.0 ms (2) | Most fights stayed `Poison Nova`-sparse; `llm` also dispatched 120 `AVOID Toxic Waste` acts |
| | `llm_no_notes` | 2 | 2/10 (20.0%) | 10.2 yd | — | — |
| | `no_brain` | 0 | — | — | — | — |
| | `baseline` | 5 | **25/25 (100.0%)** | **31.5 yd** | — | Hardcoded `wotlk-pos` flee script |
| **Murmur** — `Sonic Boom` (no DBC radius*; `34.0 yd` per notes) | `llm` | 9 | **17/40 (42.5%)** | **26.4 yd** (p75 `43.2`, max `58.0`) | -76.5 ms (26) | Cut median Sonic Boom impact dmg to **5,323** (`mean 5,193`) |
| | `llm_no_notes` | 8 | 1/40 (2.5%) | 7.7 yd (p75 `18.2`, max `37.7`) | — | Med impact dmg **7,894** (`mean 8,787`) |
| | `no_brain` | 8 | 0/40 (0.0%) | 7.6 yd (p75 `15.4`, max `31.8`) | — | Med impact dmg **7,780** (`mean 8,694`) |
| | `baseline` | 8 | 0/40 (0.0%) | 7.7 yd (p75 `18.1`, max `28.5`) | — | Med impact dmg **7,769** (`mean 9,092`) |
| **Ikiss** — `Arcane Explosion` (`35.0 yd`) | `llm` | 23 | **12/110 (10.9%)** | 10.2 yd (max `42.8`) | -10.0 ms (48) | Hunter (`5/21`, med `20.5 yd`), Tank (`4/23`, `7.1 yd`), Priest (`3/22`, `16.3 yd`) |
| | `llm_no_notes` | 18 | 0/87 (0.0%) | 7.2 yd (max `31.9`) | — | — |
| | `no_brain` | 15 | 0/68 (0.0%) | 7.1 yd (max `22.6`) | — | — |
| | `baseline` | 17 | 0/65 (0.0%) | 18.5 yd (max `32.9`) | — | Tank ran to med `20.9 yd` under `tbc-seth`, pulling room trash |

\*`Sonic Boom` emits `target_cast_radius = -1.0` in `TraceSample` (`CalcRadius()` on the self-cast entry returns `0`); the `>= 34.0 yd` column above evaluates Murmur's `34 yd` blast radius using `--radius 34` in `dodge_timeline.py`.  
On **Kael'thas**, `Flame Strike` is cast by `Flame Strike Trigger (Kael - 5Man)` rather than on Kael'thas's own `target_casting` bar (`0` `Flame Strike` boss-cast windows matched by `target_casting`), so `Flame Strike` is instead reflected in total `Flame Strike` damage taken across the 8 trials per arm: **`845`** (`baseline` `tbc-mgt`), **`80,253`** (`llm_no_notes`), **`84,834`** (`llm`), and **`93,652`** (`no_brain`). Party `Phoenix Egg` kill credits were steady across all four arms (`37–41` over 8 trials).

---

## 5. Key Takeaways & Honest Caveats

1. **LLM arms doubled `no_brain` and tripled `baseline` kill rates on Talon King Ikiss (`75.0%` vs `37.5%` and `25.0%`):**
   - `baseline` (`tbc-seth`) moved the whole party away from Ikiss to a median `18.5 yd` (`tank` `20.9 yd`), which body-pulled the nearby `-x` Sethekk room trash (`Sethekk Shaman`, `Sethekk Prophet`, `Sethekk Talon Lord`, `Avian Warhawk`, `Time-Lost Shadowmage`, 49–65 yd from `spawn_xyz`) in **7/8 trials** and **6/6 wipes**, depressing `baseline` throughput (`731.3` DPS, `564.9` HPS).
   - `llm` pulled trash in **2/8 trials** (its only `2/2` wipes) when `AWAY Talon King Ikiss` moved near those mobs, while reaching full `>=35 yd` clearance on `12/110` exposures. Even **`llm_no_notes`** matched `llm`'s `6/8` kill rate with **0/8** trash pulls and fewer tank deaths (`2` vs `no_brain`'s `6`) alongside higher healer output (`773.0` vs `648.2` HPS).
2. **Standing-order dodge verified live on Murmur (`Sonic Boom`) — with a real DPS/aggro tradeoff:**
   - Only `llm` moved out during `Sonic Boom` (`17/40 >= 34 yd`, median impact distance `26.4 yd` vs `7.6–7.7 yd`, cutting median `Sonic Boom` impact damage by **~32%** from `7,769–7,894` to `5,323`), responding almost immediately upon cast start (`-76.5 ms` median vs `5 Hz` `s0`).
   - However, running 38 yd away in epic L70 gear lowered party DPS (`2,845.1` / duration `58.4 s` vs `4,346.1` / `39.1 s` for pure tank-and-spank `no_brain`), and both LLM arms engaged deeper room trash (`Cabal Summoner` / `Cabal Executioner`, 54–81 yd north of `spawn_xyz`) more often (`6/8` `llm`, `5/8` `llm_no_notes`, `0/8` `no_brain` / `baseline`), resulting in `6/8` kills on `llm` vs `8/8` on `llm_no_notes` and `baseline`.
3. **Kael'thas & Ick show where playerbots' C++ mechanics still lead vs parity:**
   - **Ick (`8/8` all arms):** L80 epic five-mans cleared comfortably in ~80–83 s (`0.0` median deaths); `baseline` (`wotlk-pos`) cleanly dodged `25/25` `Poison Nova` casts (`31.5 yd`), while `llm` actively dispatched `AVOID Toxic Waste` (`120`), `AWAY`/`AWAY Ick` (`18`), and `KITE Ick` (`5`).
   - **Kael'thas (`7/8` `baseline` vs `6/8` others):** `baseline` (`tbc-mgt`) avoided almost all `Flame Strike` damage (`845` total vs `80,253–93,652` in the non-scripted arms) and finished with `0.0` median deaths; across all 4 arms, `Arcane Sphere` during `Gravity Lapse` caused the majority of deaths (`50` total across the 32 trials).
4. **Room trash caveat & live environmental death observation:**
   - Every boss's `spawn_xyz` has nearby dungeon trash (`kaelthas`: 6-mob Sunblade/Coilskar pack at `34–41 yd`; `ick`: Plagueborn Horrors at `21–25 yd`; `murmur`: 3 Cabal Spellbinders at `28–30 yd`; `ikiss`: Sethekk packs at `49–65 yd`), affecting all 4 arms equally per pull.
   - One live environmental death (`event="died"`, `detail="environment"`) was recorded in `murmur` when `Pfoua` was teleported to map 0 right after a fight while `Murmur's Touch` (`33760`) expired and triggered `Shockwave` (`33686`) knockback/fall damage (`3,579`).

---

## 6. Saved Artifacts
- Raw summary JSON: `var/bench_boss_10_mixed_all.json` (untracked)
- Unified 5 Hz + event traces:
  - `var/bench_boss_kaelthas_mgt_mixed_all/bot/`
  - `var/bench_boss_ick_kite_mixed_all/bot/`
  - `var/bench_boss_murmur_boom_mixed_all/bot/`
  - `var/bench_boss_ikiss_blink_mixed_all/bot/`
