# Phase 4 4-Hour Endurance Soak Report (Classic + TBC + WotLK, 2026-11-23)

- **Commit tested (live soak binary):** `b7bf727` (`main`, `scorer.sha256` verified: `1a867dcdf2fd...`). *(Follow-up cold-review fixes `ee34a87` and `4b6be66` were completed concurrently with **No findings** from the final cold review and installed into `out/server-test` immediately after the soak completed.)*
- **Command:**
  ```bash
  tools/wlb test soak --suite endurance --minutes 40 --runs 6 --score \
    --human-dir ~/.local/share/wow-llm-bots/human_baseline \
    --verify-hash ~/.local/share/wow-llm-bots/scorer.sha256
  python3 tools/bench/stall_check.py var/soak_traces
  ```
- **Duration & scope:** `6 × 40.0 min` = **`4.0 h` wall-clock (`40` bot-hours, `60` bot-runs)** across Classic, TBC, and WotLK (`5` L32–L78 open-world quest+profession bots + `5`-man hard dungeon party per run with explicit `Tank / Healer / 3 DPS` spec tabs, Heroic difficulty on L70H/L80H, and per-dungeon `content/notes/<Dir>/notes.md` boss notes).
- **Log:** `var/phase4_4h_endurance_soak_2026-11-23.log`.

---

## 1. Per-Scenario & Pooled Metrics (`6 × 40 min` = `4 h` / `40` Bot-Hours)

| Run | Scenario | Decisions | `max_wakes/min` | `do_p50` / `do_p95` | Errors / Invalid | XP/h | Deaths | Moved | Open-World Kills / GO / Skill-Up | Dungeon Kills (`Loadf..j`) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| `r00` | `soak_classic_ulda` (Uldaman L42) | 847 | 3.6 | 160 / 439 ms | 0 / 0 | 8,081 | 3 | 10/10 | `168` / `12` / `12` (`11` zones) | `[34, 24, 26, 26, 25]` (`0` deaths, `1` HS) |
| `r01` | `soak_classic_strat` (Stratholme L60) | 1,205 | 6.1 | 163 / 380 ms | 0 / 0 | 23,768 | 16 | 10/10 | `229` / `12` / `9` (`7` zones) | `[153, 14, 12, 15, 14]` (`0` deaths, `0` HS) |
| `r02` | `soak_tbc_seth` (Sethekk Halls L67) | 1,816 | 18.6 | 163 / 359 ms | 0 / 0 | 39,900 | 7 | 10/10 | `294` / `42` / `5` (`9` zones) | `[33, 32, 33, 16, 32]` (`5` deaths, `1` HS) |
| `r03` | `soak_tbc_mgt` (Magisters' Terrace L70H) | 853 | 5.1 | 180 / 452 ms | 0 / 0 | 29,607 | 3 | 10/10 | `226` / `4` / `8` (`7` zones) | `[0, 1, 2, 2, 2]` (`0` deaths, `0` HS) |
| `r04` | `soak_wotlk_ak` (Ahn'kahet L74) | 1,837 | 13.7 | 196 / 390 ms | 0 / 0 | 42,655 | 3 | 10/10 | `241` / `36` / `2` (`9` zones) | `[2, 1, 1, 0, 2]` (`0` deaths, `0` HS) |
| `r05` | `soak_wotlk_pos` (Pit of Saron L80H) | 874 | 3.9 | 183 / 456 ms | 0 / 0 | 10,802 | 1 | 10/10 | `86` / `19` / `9` (`7` zones) | `[61, 59, 60, 51, 46]` (`1` death, `0` HS) |
| **Pooled** | **60 bot-runs (`40` bot-hours)** | **7,432** | **18.6** | **174 / 456 ms** | **0 / 0** | **25,556** | **35** | **60/60** | **`1,244` kills / `125` GO / `45` skill-ups** | **`779` kills, `6` deaths, `2` HS** |

*(Honest note on raw trace counters: `quest_turnin=594` and `item_equip` include setup-time `PlayerbotFactory` / `llmtest gear` auto-equips and high-level quest/spell initialization; live open-world progress is best reflected by `1,244` mob kills, `195` quests accepted, `2,011` loots, `125` gameobject/node interactions, `45` skill-ups, `1,068,715` total XP, and `0/30` stalls.)*

---

## 2. Layer A Guardrails & `stall_check.py`

- **Layer A gates (`11/12` PASS, `1` FAIL):**
  - **PASS:** `has_traced_bots` (`60 >= 30`), `has_decisions` (`7,432 >= 1`), `no_llm_errors` (`0`), `no_invalid_plans` (`0`), `no_wake_loop` (`18.6 <= 20.0`), `latency_p95` (`456 ms <= 2000 ms`), `not_stuck` (`0/60 <= 18`), `deaths_bounded` (`35 <= 60`), `no_refusal_loop` (`0 <= 5`), `human_whisper_timing` (`None`), `recording_parity` (`human=4.98 Hz, bot=4.98 Hz`).
  - **FAIL (accepted per user direction):** `human_react_timing` (`react_p10 = 41.0 ms` vs `>= 150 ms`, from Playerbots' C++ combat reflexes; user confirmed faster combat reflexes are acceptable as long as there are no cheater moves).
- **Open-world questing + professions (`Loada..Loade`, `30` bot-runs / `20` bot-hours):**
  - **`0/30` stalls** flagged by `stall_check.py` (`>= 300 s`).
- **Hard 5-man dungeons (`Loadf..Loadj`, `30` bot-runs / `20` bot-hours):**
  - **What improved dramatically:**
    - `Heavy Sharpening Stone` spam and `item_equip` ping-pong are gone.
    - Hearthstone dungeon abandonment dropped to **`2` total across 30 dungeon bot-runs** (`0` in Stratholme, Magisters' Terrace H, Ahn'kahet, and Pit of Saron H).
    - **Pit of Saron L80H** (`[61, 59, 60, 51, 46]` kills, `1` death), **Sethekk Halls L67** (`[33, 32, 33, 16, 32]` kills, `5` deaths), **Uldaman L42** (`[34, 24, 26, 26, 25]` kills, `0` deaths), and **Stratholme L60** (`153` leader kills) all engaged and cleared substantial packs together.
  - **Remaining open gap (`38` stall spans on `Loadf..Loadj` in `stall_check.py`):**
    1. **Long empty entrance corridors (`Magisters' Terrace L70H` & `Ahn'kahet L74`):** first hostile packs are `69 yd` (MgT) and `147 yd` (AK) from the entrance portal, so the party only cleared `0–2` mobs before parking.
    2. **Multi-room dungeon navigation:** after clearing visible entry rooms in Uldaman, Stratholme, Sethekk Halls, and Pit of Saron, short straight `Explore` rays (`option_facts.cc`) do not guide the party leader through dungeon turns/doors/ramps into the next room, so the party stalls (`300–1,792 s`) once nearby mobs are dead.
