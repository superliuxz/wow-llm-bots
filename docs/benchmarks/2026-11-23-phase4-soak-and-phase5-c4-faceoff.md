# Phase 4 Live Soak & Phase 5 (`C4`) Model Face-Off Report (2026-11-23)

Commit tested: `19b3bbb` (`main`, `scorer.sha256` relocked & verified: `1a867dcdf2fd...`, all `30` measurement files locked).

---

## 1. Phase 4 Live Soak Validation (`B1–B3` + `world_knowledge.json`)

**Command:**
```bash
tools/wlb test soak --suite varied --minutes 20 --runs 3 --score \
  --human-dir ~/.local/share/wow-llm-bots/human_baseline \
  --verify-hash ~/.local/share/wow-llm-bots/scorer.sha256
python3 tools/bench/stall_check.py var/soak_traces
```
- **Scope & honest duration:** `3 × 20 min` = **`60 min` wall-clock (`10` bot-hours, `30` bot-runs)** across `SUITES["varied"]` (`soak_horde_rfc`, `soak_alliance_dm`, `soak_horde_rfc` — `SUITES["varied"]` has 2 scenarios, so `--runs 3` cycled `r00`, `r01`, `r02`). This is a 1-hour validation run, not a 4-hour endurance run.
- **Knowledge file:** `var/knowledge/world_knowledge.json` (`2.50 MiB`: `9,464` quests, `61` zones, `77` dungeons, `1,183` bosses), loaded by default by `harness run`.
- **Log:** `var/phase4_soak_2026-11-23.log`.

### Per-Run & Pooled Soak Metrics

| Run | Scenario | Decisions | `max_wakes/min/bot` | `do_p50` / `do_p95` | Errors / Invalid | XP/h | Deaths | Moved |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `r00` | `soak_horde_rfc` (20.0 min, 10 bots) | 747 | 11.4 | 129 / 334 ms | 0 / 0 | 3,146 | 2 | 10/10 |
| `r01` | `soak_alliance_dm` (20.0 min, 10 bots) | 700 | 8.1 | 137 / 352 ms | 0 / 0 | 6,848 | 3 | 10/10 |
| `r02` | `soak_horde_rfc` (20.0 min, 10 bots) | 739 | 7.3 | 147 / 345 ms | 0 / 0 | 3,536 | 2 | 10/10 |
| **Pooled** | **30 bot-runs (`10` bot-hours)** | **2,186** | **11.4** | **137 / 352 ms** | **0 / 0** | **4,476** | **7** | **30/30** |

### Layer A Guardrails & `stall_check.py`

- **Layer A gates (`11/12` PASS, `1` FAIL):**
  - **PASS:** `has_traced_bots` (`30 >= 15`), `has_decisions` (`2,186 >= 1`), `no_llm_errors` (`0`), `no_invalid_plans` (`0`), `no_wake_loop` (`11.4 <= 20.0`), `latency_p95` (`352 ms <= 2000 ms`), `not_stuck` (`0/30 <= 9`), `deaths_bounded` (`7 <= 30`), `no_refusal_loop` (`0 <= 5`), `human_whisper_timing` (`None`), `recording_parity` (`human=4.98 Hz, bot=4.98 Hz`).
  - **FAIL:** `human_react_timing` (`react_p10 = 146.0 ms` vs `>= 150 ms` floor — missed by `4 ms`).
- **Open-world questing (`Loada..Loade`, `15` bot-runs / `5` bot-hours):**
  - **`0/15` stalls** flagged by `stall_check.py` (`>= 300 s`).
  - **`80` quests accepted**, **`44` quest turn-ins** (`8.8 turn-ins/bot-hr` open-world; `4.4/bot-hr` pooled), **`29` level-ups**, **`582` mob kills**, **`840` loots**, **`263` item equips**, **`6` talent learns**, **`1` taxi learn**, **`17` gameobject interacts**.
  - **Cross-zone travel observed live:** `Loadc` traveled `Tirisfal Glades -> Silverpine Forest`; `Loade` (L15) traveled `Silverpine Forest -> Durotar -> The Barrens` and `Elwynn Forest -> Westfall`.
- **Dungeon-party followers (`Loadf..Loadj`, `15` bot-runs / `5` bot-hours) — open gap:**
  - **`8` stalls (`525.9 s – 1,078.0 s`)** flagged by `stall_check.py`: `Loadf_r02` (`525.9 s`), `Loadg_r00` (`589.5 s`), `Loadg_r01` (`593.3 s`), `Loadg_r02` (`1,072.4 s`), `Loadi_r00` (`1,028.2 s`), `Loadi_r01` (`830.6 s`), `Loadi_r02` (`1,078.0 s`), `Loadj_r01` (`771.4 s`).
  - Dungeon followers (especially healer `Loadg` and mage `Loadi`) remained parked in RFC / Deadmines when the group was not actively pulling — `304` dungeon mob kills and `119` combats occurred across the 3 runs, but followers stalled between/after pulls.

