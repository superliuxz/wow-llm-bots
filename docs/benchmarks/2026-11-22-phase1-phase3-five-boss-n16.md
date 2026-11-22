# 5-Boss Benchmark Sign-Off (`n = 16` per arm, 320 valid trials, single codebase)

**Date:** 2026-11-22
**Codebase:** `d5b8516` (after `A6` mechanical potion lockout + `skipAutorepeat=true` and generic periodic-trigger hazard inspection / `nearby.areas` screen rendering).
**Command:** `python3 tools/bench/bench_bosses.py --only devourer,loken,murmur,ikiss,kaelthas --runs 16 --parallel 4 --mode mixed_all --out var/bench_boss_p12_signoff_v3_n16.json`
**Artifact:** `var/bench_boss_p12_signoff_v3_n16.json` (`320` valid trials, `0` invalid trials, `0` pre-pull-combat trials, timestamps `2026-11-22 15:35:13`–`18:00:44` after `d5b8516`).

> [!IMPORTANT]
> **Supersedes `docs/benchmarks/2026-11-21-phase1-phase2-five-boss.md` (`n = 8`).** On a single codebase with `n = 16` per arm (`n = 80` pooled per arm), `llm` achieved **`52/80` (65.0%)** kills, significantly beating `no_brain` (`35/80`, `43.8%`; exact two-sided Fisher `p_raw = 0.0109`, **`p_holm3 = 0.0326`** across the 3 pooled arm comparisons). Per-boss Holm–Bonferroni across all `m = 15` (`5 bosses × 3 arms`) comparisons is significant on **Ikiss** vs `no_brain` (`12/16` vs `2/16`, `p_raw = 0.0010`, **`p_holm15 = 0.0135`**; `p_holm5 = 0.0048`) and vs `baseline` (`12/16` vs `1/16`, `p_raw = 0.0002`, **`p_holm15 = 0.0026`**).

---

## 1. Kills First (Per Boss & Pooled, `n = 16` per arm)

| Boss | `llm` (notes) | `llm_no_notes` | `no_brain` | `baseline` (PB scripts) | `llm` vs `llm_no_notes` (`p_raw` / `p_holm15`) | `llm` vs `no_brain` (`p_raw` / `p_holm15`) | `llm` vs `baseline` (`p_raw` / `p_holm15`) |
|---|---:|---:|---:|---:|---:|---:|---:|
| **Devourer** (`devourer`) | `7/16` (43.8%) | `5/16` (31.2%) | `7/16` (43.8%) | `6/16` (37.5%) | `0.7160` / `1.0000` | `1.0000` / `1.0000` | `1.0000` / `1.0000` |
| **Loken** (`loken`) | `14/16` (87.5%) | `14/16` (87.5%) | `14/16` (87.5%) | `9/16` (56.2%) | `1.0000` / `1.0000` | `1.0000` / `1.0000` | `0.1134` / `1.0000` |
| **Murmur** (`murmur`) | `10/16` (62.5%) | `14/16` (87.5%) | `9/16` (56.2%) | `12/16` (75.0%) | `0.2200` / `1.0000` | `1.0000` / `1.0000` | `0.7043` / `1.0000` |
| **Ikiss** (`ikiss`) | **`12/16` (75.0%)** | `6/16` (37.5%) | `2/16` (12.5%) | `1/16` (6.2%) | `0.0732` / `0.8781` | **`0.0010` / `0.0135`** | **`0.0002` / `0.0026`** |
| **Kael'thas** (`kaelthas`) | `9/16` (56.2%) | `4/16` (25.0%) | `3/16` (18.8%) | `12/16` (75.0%) | `0.1489` / `1.0000` | `0.0659` / `0.8566` | `0.4578` / `1.0000` |
| **Pooled (`80` trials/arm)** | **`52/80` (65.0%)** | **`43/80` (53.8%)** | **`35/80` (43.8%)** | **`40/80` (50.0%)** | `0.1977` / `0.1977` (`m=3`) | **`0.0109` / `0.0326` (`m=3`)** | `0.0782` / `0.1564` (`m=3`) |

