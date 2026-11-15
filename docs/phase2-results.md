# Phase 2 results

## Snapshot pressure test (2026-11-15)

What it costs the world thread to build `nearby` with no caps (docs/specs/snapshot.md §4,
§7) in a busy place. 50 test bots stand in Orgrimmar's Valley of Strength
(`config/tests/pressure.llm-brain.overrides`, fixture `city-orgrimmar.pdump`, test build),
each seeing ~150 objects in its client visibility set: the 49 other bots, city NPCs, signs,
bonfires, the mailbox and auction house. Measured with the world server's own update times
(`.server info`, last 500 world updates) and the module's per-minute summary, with no harness
connected (the module builds nothing) and with a client reading every frame (50 snapshots a
second).

| | No harness | Harness connected |
|---|---|---|
| World update mean / median | 1 ms / 1 ms | 1 ms / 1 ms |
| World update p95 / p99 / max | 2 / 2 / 2 ms | 2 / 2 / 2 ms |
| Snapshot build (incl. serialization) | - | p50 55 µs, p99 ~155 µs, max 424 µs |
| World-thread time, all 50 bots | - | ~170 ms per minute (~0.3% of one core) |
| Snapshot size | - | ~8.2 KB (63 units, 23 objects) |
| Frames | - | 50.0/s, none dropped |
| Line-of-sight casts | - | 32 per snapshot (the budget) |

**Passes** the criteria set before the run: under 5 ms added per world update on average, no
single build over 2 ms, no dropped frames. Live snapshots validate against the spec.

About 67 of the ~150 visible objects are left out per snapshot: far and out of line of sight,
mostly NPCs inside the bank, auction house and other buildings (near ones behind walls still
show, with `in_los: false`, as nameplates do).

### Bugs the test found (fixed before the numbers above)

1. **Line-of-sight starvation.** Checks went to the nearest objects first; the 49 bots
   standing next to each other went stale every second and used the whole budget, so far
   objects were never checked and always left out. Now: never-checked objects first, then
   the least recently checked.
2. **Stale results counted as unknown,** so far objects flickered out whenever their result
   was older than its refresh time. The last known result now counts at any age (until
   pruned after 30 s); freshness only decides what to re-check next.
3. **Game objects never in line of sight.** They have no collision height, so the check aimed
   at their base on the ground and through their own model. Now the ray aims 1 yd above the
   base and checks the map geometry only (units still check everything, including doors).
4. **Test setup:** an account holds at most 10 characters (the fixture loader now fills a
   pool of test accounts), and a name with three identical letters in a row (`Presss`) is
   invalid, which made AzerothCore's dump loader silently save the character under a
   temporary name (the loader now checks names first).
