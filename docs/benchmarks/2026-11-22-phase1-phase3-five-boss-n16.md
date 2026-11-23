# 5-Boss Benchmark Evaluation (`v1`–`v3`, Pending User Sign-Off)

**Date:** 2026-11-22
**Status:** **Pending user sign-off decision** (not self-signed-off).
**Latest run (`v3`) codebase:** `d5b8516` (after `A6` mechanical potion lockout + `skipAutorepeat=true` and `3b5cae8` generic periodic-trigger hazard inspection / `nearby.areas` screen rendering + Kael'thas `Arcane Sphere` standing order).
**Command (`v3`):** `python3 tools/bench/bench_bosses.py --only devourer,loken,murmur,ikiss,kaelthas --runs 16 --parallel 4 --mode mixed_all --out var/bench_boss_p12_signoff_v3_n16.json`
**Artifacts:**
- `v1` (`7393849`, `n = 8`/boss): `var/bench_boss_p12_signoff.json` (`160` valid trials)
- `v2` (`c3a6e0a`, `n = 8`/boss): `var/bench_boss_p12_signoff_v2.json` (`160` valid trials)
- `v3` (`d5b8516`, `n = 16`/boss): `var/bench_boss_p12_signoff_v3_n16.json` (`320` valid trials, `0` invalid, `0` pre-pull-combat, timestamps `2026-11-22 15:35:13`–`18:00:44`)

> [!IMPORTANT]
> **Full three-run history & disclosure:** `v3` was the **third** 5-boss sign-off attempt after iterative fixes between runs (`3b5cae8` added the Kael'thas `Arcane Sphere` standing order between `v2` and `v3`). The pooled comparison was **not** pre-declared before running `v3`. Across all three runs combined (`160` trials/arm), `llm` achieved **`96/160` (60.0%)** vs `no_brain` **`76/160` (47.5%)** (exact two-sided Fisher `p = 0.0330`), vs `llm_no_notes` `90/160` (`56.2%`, `p = 0.5711`), and vs `baseline` `90/160` (`56.2%`, `p = 0.5711`). In `v3` alone (`n = 16`/boss), `llm` achieved **`52/80` (65.0%)** vs `no_brain` `35/80` (`43.8%`, `p_raw = 0.0109`, `p_holm3 = 0.0326`), with Ikiss (`+10`) and Kael'thas (`+6`) accounting for `+16` of the `+17` kill edge. **Whether to accept Kael'thas's `-20.2%` DPS trade-off and sign off Phases 1–3 is the user's decision.**

---

## 0. All Three Sign-Off Runs & Combined Totals

| Run | Codebase | `n`/boss | `llm` (notes) | `llm_no_notes` | `no_brain` | `baseline` | `llm` vs `no_brain` (2-sided Fisher `p`) |
|---|---|---:|---:|---:|---:|---:|---:|
| **`v1`** (`bench_boss_p12_signoff.json`) | `7393849` | `8` | `23/40` (57.5%) | `24/40` (60.0%) | `22/40` (55.0%) | `26/40` (65.0%) | `1.0000` |
| **`v2`** (`bench_boss_p12_signoff_v2.json`) | `c3a6e0a` | `8` | `21/40` (52.5%) | `23/40` (57.5%) | `19/40` (47.5%) | `24/40` (60.0%) | `0.8233` |
| **`v3`** (`bench_boss_p12_signoff_v3_n16.json`) | `d5b8516` | `16` | **`52/80` (65.0%)** | `43/80` (53.8%) | `35/80` (43.8%) | `40/80` (50.0%) | `0.0109` (`p_holm3 = 0.0326`) |
| **Combined (`v1 + v2 + v3`)** | — | `32` | **`96/160` (60.0%)** | `90/160` (56.2%) | `76/160` (47.5%) | `90/160` (56.2%) | **`0.0330`** (`p = 0.5711` vs `llm_no_notes` / `baseline`) |

---

## 1. `v3` Kills First (Per Boss & Pooled, `n = 16` per arm)

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
3. **Kael'thas DPS trade-off (`-20.2%` all-trial, `-15.9%` kill-only `1,983` vs `2,357` — pending user decision):** Four of five bosses are within `3%` of `llm_no_notes` (`+5.3%`, `-2.7%`, `-3.0%`, `-2.7%`). On Kael'thas, both `llm` (`1,708` DPS, `9/16` kills) and hand-written `baseline` (`1,548` DPS, `12/16` kills) trade turret DPS for dodging `Arcane Sphere` / `Flame Strike` and switching to `Phoenix Egg` — lifting kills from `3–4/16` to `9–12/16`. Whether to grant a waiver on the `<10%` DPS criterion for Kael'thas is left to the user.
4. **Murmur variance:** `llm` (`10/16`) trailed `llm_no_notes` (`14/16`, `p_raw = 0.2200`) and `baseline` (`12/16`, `p_raw = 0.7043`) while beating `no_brain` (`9/16`, `1.0` vs `3.0` med deaths).
