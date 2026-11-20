# Overnight Gameplay Benchmark Report (`trace-all-events`)

**Branch:** `trace-all-events` (local only, not pushed; `main` untouched at `2834756`)  
**Commits added overnight:**
- `3a77baf` — Ignore `SIGPIPE` in [harness/src/main.cc](../../harness/src/main.cc) so closed HTTP keep-alive connections to `llama-server` don't terminate `harness` (`rc=-13`). Verified: `harness` ran **240 minutes continuously with 0 crashes** (previously died at 34 min).
- `87668fc` — Filter [tools/bench/boss_meter.py](../../tools/bench/boss_meter.py) events and samples to the `[t_start, t_end]` combat window so pre-pull buffing/gearing and post-combat top-ups don't inflate CPM/HPS.

---

## 1. Geared 5-Man Boss Benchmarks

Both bosses were run on the isolated test server (`out/server-test`) with a fresh 5-man party (`Loada` Warrior Tank, `Loadb` Priest Healer, `Loadc` Mage, `Loadd` Warlock, `Loade` Hunter), properly geared via `llmtest gear <bot> <level>` and driven by real `harness run` + `llama-server` (Gemma 4).

### 1A. Trivial Tank-and-Spank Boss: Prince Keleseth (Utgarde Keep, L72 Geared 5-Man)
Isolates pure DPS/HPS throughput, threat/tanking, and rotation execution.

- **Result:** **KILLED in 40.0 s** | **0 deaths** | **4,659.6 party DPS** | **476.7 party HPS**

| Character | Role | DPS | Damage | HPS | Healing | Dmg Taken | CPM | Deaths | Max Dist (Boss Cast) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `Loadc` | Mage (DPS) | 1,681.7 | 67,336 | 0.0 | 0 | 1,244 | 106.4 | 0 | 11.2 yd |
| `Loadd` | Warlock (DPS) | 1,287.7 | 51,561 | 0.0 | 0 | 0 | 86.9 | 0 | 11.3 yd |
| `Loade` | Hunter (DPS) | 865.8 | 34,665 | 0.0 | 0 | 0 | 154.3 | 0 | 10.5 yd |
| `Loada` | Warrior (Tank) | 668.7 | 26,775 | 0.0 | 0 | 13,105 | 157.3 | 0 | 1.5 yd |
| `Loadb` | Priest (Healer) | 155.7 | 6,236 | 476.7 | 19,089 | 0 | 55.4 | 0 | 12.0 yd |

**Takeaway:** Clean tank-and-spank execution. `Loada` held melee range (`1.5 yd`) and absorbed **91.3%** of party damage; `Loadb` kept everyone alive with 0 deaths and 0 damage taken; ranged DPS stayed at 10–12 yd and sustained solid L72 throughput.

---

### 1B. Non-Trivial Mechanics Boss: Loken (Halls of Lightning, L80 Geared 5-Man)
Playerbots' hardcoded `-wotlk-hol` boss script was stripped; all bots received `notes.md` describing **Lightning Nova** (`Lightning Nova cast -> away Loken 25`) and **Pulsing Shockwave**.

- **Result:** **KILLED in 43.3 s** | **1 death** (`Loadc`) | **8,704.5 party DPS** | **1,348.4 party HPS**

| Character | Role | DPS | Damage | HPS | Healing | Dmg Taken | CPM | Deaths | Max Dist (Boss Cast) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `Loadd` | Warlock (DPS) | 4,303.1 | 186,485 | 272.9 | 11,825 | 16,501 | 126.0 | 0 | **24.6 yd** |
| `Loade` | Hunter (DPS) | 3,099.7 | 134,333 | 0.9 | 41 | 17,123 | 155.1 | 0 | **30.1 yd** |
| `Loada` | Warrior (Tank) | 826.3 | 35,808 | 0.0 | 0 | 10,938 | 88.6 | 0 | **27.2 yd** |
| `Loadb` | Priest (Healer) | 470.1 | 20,373 | 1,074.6 | 46,572 | 19,336 | 99.7 | 0 | **36.3 yd** |
| `Loadc` | Mage (DPS) | 5.3 | 228 | 0.0 | 0 | 35,394 | 15.2 | **1** | `-` |

**Takeaways:**
- **4 of 5 bots executed the standing-order dodge cleanly**, moving beyond the 20 yd Lightning Nova blast radius (**24.6–36.3 yd**) with **0 deaths**, while sustaining **8.7k party DPS** and **1.35k HPS**.
- **Diagnostic on `Loadc` (Mage) death:** From trace inspection (`var/bench_boss_loken_mechanics/bot/Loadc/`), `Loadc` targeted **itself** (`target="Loadc"`) at `t+3.0s` to cast `Conjure Mana Gem` / `Arcane Brilliance` and never switched back to `Loken` (`15.2 CPM`, `228 dmg`). Because it stood still without engaging, it took 7,525 from Lightning Nova + escalating Pulsing Shockwave ticks (2,385 → 4,789 → 5,912) and died at `t+39.2s`.

