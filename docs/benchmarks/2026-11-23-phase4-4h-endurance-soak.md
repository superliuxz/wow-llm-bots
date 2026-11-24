# Phase 4 4-Hour Endurance Soak Report (Classic + TBC + WotLK, 2026-11-23)

- **Commits tested (split-binary live soak):** `b7bf727` for `r00`, and `4b6be66` for `r01..r05` (`Stack()` ran `ninja install` between `r00` and `r01`, installing `out/server-test/bin/worldserver` at `13:56:36`).
- **Scorer verification:** `soak.py --verify-hash` verified the 7-file `soak_scorer.SCORER_FILES` digest (`scorer=c1253b66fe60`). The full 30-file `scorer.sha256` manifest (`1a867dcdf2fd...`) does not yet include the newly added endurance configs or modified `soak.py`/`testing.py` and awaits a user-approved relock per `CONTRIBUTING.md` Rule #6.
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

| Run | Scenario | Decisions | `max_wakes/min` | `do_p50` / `do_p95` | Errors / Invalid | XP/h | Deaths (per-run / flushed) | Moved | Open-World Kills / GO / Skill-Up (raw) | Dungeon Kills (`Loadf..j`, raw; inside-dungeon total) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| `r00` | `soak_classic_ulda` (Uldaman L42) | 847 | 3.6 | 160 / 439 ms | 0 / 0 | 8,081 | 3 / 3 | 10/10 | `168` / `12` / `12` (`11` zones) | `[34, 24, 26, 26, 25]` (`130` inside; `0` deaths, `1` aborted HS) |
| `r01` | `soak_classic_strat` (Stratholme L60) | 1,205 | 6.1 | 163 / 380 ms | 0 / 0 | 23,768 | 16 / 17 | 10/10 | `229` / `12` / `9` (`7` zones) | `[153, 14, 12, 15, 14]` (`195` inside; `0` deaths, `0` HS) |
| `r02` | `soak_tbc_seth` (Sethekk Halls L67) | 1,816 | 18.6 | 163 / 359 ms | 0 / 0 | 39,900 | 7 / 7 | 10/10 | `294` / `42` / `5` (`9` zones) | `[33, 32, 33, 16, 32]` (`139` inside; `5` deaths, `1` completed HS) |
| `r03` | `soak_tbc_mgt` (Magisters' Terrace L70H) | 853 | 5.1 | 180 / 452 ms | 0 / 0 | 29,607 | 3 / 4 | 10/10 | `226` / `4` / `8` (`7` zones) | `[0, 1, 2, 2, 2]` (`0` inside; `0` deaths, `0` HS) |
| `r04` | `soak_wotlk_ak` (Ahn'kahet L74) | 1,837 | 13.7 | 196 / 390 ms | 0 / 0 | 42,655 | 3 / 3 | 10/10 | `241` / `36` / `2` (`9` zones) | `[2, 1, 1, 0, 2]` (`0` inside; `0` deaths, `0` HS) |
| `r05` | `soak_wotlk_pos` (Pit of Saron L80H) | 874 | 3.9 | 183 / 456 ms | 0 / 0 | 10,802 | 1 / 1 | 10/10 | `86` / `19` / `9` (`7` zones) | `[61, 59, 60, 51, 46]` (`274` inside; `1` death, `0` HS) |
| **Pooled** | **60 bot-runs (`40` bot-hours)** | **7,432** | **18.6** | **174 / 456 ms\*** | **0 / 0** | **25,556** | **33 / 35** | **60/60** | **`1,244` (`1,232` post-setup) / `125` / `45` (`30` prof)** | **`779` raw (`738` inside), `6` deaths, `1` HS exit + `1` aborted** |

*\*Pooled `do_p50 / do_p95` (`174 / 456 ms`) is the unweighted mean of the 6 per-run medians and the max of the 6 per-run p95s (matching `soak.py`'s summary aggregation), not pooled quantiles over all 7,432 decisions. Per-run `Deaths` sum to `33` because per-run scoring ran before `worldserver` stopped and flushed final buffered records; flushed pooled traces contain `35` deaths (`+1` late death in `r01` -> `17` and `+1` in `r03` -> `4`).*

*(Honest note on setup-time vs post-setup trace counters: `quest_turnin=594` and `item_equip` include setup-time `PlayerbotFactory` / `llmtest gear` initialization; `41` of the `779` raw dungeon kills occurred in starting zones before `llmtest tele` (`r03` MgT and `r04` AK had **`0` actual inside-dungeon kills**; total inside-dungeon kills = **`738`**). On open-world bots, post-setup counts are `1,232` mob kills (`1,244` raw), `166` quests accepted (`195` raw), `125` GO/node interactions, and `30` profession skill-ups (`29` Herbalism + `1` Mining; the other `15` of `45` raw skill-ups were pre-setup weapon/defense skills). Additionally, `soak.py:395` emitted `llmtest additem` instead of `llmtest item`, so `42/42` profession tool commands failed (`0/30` bots received Skinning Knife / Mining Pick / Fishing Pole, yielding `0` Skinning, Fishing, First Aid, or Cooking skill-ups).)*

---

## 2. Layer A Guardrails & `stall_check.py`

- **Layer A gates (`11/12` PASS, `1` FAIL):**
  - **PASS:** `has_traced_bots` (`60 >= 30`), `has_decisions` (`7,432 >= 1`), `no_llm_errors` (`0`), `no_invalid_plans` (`0`), `no_wake_loop` (`18.6 <= 20.0`), `latency_p95` (`456 ms <= 2000 ms`), `not_stuck` (`0/60 <= 18`), `deaths_bounded` (`35 <= 60`), `no_refusal_loop` (`0 <= 5`), `human_whisper_timing` (`None`), `recording_parity` (`human=4.98 Hz, bot=4.98 Hz`).
  - **FAIL (accepted per user direction):** `human_react_timing` (`react_p10 = 41.0 ms` vs `>= 150 ms`, from Playerbots' C++ combat reflexes; user confirmed faster combat reflexes are acceptable as long as there are no cheater moves).
- **Open-world questing + professions (`Loada..Loade`, `30` bot-runs / `20` bot-hours):**
  - **`0/30` stalls** flagged by `stall_check.py` (`>= 300 s`).
- **Hard 5-man dungeons (`Loadf..Loadj`, `30` bot-runs / `20` bot-hours):**
  - **What worked:**
    - Completed Hearthstone dungeon abandonment dropped to **`1` across 30 dungeon bot-runs** (`Loadi_r02` in Sethekk Halls; `Loadf_r00` in Uldaman had `1` interrupted Hearthstone `item_use` with no `spell_cast` or `map_change`).
    - **Pit of Saron L80H** (`274` inside-dungeon kills, `1` death), **Sethekk Halls L67** (`139` inside-dungeon kills, `5` deaths), and **Uldaman L42** (`130` inside-dungeon kills, `0` deaths) engaged and cleared packs as a group. In **Stratholme L60** (`195` inside-dungeon kills), leader `Loadf_r01` advanced `346.99 yd` (`151` dungeon kills) while the 4 followers remained at the entrance (`max_dist_from_entrance = 0.00 yd`, `11` dungeon kills each near the gate).
  - **Remaining open gaps observed in traces & `stall_check.py` (`38` stall spans on `Loadf..Loadj`):**
    1. **Post-setup `item_equip` ping-pong and higher-level stone/oil reuse persisted:** `Loadf_r04` had `491` post-teleport `item_equip` events and `Loadh_r04` had `383` (swapping equal/near-ilvl off-spec items left in bags by `llmtest gear`); higher-level stones/oils also repeated frequently (`Loadf_r03`: `124` `Adamantite Sharpening Stone`, `Loadc_r02`: `84` `Brilliant Wizard Oil`, `Loadf_r00`: `70` `Dense Sharpening Stone`).
    2. **Long empty entrance corridors (`Magisters' Terrace L70H` & `Ahn'kahet L74`):** both had **`0` inside-dungeon kills** because the first hostile packs lie well past the entrance portal beyond short straight `Explore` rays.
    3. **Multi-room dungeon navigation:** after clearing visible entry rooms in Uldaman, Stratholme, Sethekk Halls, and Pit of Saron, short straight `Explore` rays (`option_facts.cc`) did not guide the party leader through dungeon turns/doors/ramps into the next room, leaving stalls up to `1,793 s` (`1,792.7 s` on `Loadj_r04`).