*Exact two-sided Fisher tests. Per-boss `p_holm15` applies Holm–Bonferroni across all `m = 15` (`5 bosses × 3 arms`) comparisons; against `no_brain` only (`m = 5`), Ikiss is `p_holm5 = 0.0048` and Kael'thas is `p_holm5 = 0.2636`. Pooled row applies Holm–Bonferroni across `m = 3` arm comparisons. Note: this is the 3rd Phase 1–2 sign-off 5-boss evaluation (`v1`/`v2`/`v3`; 6th 4-arm 5-boss `mixed_all` run overall in `var/`) after iterative bug fixes (`A6` potion lockout, generic trigger-hazard perception).*

---

## 2. Party Median DPS & Median Deaths (Per Boss & Arm)

| Boss | `llm` DPS (vs `llm_no_notes`) | `llm_no_notes` DPS | `no_brain` DPS | `baseline` DPS | Median Deaths (`llm` / `no_notes` / `no_brain` / `base`) |
|---|---:|---:|---:|---:|---:|
| **Devourer** | `4,572.1` (`+5.3%`) | `4,343.8` | `4,794.2` | `4,421.8` | `4.0` / `5.0` / `5.0` / `5.0` |
| **Loken** | `5,654.0` (`-2.7%`) | `5,810.1` | `5,762.6` | `4,286.6` | **`0.0`** / `1.0` / `1.0` / `1.5` |
| **Murmur** | `2,485.8` (`-3.0%`) | `2,563.6` | `3,135.8` | `3,067.8` | `1.0` / `1.0` / `3.0` / `2.0` |
| **Ikiss** | `1,016.8` (`-2.7%`) | `1,044.7` | `1,029.2` | `612.9` | `3.0` / `5.0` / `5.0` / `5.0` |
| **Kael'thas** | `1,708.4` (`-20.2%`) | `2,142.1` | `1,512.4` | `1,548.0` | `2.5` / `5.0` / `5.0` / `0.5` |

---

## 3. Honest Takeaways & Waivers

1. **Where the `+17` kill edge over `no_brain` comes from:** Almost entirely from mechanics-heavy **Ikiss** (`12/16` vs `2/16`, `+10`, `p_holm15 = 0.0135`) and **Kael'thas** (`9/16` vs `3/16`, `+6`, `p_raw = 0.0659`). On the other 3 bosses combined, Playerbots' core combat AI already clears at high rates (`llm` `31/48` vs `no_brain` `30/48`), though `llm` achieves `0.0` median deaths on Loken and `1.0` on Murmur.
2. **Non-significant pooled comparisons:** `llm` (`52/80`) numerically leads `llm_no_notes` (`43/80`, `p_holm3 = 0.1977`) and hand-written `baseline` (`40/80`, `p_holm3 = 0.1564`), but those two pooled differences are not statistically significant at `n = 80`.
3. **Kael'thas DPS waiver (`-20.2%` all-trial, `-15.9%` kill-only `1,983` vs `2,357`):** Four of five bosses are within `3%` of `llm_no_notes` (`+5.3%`, `-2.7%`, `-3.0%`, `-2.7%`). On Kael'thas, both `llm` (`1,708` DPS, `9/16` kills) and hand-written `baseline` (`1,548` DPS, `12/16` kills) trade turret DPS for dodging `Arcane Sphere` / `Flame Strike` and switching to `Phoenix Egg` — lifting kills from `3–4/16` to `9–12/16`. We accept this mechanical movement trade-off as an explicit waiver on the `<10%` DPS criterion for Kael'thas.
4. **Murmur variance:** `llm` (`10/16`) trailed `llm_no_notes` (`14/16`, `p_raw = 0.2200`) and `baseline` (`12/16`, `p_raw = 0.7043`) while beating `no_brain` (`9/16`, `1.0` vs `3.0` med deaths).