---

## 2. 5-Bot Solo `L1 → L10` Leveling Benchmark (4-Hour Unattended Run) vs Human (`Hele`)

Five solo bots started fresh at Level 1 in five separate starting zones and ran for **240 minutes** (0 harness crashes).

### Baseline — Real Human (`Hele`, Human Warlock, Elwynn Forest)
- **Active play (AFK stretches > 2 min removed):** `85.5 min` → **L6 + 2,214 XP** (`9,814 XP` total)
- **Pace:** **114.8 XP/min** (`6,888 XP/h`) | **L6 reached at ~71.9 active min** (`~105.7 XP/min`)
- **Playstyle:** **14 quests** (`9.8/h`), **89 mob kills** (`62.5/h`), **2 deaths**

### 5 Solo Bots (238.9 min total wall time)

| Bot | Race / Class | Zone | Final Level | Total XP | Avg XP/min | L5 Split | L6 Split | L8 Split | L9 Split | Quests (rate) | Kills (rate) | Deaths |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **`Loadd`** | Orc Warrior | Durotar | **L9 + 2,585** | 23,685 | **99.1** | 42.4m | **76.5m** | 188.9m | 223.3m | 13 (3.3/h) | 766 (192.4/h) | 7 |
| **`Loadc`** | Dwarf Paladin | Dun Morogh | **L9 + 554** | 21,654 | **90.6** | 64.0m | **89.0m** | 185.8m | 235.8m | 12 (3.0/h) | 608 (152.7/h) | 11 |
| **`Loade`** | Undead Mage | Tirisfal Glades | **L7 + 2,434** | 13,634 | 57.1 | 45.5m | 195.2m | — | — | 6 (1.5/h) | 270 (67.8/h) | 14 |
| **`Loada`** | Human Warlock | Elwynn Forest | **L5 + 2,421** | 7,221 | 30.2 | 76.5m | — | — | — | 7 (1.8/h) | 210 (52.7/h) | 15 |
| **`Loadb`** | Night Elf Hunter | Teldrassil | **L3 + 1,321** | 2,621 | 11.0 | — | — | — | — | 2 (0.5/h) | 39 (9.8/h) | 0 |

---

## 3. Key Findings & Actionable Bottlenecks Exposed by the New Trace Fields

> [!IMPORTANT]
> **Top 3 gameplay gaps vs a human player (all directly diagnosed from the new trace fields):**

1. **Full-bag stall (`bag_free_slots == 0`) is the #1 long-horizon leveling killer:**
   - `Loada` reached **L5 + 2,421 XP** (99% of the way to L6) at `t+76.5m`, then **froze completely** at `(-9808.8, 180.7)` for the remaining ~160 minutes (`moving=0.0%`, `bag_free_slots=0`, `target="Kobold Tunneler"`).
   - `Loadb` also hit `bag_free_slots=0` and bounced back and forth across Shadowglen (`79 area_change` events) for ~200 minutes before resuming kills at `t+217m`.
   - `Loade` stalled at L5 from `t+45m` to `t+195m` before breaking free to reach L7.
   - **Fix idea:** Suppress `Loot` when `bag_free_slots == 0` (or prioritize vendor/sell-junk when bags are full) so bots don't loop on `Inventory is full`.

2. **Mob-grinding vs questing ratio:**
   - Even the fastest bots (`Loadd` at **76.5m to L6** vs `Hele` at **71.9m** — within **6% of human speed!**) achieved their pace mostly by **grinding mobs** (`153–192 kills/h`, `3.0–3.3 quests/h`) rather than chaining quests like `Hele` (`62.5 kills/h`, `9.8 quests/h`).

3. **Boss pull target-acquisition (`Loadc` on Loken):**
   - `Loadc` spent the entire 43s Loken fight targeting itself after `Conjure Mana Gem` / `Arcane Brilliance` and never acquired `Loken` as its target. Ensuring grouped bots drop self-buffing and acquire the tank's/group's combat target once a boss pull starts would eliminate this failure mode.

---

## 4. Saved Raw Data
- Boss traces & summary JSON: `var/bench_boss_keleseth_trivial/`, `var/bench_boss_loken_mechanics/`, `var/bench_boss_results.json`
- Solo leveling traces (latest rotation window): `var/bench_l1_l6_traces/bot/{Loada..Loade}/`
- Benchmark scripts: `bench_bosses.py`, `bench_l1_to_l6.py` (local scratch runners)
