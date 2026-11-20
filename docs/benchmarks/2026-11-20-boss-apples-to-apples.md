# Apples-to-Apples Boss Benchmark (3 Modes × 3 Runs, Fixed Roles & Verified Strips)

**Branch:** `trace-all-events` (`b78b63f`)  
**Runner:** [tools/bench/bench_bosses.py](../../tools/bench/bench_bosses.py)  
**Controls held identical across all 45 trials:**
- Fixed deterministic roles verified before every pull (`Loada` Prot Warrior `role=tank`, `Loadb` Holy Priest `role=heal`, `Loadc` Frost Mage `role=dps`, `Loadd` Destro Warlock `role=dps`, `Loade` MM Hunter `role=dps`).
- Verified strategy strips via `llmtest checkstrat` (`tbc-seth`, `tbc-mgt`, `wotlk-fos`, `wotlk-pos` confirmed `exists=1`, `combat=0 noncombat=0` in `floor`/`llm` modes).
- `harness run` connected before teleporting to the boss.

**Modes compared (3 runs each per boss):**
- **FLOOR:** Playerbots boss scripts **OFF**, no `notes.md` (no boss standing orders).
- **LLM BRAIN:** Playerbots boss scripts **OFF**, `notes.md` provided (LLM writes boss standing orders).
- **BASELINE:** Playerbots hardcoded C++ boss scripts **ON**, no `notes.md`.

---

## 3-Mode × 3-Run Scoreboard (5 Mechanics Bosses, 45 Trials Total)

| Boss | Era / Lvl | FLOOR (no scripts, no notes) | LLM BRAIN (notes, no C++ scripts) | BASELINE (C++ scripts ON) |
|---|---|---:|---:|---:|
| **Talon King Ikiss** | TBC L68 | `0/3` \| 0 DPS \| 0.0d | `0/3` \| 686 DPS \| 5.0d | **`1/3`** \| 622 DPS \| 5.0d |
| **Kael'thas Sunstrider** | TBC L70 | **`2/3`** \| 2,140 DPS \| 2.0d | `0/3` \| 921 DPS \| 5.0d | **`2/3`** \| 1,475 DPS \| 1.0d |
| **Devourer of Souls** | WotLK L80 | **`3/3`** \| 12,739 DPS \| 0.0d | **`3/3`** \| 11,869 DPS \| 0.0d | **`3/3`** \| 9,776 DPS \| 1.0d |
| **Ick** | WotLK L80 | **`2/3`** \| 2,765 DPS \| 1.0d | `1/3` \| 787 DPS \| 1.0d | `1/3` \| 1,019 DPS \| 1.0d |
| **Murmur** | TBC L70 | **`2/3`** \| 2,615 DPS \| 2.0d | `1/3` \| 1,201 DPS \| 4.0d | `1/3` \| 1,567 DPS \| 5.0d |
| **TOTAL KILLS** | — | **9 / 15** | **5 / 15** | **8 / 15** |

*(Additional single-run fixed-role sanity checks in `llm` mode: **Princess Theradras** [Classic L50 5-man] = **KILL** in 29.2 s, 0 deaths, 707 DPS; **Patchwerk** [Naxx 10 L80 10-man, fixed 2 Tanks + 2 Healers] = **WIPE** at 45.0 s despite 9,967 DPS and 9,259 HPS from Holy Priest + Resto Druid.)*

---

## Key Scientific Findings

1. **`notes.md` standing orders currently *hurt* kill rate (`5/15` vs `9/15` FLOOR) by cutting party DPS in half:**
   - **Kael'thas:** `2,140 DPS` (`2/3`) in FLOOR → `921 DPS` (`0/3`) in LLM.
   - **Ick:** `2,765 DPS` (`2/3`) in FLOOR → `787 DPS` (`1/3`) in LLM.
   - **Murmur:** `2,615 DPS` (`2/3`) in FLOOR → `1,201 DPS` (`1/3`) in LLM.
   - Standing orders (`away`, `avoid`, `target nearest ...`) fire frequently and interrupt casts / walk bots out of range without cleanly resuming DPS, lengthening fights until healers OOM or the party wipes.
2. **`Devourer of Souls` is overgeared at L80 epic (`3/3` in ALL modes, including FLOOR):**
   - Not a discriminating mechanics test at current `ITEM_QUALITY_EPIC` gear — needs lower gear quality or Heroic difficulty to test `Mirrored Soul` / `Wailing Souls`.
3. **`Talon King Ikiss` is a real mechanics discriminator:**
   - With `tbc-seth` verified OFF, neither FLOOR (`0/3`) nor LLM (`0/3`) killed Ikiss across 3 runs, while BASELINE (`tbc-seth` ON) killed `1/3`.
4. **Setup artifact caught in harness-before-tele prep window:**
   - Starting `harness` while bots were still in Northshire let some bots pick `Leave group` or `Go to Stormwind City` during the 10 s prep window before teleporting (also explaining why `Ikiss` FLOOR had `0 DPS` on all 3 runs). Next fix: teleport bots into a safe out-of-aggro staging spot *inside the dungeon* before starting `harness`, then move/pull on cue.

---

## Raw Data & Tools
- `var/bench_boss_10_{floor,llm,baseline}.json`
- Dodge reaction timelines: [tools/bench/dodge_timeline.py](../../tools/bench/dodge_timeline.py)