---

## 2. Phase 5 (`C4`) Model Choice Face-Off

All 5 candidate GGUFs in `models/` were evaluated on `tools/wlb test eval` (`52` scored cases, random baseline `22.0`), the top two were benchmarked head-to-head on `tools/wlb test perf brain` (with the play server idle/6 bots noted in the log), and the top challenger (`Qwen3.8-27B-IQ4_XS.gguf`) ran the full 4-arm `n = 16` (`320` valid trials, `0` invalid) 5-boss bench against the signed-off `gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf` baseline (`var/bench_boss_p12_signoff_v3_n16.json`).

### 2A. Offline Eval Shootout (`52` Scored Cases, `var/phase5_eval_shootout_2026-11-23.log`)

| Model | Size | Architecture | Score (`52` cases) | Standing Orders + Mined Boss (`8` cases) | `do:` p50 | Total p50 |
|---|---:|---|---:|---:|---:|---:|
| **`Qwen3.8-27B-IQ4_XS.gguf`** | 15G | 27B dense | **49/52 (94%)** | **8/8** | 153 ms | 418 ms |
| **`gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf`** *(default)* | 16G | 26B MoE (4B active) | **47/52 (90%)** | **8/8** | **68 ms** | **232 ms** |
| `Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf` | 21G | 35B MoE (3B active) | 37/52 (71%) | 1/8 | 63 ms | 149 ms |
| `gemma-4-E4B-it-qat-IQ4_XS-imatrix.gguf` | 4.7G | 4B dense QAT | 31/52 (60%) | — | 44 ms | 191 ms |
| `Qwen3-4B-Instruct-2507-UD-Q4_K_XL.gguf` | 2.4G | 4B dense | 28/52 (54%) | 4/8 | 53 ms | 205 ms |

`Qwen3.8-27B` missed only `red_mob_nearby`, `hp_60_fine`, and `new_loot_hover`.

### 2B. Throughput & 16-Slot Load (`tools/wlb test perf brain`, `var/phase5_perf_faceoff_2026-11-23.log`)

| Metric | `gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf` | `Qwen3.8-27B-IQ4_XS.gguf` | Ratio |
|---|---:|---:|---:|
| 16-slot capacity (`wake-ups/s`) | **9.6 / s** | 4.6 / s | **2.09× faster** |
| 16-slot call p50 / p95 | **1.68 s / 1.96 s** | 3.36 s / 4.48 s | 2.00× faster |
| 16-slot first-line p50 / p95 | **0.35 s / 0.54 s** | 0.88 s / 1.42 s | 2.51× faster |
| 50 bots / 30 s `wake->do` p50 / p95 | **0.27 s / 0.46 s** | 0.65 s / 1.40 s | 2.41× faster |
| 50 bots / 30 s `wake->plan` p50 / p95 (busy %) | **0.6 s / 0.9 s (41%)** | 1.5 s / 2.7 s (73%) | 2.50× faster |
| 100 bots / 15 s `wake->do` p50 / p95 (busy %) | **0.41 s / 0.71 s (98%)** | **21.69 s / 38.54 s (100%, saturated)** | **52.9× faster** |
| 100 bots / 15 s `wake->plan` p50 / p95 | **1.5 s / 2.0 s** | 24.3 s / 41.2 s | 16.2× faster |

### 2C. Live 5-Boss Benchmark (`n = 16`, 4 Arms, `320` Valid Trials, `0` Invalid)

Artifacts: `var/bench_boss_c4_qwen38_27b_n16.json` (`Qwen3.8-27B`, commit `19b3bbb`) vs `var/bench_boss_p12_signoff_v3_n16.json` (`gemma-4-26B-A4B-it`, commit `d5b8516`).

#### `Qwen3.8-27B-IQ4_XS.gguf` Within-Run Results (`m = 3` Pooled / `m = 15` Per-Boss Holm–Bonferroni)

