# Measured Results

Current benchmark and soak numbers only (older phase logs live in `git log`). Every row lists
its commit, sample size `n`, and main caveats so claims never outrun the data.

## Core Runtime & Throughput

| Benchmark | Commit | `n` | Result | Caveats |
|---|---|---:|---|---|
| **World-thread snapshot pressure** (`perf snapshots`, 50 bots in Orgrimmar, 1 s interval) | `17 Nov` (`perf` baseline) | 50 bots, 50 fps | Build p50 **83 µs**, p99 **175 µs**, max **264 µs** (`264 ms/min` total), 0 dropped, avg 23.2 KB | Measured idle standing in city, not 25-man raid combat at 4 Hz. |
| **Brain load & capacity** (`perf brain`, `gemma-4-26B-A4B-it-UD-Q4_K_XL`, 16 shared slots, RTX 5090) | `19b3bbb` | 16-slot sat + 50/100-bot replay | **9.6 wakes/s** capacity; 50 bots / 30 s wake→`do:` p50 **0.27 s** / p95 **0.46 s**; 100 bots / 15 s wake→`do:` p50 **0.41 s** / p95 **0.71 s** | Replayed saved snapshots, not live dynamic combat; RTX 5090 single-GPU numbers. |
| **Live 2-bot decision latency** (`Testpriest` + `Gatda`, `gemma-4-26B-A4B-it`) | `17 Nov` | `n = 86` decisions | `do:` p50 **110 ms** (p95 **190 ms**), full plan p50 **247 ms** (p95 **346 ms**), `age_at_apply` p50 **94–115 ms** | 2 bots only (unloaded GPU). |

## Offline Eval & Model Comparison

| Model (GGUF on RTX 5090) | Commit | `n` | Offline Eval | 16-slot Capacity | Caveats |
|---|---|---:|---:|---:|---|
| **`gemma-4-26B-A4B-it-UD-Q4_K_XL`** *(default)* | `19b3bbb` | 52 cases | **47/52 (90%)**, `do:` p50 68 ms | **9.6 wakes/s** (2.1× faster) | `RULES.md` was tuned against these same 52 cases at `temp = 0` (live play uses `0.7`); no held-out split; `decide.cc` omitted memory/relationships until `dc31cec`. |
| `Qwen3.8-27B-IQ4_XS` | `19b3bbb` | 52 cases | **49/52 (94%)**, `do:` p50 153 ms | 4.6 wakes/s (saturates at 100 bots) | Same eval-overfitting caveats; 2× slower throughput and 53× worse tail latency at 100 bots. |
| `Qwen3.6-35B-A3B-UD-Q4_K_XL` | `19b3bbb` | 52 cases | 37/52 (71%) | 11.3 wakes/s (on older prompt) | Weak on standing-order cases (1/8). |
| `gemma-4-E4B-it-qat` / `Qwen3-4B` | `19b3bbb` | 52 cases | 31/52 (60%) / 28/52 (54%) | — | Too weak for reliable multi-step plans. |

## Live 5-Boss Benchmark (`bench_bosses.py`, `mixed_all` 4 Arms)

Bosses: Devourer of Souls, Loken, Murmur, Talon King Ikiss, Kael'thas (Magisters' Terrace).

| Run & Model | Commit | `n` / boss (total/arm) | `llm` (notes) | `llm_no_notes` | `no_brain` | `baseline` (PB C++ scripts) | Caveats |
|---|---|---:|---:|---:|---:|---:|---|
| **`gemma-4` `v3`** | `d5b8516` | 16 (80) | **52/80 (65.0%)** | 43/80 (53.8%) | 35/80 (43.8%) | 40/80 (50.0%) | 3rd sign-off attempt after iterative fixes; `llm` vs `no_brain` `p_holm3 = 0.033`, driven by Ikiss (+10) & Kael'thas (+6); vs `llm_no_notes`/`baseline` n.s. (`p > 0.15`). |
| **`gemma-4` combined (`v1+v2+v3`)** | `7393849`–`d5b8516` | 32 (160) | **96/160 (60.0%)** | 90/160 (56.2%) | 76/160 (47.5%) | 90/160 (56.2%) | Pooled `llm` vs `no_brain` `p_raw = 0.033`, **`p_holm3 = 0.099` (n.s.)**. |
| **`Qwen3.8-27B`** | `19b3bbb` | 16 (80) | **51/80 (63.8%)** | 44/80 (55.0%) | 37/80 (46.2%) | 42/80 (52.5%) | Compared against `gemma-4` `v3` from a *different commit* (`d5b8516`); at `n = 80`, the 95% CI on a difference between two arms is roughly **±15 percentage points** (51/80 vs 52/80 is noise, not proof of equivalence). |

## Live Soaks (`wlb test soak`)

| Run | Commit | `n` (bot-hours) | Decisions | Errors / Invalid | Open-World Stalls (`>= 300 s`) | Dungeon Stalls (`>= 300 s`) | Caveats |
|---|---|---:|---:|---:|---:|---:|---|
| **1 h Varied soak** (`RFC` + `Deadmines`) | `19b3bbb` | 30 bot-runs (10 bh) | 2,186 | 0 / 0 | 0 / 15 | 8 spans (526–1,078 s) | Old `not_stuck` check in `soak_scorer.py` only tested distance from first sample (fixed in `eef8b62`); XP/h undercounted across level-ups (fixed in `eef8b62`). |
| **4 h Endurance soak** (6 Classic/TBC/WotLK runs) | `b7bf727` + `4b6be66` | 60 bot-runs (40 bh) | 7,432 | 0 / 0 | 0 / 30 | 38 spans (up to 1,793 s) | Split-binary run (`worldserver` rebuilt between `r00` and `r01`); MgT and Ahn'kahet had 0 inside-dungeon kills (long empty entrance corridors); `llmtest additem` typo dropped profession tools (fixed later); old `not_stuck` falsely reported `0/60`. |

## Retired / Parked Experiments

- **Automated boss-notes loop (`experimental/notes_hill_climb/`):** Parked — its significance test counted screening trials (winner's curse). Edit `content/notes/<Dungeon>/notes.md` by hand.
- **Human-likeness scorer (`experimental/human_likeness/`):** Parked — live human-play verification takes priority over automated realism scores.
