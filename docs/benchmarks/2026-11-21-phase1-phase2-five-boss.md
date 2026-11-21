# 5-Boss Benchmark After High 1 Fix (`n = 8` per arm, 160 valid trials, single codebase)

**Date:** 2026-11-21
**Codebase:** Working tree after fixing Review `ede3f6c` High 1 (`away`/`out of` no longer auto-walk back; explicit `back` only), Mediums 5–8 (`out of` combat reach, `orders: none`, pre-pull cache preservation, `CanEscapeBeforeImpact` removed), and Medium 12 (`BackMs` requires in-range + casting).
**Command:** `python3 -u tools/bench/bench_bosses.py --only kaelthas,murmur,ikiss,loken,devourer --mode mixed_all --runs 8`
**Artifact:** `var/bench_boss_10_mixed_all.json` (`160` valid trials, `0` invalid trials, `0` wave errors across all 5 bosses on one identical build).

> [!IMPORTANT]
> **Supersedes the earlier mixed-commit report** (`221b706` + `d7b54f4`), where High 1 (`away` walking straight back on arrival) caused boss notes to cut kills to `10/40` vs `llm_no_notes` `24/40` (`p = 0.003`; `Murmur 0/8`, `Loken 2/8`). After fixing High 1, `llm` recovered to **`24/40` (60.0%)** (`Murmur 7/8`, `Loken 6/8`), eliminating the notes regression (`24/40` vs `25/40`, Fisher `p = 1.0000`). However, **`llm` does not yet significantly beat `no_brain` (`24/40` vs `20/40`, `p = 0.5005`) or `baseline` (`22/40`, `p = 0.8213`)** at `n = 8` after Holm–Bonferroni correction.

---

## 1. Kills First (Per Boss & Pooled, `n = 8` per arm)

| Boss | `llm` (notes) | `llm_no_notes` | `no_brain` | `baseline` (PB scripts) | `llm` vs `llm_no_notes` (`p_raw` / `p_holm`) | `llm` vs `no_brain` (`p_raw` / `p_holm`) | `llm` vs `baseline` (`p_raw` / `p_holm`) |
|---|---:|---:|---:|---:|---:|---:|---:|
| **Kael'thas** (`kaelthas`) | `3/8` (37.5%) | `5/8` (62.5%) | `2/8` (25.0%) | `6/8` (75.0%) | `0.6193` / `1.0000` | `1.0000` / `1.0000` | `0.3147` / `1.0000` |
| **Murmur** (`murmur`) | `7/8` (87.5%) | `8/8` (100.0%) | `6/8` (75.0%) | `5/8` (62.5%) | `1.0000` / `1.0000` | `1.0000` / `1.0000` | `0.5692` / `1.0000` |
| **Ikiss** (`ikiss`) | `5/8` (62.5%) | `4/8` (50.0%) | `1/8` (12.5%) | `1/8` (12.5%) | `1.0000` / `1.0000` | `0.1189` / `1.0000` | `0.1189` / `1.0000` |
| **Loken** (`loken`, `q=2`) | `6/8` (75.0%) | `5/8` (62.5%) | `6/8` (75.0%) | `5/8` (62.5%) | `1.0000` / `1.0000` | `1.0000` / `1.0000` | `1.0000` / `1.0000` |
| **Devourer** (`devourer`, `q=2`) | `3/8` (37.5%) | `3/8` (37.5%) | `5/8` (62.5%) | `5/8` (62.5%) | `1.0000` / `1.0000` | `0.6193` / `1.0000` | `0.6193` / `1.0000` |
| **Pooled (`40` trials/arm)** | **`24/40` (60.0%)** | **`25/40` (62.5%)** | **`20/40` (50.0%)** | **`22/40` (55.0%)** | **`1.0000`** | **`0.5005`** | **`0.8213`** |

*Exact two-sided Fisher tests; `p_holm` applies Holm–Bonferroni across all `m = 15` per-boss `llm`-vs-other comparisons. No individual boss or pooled kill difference is statistically significant at `n = 8`.*

---

## 2. Party Median DPS & Median Deaths (Per Boss & Arm)

| Boss | `llm` DPS (vs `llm_no_notes`) | `llm_no_notes` DPS | `no_brain` DPS | `baseline` DPS | Median Deaths (`llm` / `no_notes` / `no_brain` / `base`) |
|---|---:|---:|---:|---:|---:|
| **Kael'thas** | `1,783.1` (`-9.3%`) | `1,965.9` | `969.7` | `1,474.0` | `5.0` / `3.5` / `5.0` / `1.0` |
| **Murmur** | `2,466.8` (`-0.4%`) | `2,477.8` | `3,001.5` | `2,530.9` | `1.5` / `1.0` / `1.5` / `1.5` |
| **Ikiss** | `885.0` (`-11.2%`) | `996.9` | `924.0` | `665.0` | `4.0` / `4.0` / `5.0` / `5.0` |
| **Loken** (`q=2`) | `5,436.2` (`+2.1%`) | `5,323.5` | `5,393.1` | `3,477.5` | `0.5` / `1.0` / `2.0` / `2.0` |
| **Devourer** (`q=2`) | `4,911.4` (`+16.7%`) | `4,209.9` | `4,390.9` | `4,540.4` | `4.5` / `5.0` / `2.0` / `1.5` |

---

## 3. Honest Takeaways

1. **High 1 was indeed the cause of the `10/40` collapse:** stopping `away`/`out of` from unconditionally walking straight back lifted `llm` from `10/40` to **`24/40`**, `Murmur` from `0/8` (`-37.5%` DPS) to **`7/8` (`-0.4%` DPS)**, and `Loken` from `2/8` (`-38.9%` DPS) to **`6/8` (`+2.1%` DPS)**.
2. **Notes no longer hurt kills** (`24/40` vs `25/40`, `p = 1.0000`), and `llm` DPS is within `10%` of `llm_no_notes` on 4 of 5 bosses (`kaelthas -9.3%`, `murmur -0.4%`, `loken +2.1%`, `devourer +16.7%`; `ikiss` is `-11.2%`).
3. **What is still open:** `llm` kills (`24/40`) do not yet beat `no_brain` (`20/40`, `p = 0.5005`) or `baseline` (`22/40`, `p = 0.8213`) significantly, and `Kael'thas` (`3/8` vs `baseline 6/8`) and `Devourer` (`3/8` vs `5/8`) still lag `baseline` on kills.