| Boss | `llm` | `llm_no_notes` | `no_brain` | `baseline` | `llm` vs `no_brain` (`p_raw` / `p_holm`) |
|---|---:|---:|---:|---:|---|
| **Devourer of Souls** | 7/16 (43.8%, `[23.1–66.8%]`) | 6/16 (37.5%) | 4/16 (25.0%) | 6/16 (37.5%) | `0.4578` / `1.0000` |
| **Loken** | 14/16 (87.5%, `[64.0–96.5%]`) | 13/16 (81.2%) | 10/16 (62.5%) | 9/16 (56.2%) | `0.2200` / `1.0000` |
| **Kael'thas Sunstrider** | 9/16 (56.2%, `[33.2–76.9%]`) | 7/16 (43.8%) | 5/16 (31.2%) | 9/16 (56.2%) | `0.2852` / `1.0000` |
| **Murmur** | 8/16 (50.0%, `[28.0–72.0%]`) | 14/16 (87.5%) | 11/16 (68.8%) | 11/16 (68.8%) | `0.4725` / `1.0000` |
| **Talon King Ikiss** | **13/16 (81.2%, `[57.0–93.4%]`)** | 6/16 (37.5%) | 3/16 (18.8%) | 4/16 (25.0%) | **`0.0011` / `0.0164`** |
| **Pooled (`80`/arm)** | **51/80 (63.8%, `[52.8–73.4%]`)** | 46/80 (57.5%) | 33/80 (41.2%) | 39/80 (48.8%) | **`0.0069` / `0.0208`** |

Pooled `Qwen3.8-27B` `llm` vs `llm_no_notes`: `p_raw = 0.5176, p_holm3 = 0.5176`; vs `baseline`: `p_raw = 0.0793, p_holm3 = 0.1585`.

#### Direct Head-to-Head (`llm` Arm): `Qwen3.8-27B-IQ4_XS` vs `gemma-4-26B-A4B-it-UD-Q4_K_XL`

| Boss | `Qwen3.8-27B` `llm` | `Gemma-4-26B-A4B` `llm` | `Qwen3.8` Med DPS / `first_line_ms` | `Gemma4` Med DPS / `first_line_ms` | Exact Fisher `p_raw` (`p_holm5`) |
|---|---:|---:|---:|---:|---|
| **Devourer of Souls** | 7/16 (43.8%) | 7/16 (43.8%) | 4,842.0 / 803.0 ms | 4,572.1 / **182.9 ms** | `1.0000` (`1.0000`) |
| **Loken** | 14/16 (87.5%) | 14/16 (87.5%) | 5,234.5 / 552.5 ms | 5,654.0 / **168.4 ms** | `1.0000` (`1.0000`) |
| **Kael'thas Sunstrider** | 9/16 (56.2%) | 9/16 (56.2%) | 1,486.0 / 735.9 ms | 1,708.4 / **240.2 ms** | `1.0000` (`1.0000`) |
| **Murmur** | 8/16 (50.0%) | 10/16 (62.5%) | 2,239.7 / 970.0 ms | 2,485.8 / **249.2 ms** | `0.7224` (`1.0000`) |
| **Talon King Ikiss** | 13/16 (81.2%) | 12/16 (75.0%) | 1,129.2 / 627.4 ms | 1,016.8 / **169.7 ms** | `1.0000` (`1.0000`) |
| **Pooled `llm` (`80`)** | **51/80 (63.8%)** | **52/80 (65.0%)** | — | — | **`1.0000`** |
| **Pooled `llm_no_notes` (`80`)** | 46/80 (57.5%) | 43/80 (53.8%) | — | — | `0.7504` |

*(Non-LLM reference arms were also stable across both runs: `no_brain` `33/80` vs `35/80` `p_raw = 0.8730`; `baseline` `39/80` vs `40/80` `p_raw = 1.0000`.)*

---

## 3. Verdict & Next Steps

1. **Keep `gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf` as the default model (`C4` settled with data):**
   - Although `Qwen3.8-27B-IQ4_XS.gguf` edges out Gemma-4-26B-A4B on offline eval (`49/52` vs `47/52`), both models hit the **exact same live boss-kill ceiling** (`51/80` `63.8%` vs `52/80` `65.0%`, `p = 1.0000`; identical kills on Devourer, Loken, and Kael'thas).
   - Meanwhile, `gemma-4-26B-A4B-it` is **`2.1×` higher 16-slot throughput**, **`3.1×–4.4×` faster in live boss combat** (`168–249 ms` vs `552–970 ms`), and handles **100 bots cleanly** where 27B dense saturates (`21.69 s` `wake->do` p50).
2. **Phase 4 follow-ups surfaced by the live soak:**
   - **Dungeon-party follower stalls (`8/15` in RFC/DM):** open-world questing had **`0/15` stalls** and `44` quest turn-ins, so Phase 4 `B1–B3` works cleanly in open-world play, while dungeon-party follower coordination needs investigation before signing off a 4-hour mixed soak.
   - **`human_react_timing` (`146.0 ms` vs `>= 150 ms`):** only `4 ms` below the Layer A guardrail floor.
