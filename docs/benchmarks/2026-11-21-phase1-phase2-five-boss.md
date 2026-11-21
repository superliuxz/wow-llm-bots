# Phase 1 & Phase 2 Five-Boss Benchmark (2026-11-21)

**Commit:** `221b706` (`kaelthas`) and `d7b54f4` (`murmur`, `ikiss`, `loken`, `devourer`), `n = 8` valid trials per arm per boss (`160` valid scored trials total, `0` invalid, `0` wave errors) using `bench_bosses.py --mode mixed_all --runs 8 --parallel 4`.

**Gear tuning (no bosses dropped):** `devourer` and `loken` were lowered from `ITEM_QUALITY_EPIC` (`4`) to `ITEM_QUALITY_UNCOMMON` (`2`) via `llmtest gear` optional quality parameter, breaking the prior `16/16` ceiling on both bosses so every boss separates arms.

## Pre-registered primary comparisons (Holm–Bonferroni corrected)

1. **Primary kill comparison (`llm` vs `no_brain`, two-sided Fisher exact, Holm–Bonferroni across `m = 5` bosses):**

| Boss | Gear | `llm` kills | `llm_no_notes` kills | `no_brain` kills | `baseline` kills | `llm` vs `no_brain` `p_raw` | `p_holm` (`m=5`) |
|---|---|---|---|---|---|---|---|
| `kaelthas` | L70 Epic | 3/8 (37.5%) | 5/8 (62.5%) | 2/8 (25.0%) | 6/8 (75.0%) | 1.0000 | 1.0000 |
| `murmur` | L70 Epic | 0/8 (0.0%) | 7/8 (87.5%) | 8/8 (100.0%) | 7/8 (87.5%) | 0.0002 (worse) | 0.0008 |
| `ikiss` | L68 Epic | 3/8 (37.5%) | 4/8 (50.0%) | 1/8 (12.5%) | 0/8 (0.0%) | 0.5692 | 1.0000 |
| `loken` | L80 Uncommon | 2/8 (25.0%) | 5/8 (62.5%) | 4/8 (50.0%) | 6/8 (75.0%) | 0.6084 | 1.0000 |
| `devourer` | L80 Uncommon | 2/8 (25.0%) | 3/8 (37.5%) | 4/8 (50.0%) | 3/8 (37.5%) | 0.6084 | 1.0000 |
| **Pooled** | — | **10/40 (25.0%)** | **24/40 (60.0%)** | **19/40 (47.5%)** | **22/40 (55.0%)** | `0.0619` | — |

2. **Phase 1 DPS retention (`llm` vs `llm_no_notes` median party DPS) & `dodge_timeline.py` recovery:**

| Boss | `llm` med DPS | `llm_no_notes` med DPS | Delta vs `llm_no_notes` | `llm` AoE Safe% | `no_brain` Safe% | `llm` med `BackMs` | `llm` med `DmgLost` |
|---|---|---|---|---|---|---|---|
| `kaelthas` | 1691.1 | 1869.8 | **-9.6%** (within 10%) | — | — | — | — |
| `murmur` (`Sonic Boom` 34 yd) | 1772.6 | 2836.2 | -37.5% | **81.2% (39/48)** | 0.0% (0/40) | 313 ms | 2,681 |
| `ikiss` (`Arcane Explosion` 35 yd) | 931.2 | 885.5 | **+5.2%** (within 10%) | 11.7% (9/77) | 0.0% (0/82) | 0 ms | 150 |
| `loken` (`Lightning Nova` 20 yd) | 3077.9 | 5038.2 | -38.9% | **64.9% (120/185)** | 22.8% (38/167) | **11 ms** (vs `baseline` 388 ms) | **1,098** (vs `baseline` 1,718) |
| `devourer` | 4475.1 | 4817.9 | **-7.1%** (within 10%) | — | — | — | — |

3. **Phase 2 `Kael'thas` `Flame Strike` damage parity (`llm` vs `baseline`):**

| Arm | Total Flame Strike dmg (`n=8`) | Median / trial | Trials `<= 1,000` dmg |
|---|---|---|---|
| **`llm`** | **7,460** | **0** | **6/8** |
| `baseline` | 11,229 | 0 | 5/8 |
| `no_brain` | 63,482 | 6,164 | 1/8 |
| `llm_no_notes` | 81,546 | 14,146 | 2/8 |

Exposing hidden trigger creatures' active visible non-passive spell visual auras (`NPC_FLAMESTRIKE_TRIGGER` `24666` / visual `44191`) in `nearby_builder.cc` + `in Flame Strike -> out of it` completely closed the `Flame Strike` gap (`llm` took less Flame Strike damage than `baseline`, down from `128,600` in the prior `n=16` run).

## Root causes of remaining kill/DPS gaps

1. **`Murmur` `0/8` (`Resonance` wipe):** Murmur is rooted and casts raid-wide `Resonance` every 1.5 s whenever no player is in melee range. Because `MURMUR_NOTES` had every bot (including the tank) run 38 yd away on `Sonic Boom`, the tank left melee and triggered `Resonance` wipes despite dodging `81.2%` of `Sonic Boom`. Needs role-aware standing-order notes (tank stays in melee / uses defensive cooldown while non-tanks dodge `Sonic Boom`).
2. **`Loken` `-38.9%` DPS (`2/8` vs `baseline` `6/8`):** Even though `llm` returned in `11 ms` median (`DmgLost = 1,098` vs `baseline 1,718`), `baseline` (`wotlk-hol`) stacks ranged/healers close to Loken between `Lightning Nova` casts to reduce distance-scaled `Pulsing Shockwave` damage, whereas `llm` ranged bots stayed at 25–30 yd taking heavy `Pulsing Shockwave` ticks and dying early.
3. **`Ikiss` `Arcane Explosion` (`11.7%` safe):** Ikiss blinks and casts a 35 yd 5 s AoE; `baseline` (`tbc-seth`) breaks LoS behind the room pillar rather than outrunning 35 yd in 5 s.
