# Boss Benchmark Run (2026-11-20) — Invalidated Preliminary Data

> [!WARNING]
> **Invalidated by post-run code review (see `docs/TODO.md`):**
> The 45-trial table below was recorded **before** fixing three benchmark-runner bugs:
> 1. `server.console()` returned `None`, so the role and `checkstrat` assertions never executed.
> 2. `checkstrat` ran before teleporting into the dungeon (Playerbots attaches instance strategies only after entering the dungeon map), and checked only the leader.
> 3. The `floor` column ran the LLM brain without `notes.md` (not a no-brain floor), and starting `harness` in Northshire caused bots to leave group / walk away before teleporting (leaving `Ikiss` floor at `0 DPS` on all 3 runs).
> 4. `n = 3` trials per cell is too small for kill-rate significance (`9/15` vs `5/15` is `p = 0.27`).
>
> All four runner bugs are now fixed in [tools/bench/bench_bosses.py](../../tools/bench/bench_bosses.py) (`no_brain`, `llm_no_notes`, `llm`, `baseline`, in-dungeon staging, post-arrival per-bot `checkstrat`, and automatic no-pull trial invalidation). Re-run with the fixed runner before drawing conclusions.

---

## Raw Preliminary Numbers (kept for historical record only)

| Boss | Era / Lvl | `llm_no_notes` (labeled `floor`) | `llm` (with `notes.md`) | `baseline` (scripts ON, with `harness`) |
|---|---|---:|---:|---:|
| **Talon King Ikiss** | TBC L68 | `0/3` (0 DPS, no-pull) | `0/3` (686 DPS, 5.0d) | `1/3` (622 DPS, 5.0d) |
| **Kael'thas Sunstrider** | TBC L70 | `2/3` (2,140 DPS, 2.0d) | `0/3` (921 DPS, 5.0d) | `2/3` (1,475 DPS, 1.0d) |
| **Devourer of Souls** | WotLK L80 | `3/3` (12,739 DPS, 0.0d) | `3/3` (11,869 DPS, 0.0d) | `3/3` (9,776 DPS, 1.0d) |
| **Ick** | WotLK L80 | `2/3` (2,765 DPS, 1.0d) | `1/3` (787 DPS, 1.0d) | `1/3` (1,019 DPS, 1.0d) |
| **Murmur** | TBC L70 | `2/3` (2,615 DPS, 2.0d) | `1/3` (1,201 DPS, 4.0d) | `1/3` (1,567 DPS, 5.0d) |

### What still holds from this run
- **Devourer of Souls** is overgeared at L80 epic gear (`3/3` across every mode).
- Lower DPS with `notes.md` on Kael'thas, Ick, and Murmur is a lead worth testing once `act_sent` tracing (`dodge_timeline.py`) and the fixed runner are re-run.
