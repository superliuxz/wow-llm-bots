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

## Brain load: the batched think cycle (2026-11-15)

The simple design: each think cycle, every bot's latest full snapshot goes to the LLM and the
LLM answers with what to do next (goal, next step, chat, when to look again). No rules in
C++ in between. All bots are sent at once (`tools/bench/think_cycle.py`, since removed: see git history before the tools cleanup; snapshots from
`tools/bench/capture_snapshots.py`). Qwen3.6-35B-A3B Q4 on the RTX 5090, llama-server with
25 slots x 12k context (50 slots didn't fit: this hybrid model also needs ~63 MB of recurrent
state per slot), thinking off, ~40-token JSON replies, all valid.

| Snapshots | Tokens per bot | 1 bot | 25 bots | 50 bots |
|---|---|---|---|---|
| Normal (the 50 spec examples: questing, dungeon, raid, town...) | ~1,000 | 0.4 s | 3.5 s | 9 s |
| Crowded (50 test bots in Orgrimmar, live captures) | ~8,000 | - | 19 s | 40 s |

- The cost is reading the snapshots: ~10.5k tokens/s when they're big. Replies are cheap.
- Prompt caching saved nothing on the crowded snapshots: llama-server only keeps checkpoints
  of a hybrid model's state every 8k tokens (`--checkpoint-min-step`), and the fixed prefix
  here (rules + identity) is ~700 tokens. Matters once RULES.md and memory are bigger.
- Bots asked to look again in 2-15 s; the GPU gives 50 bots a look every ~9 s in normal
  places and every ~40 s in a crowded city.
- Between two looks 5-10 s apart, ~25% of a crowded snapshot changed (bots standing still);
  1 s apart, ~2%.

### Compact prompt

The same test with the snapshot written for the LLM in compact text
(`tools/bench/prompt_render.py`, a prototype of the harness's prompt renderer): one short
line per unit or object in plain words, defaults left out (full health, in sight, idle...),
no GUIDs or raw coordinates (distance and direction stay), and units that look the same at
the same spot as one line listing every name. Nothing the brain could use is dropped.

| Snapshots | Tokens per bot (JSON -> compact) | 25 bots | 50 bots |
|---|---|---|---|
| Normal (spec examples) | ~1,000 -> ~1,000 | 3.5 s -> 3.2 s | 9 s -> 8 s |
| Crowded (Orgrimmar) | ~8,000 -> ~900-1,300 | 19 s -> 3.8 s | 40 s -> 9 s |

All replies valid. A crowd now costs about what a quiet zone does, so **50 bots get a look
about every 8-10 s anywhere**. That is the floor with this model and GPU: ~50k prompt tokens
plus ~2k reply tokens per cycle. Faster looks for the bots that need them (combat) mean
the others look less often.

The captured city snapshots have no `options` yet (snapshot steps 4-5), and some bots
answered with an option number anyway: the brain needs the options list to act.

## Brain load: plans and triggers (2026-11-15)

The design in `docs/specs/triggers.md`: the brain answers with a plan and is woken only by a
trigger. Benched in two steps.

**1. How often bots wake** (`tools/bench/trigger_sim.py`). 50 test bots questing on playerbots'
stock AI in Valley of Trials for 5 minutes (`config/tests/questing.llm-brain.overrides`), live
snapshots replayed through the default triggers today's snapshot can show (died, lost control,
an add while fighting, entered combat unexpectedly). Result: **no surprise triggers at all** in
5 minutes: every fight was one the bot started, against one mob. So the load is set by how long
the brain's plans last:

| Plan length (mean) | 15 s | 30 s | 60 s | 120 s |
|---|---|---|---|---|
| Wakes per bot per minute | 3.7 | 2.0 | 1.0 | 0.55 |

(A first version counted the pulled mob turning to fight back as a surprise: 113 false wakes
in 5 minutes. Until `ext.target` tells the harness what the bot itself attacked, the bench
counts only adds.)

**2. Load replay** (`tools/bench/brain_load.py`): those wakes played in real time against
llama-server (25 slots), each a brain call with the bot's real snapshot at that moment (compact
prompt, ~1,100 tokens) answered with a plan, urgent wakes first. 100 and 200 bots replay the 50
captured timelines with time offsets. "Wake to plan" is how long a woken bot waits for its new
plan.

| Bots | Plan length | Wakes/s | Wake to plan p50 / p95 | GPU busy |
|---|---|---|---|---|
| 50 | 60 s | 0.8 | **1.1 s / 2.1 s** | 46% |
| 50 | 30 s | 1.6 | 2.3 s / 5.4 s | 88% |
| 100 | 60 s | 1.6 | 1.8 s / 3.7 s | 83% |
| 50 | 15 s | 3.3 | 22 s / 34 s | 100% |
| 100 | 30 s | 3.2 | 13 s / 29 s | 100% |
| 200 | 60 s | 3.3 | 17 s / 41 s | 100% |

- **Capacity: ~1.7 wakes per second** on this GPU and model. Under it, a woken bot has its plan
  in 1-2 s; over it, the queue grows and waits reach tens of seconds.
- **Writing the plan is the cost, not reading the snapshot.** One stream decodes ~270 tokens/s,
  but 25 together only ~800 tokens/s (batching helps this MoE model little when decoding).
  A JSON plan was ~260 tokens and took ~8 s per think with 25 in flight; plain `key: value`
  lines (~115 tokens, no JSON grammar, which itself cost ~25%) brought it to 1-2 s.
  Shorter plans raise capacity directly.
- **Plan length is the brain's behavior.** Asked to prefer plans that last a while, the brain
  still chose `look` 30 s (median). 50 bots need plans of ~30 s or more; 100 bots ~60 s or more.
- Plan quality with today's snapshot is poor (it invents quests and mobs: it can't see the
  quest log yet, snapshot steps 3-5). This bench measures load, not judgment.

**Bug found by this bench (fixed):** 50 of our bots in one chat channel hung the world server at
~20 GB: playerbots' canned chat replies looped (`docs/playerbots-random-bot-audit.md`). Fixed
with `Playerbots.RandomBotTalk = 0`.

### Limiting what the brain can say

The same replay, with the brain allowed only what a player can actually do: pick from the bot's
numbered options (`snapshot.md` §9; a stand-in built from what's on screen, since `options`
isn't in the live snapshot yet) and the watch vocabulary (`triggers.md`). A grammar built per
call enforces it (`MODE=menu` in `tools/bench/brain_load.py`); free text only in a short goal
(60 characters) and an optional chat line (80).

| Bots | Plan length | Wakes/s | Before: wake to plan p50 / p95 | Limited: p50 / p95 | GPU busy |
|---|---|---|---|---|---|
| 50 | 15 s | 3.3 | 22 s / 34 s | **1.7 s / 3.7 s** | 87% |
| 100 | 30 s | 3.2 | 13 s / 29 s | **0.9 s / 2.0 s** | 81% |
| 100 | 15 s | 6.4 | - | 27 s / 42 s | 100% |
| 200 | 30 s | 6.2 | - | 19 s / 37 s | 100% |

- Plans dropped from ~115 to **~36 tokens**, and **every plan was valid** (1,546 of 1,546): the
  brain can't invent actions or mobs any more.
- **Capacity roughly tripled: ~4-5 wakes per second** (from ~1.7). 100 bots on 30 s plans, or
  50 bots on 15 s plans, get a new plan in about a second. Reading the prompt (~1,200 tokens)
  is now the main cost, so the next levers are a shorter prompt and prompt caching.
- Behavior to fix in the prompt, not the harness: the brain picks option 1 (the nearest thing)
  over half the time, adds a chat line to ~60% of plans, and still asks to look again in
  10-30 s.

### Prompt cache and a tighter screen

- **Prompt cache: already working.** Two calls to one slot with the same rules and a different
  screen recompute only the screen: the fixed rules (~230 tokens) are reused, with llama-server's
  default checkpoint spacing or with none. It matters more once RULES.md and memory are big.
- **Tighter screen** (`prompt_render.render_tight`, `RENDER=tight`): `self` as one line, `where`
  as one line, nameless far units (model detail only, nothing to act on) as counts by look
  with distances and directions, facing and "behind it" only within melee range, duplicate
  objects grouped, defaults dropped. Questing screen: 2,260 -> 812 characters; whole prompt
  (rules + screen + options) ~1,200 -> ~790 tokens.

| Bots | Plan length | Wakes/s | Limited: p50 / p95 | Limited + tight: p50 / p95 | GPU busy |
|---|---|---|---|---|---|
| 150 | 30 s | 4.8 | - | **1.8 s / 4.4 s** | 98% |
| 200 | 30 s | 6.2 | 19 s / 37 s | 6.5 s / 14 s | 100% |
| 100 | 15 s | 6.4 | 27 s / 42 s | 16 s / 23 s | 100% |

Capacity rose from ~4 to **~5 wakes per second**: 150 bots on 30 s plans get a plan in under
2 s. Less than the prompt cut suggests: with short prompts, the decoding steps (36 tokens per
plan, 25 streams at a time) are a large share again.

### Goal only when it changes, batching, and the cost of free text

Same replay (limited + tight), 30 s plans, at the edge of capacity:

| Change | 150 bots p50 / p95 | 200 bots p50 / p95 | Plan tokens |
|---|---|---|---|
| Limited + tight (before) | 1.8 s / 4.4 s | 6.5 s / 14 s | 36 |
| + goal only when it changes (`GOAL_ONCE`) | 1.7 s / 4.2 s | 5.9 s / 12 s | 33 |
| + batching non-urgent wakes, 1.5 s (`BATCH_S`) | 3.0 s / 5.6 s | 5.9 s / 12 s | 34 |
| Codes only: no goal, no chat (`CODES_ONLY`, a floor) | **1.1 s / 2.6 s** | **2.2 s / 5.9 s** | 22 |

- **Goal only when it changes barely helped**, because the brain wrote a goal every time
  (577 of 577): its goals are step-sized ("loot the nearby boar"), so they do change. It also
  added a chat line to 53% of plans.
- **Batching didn't help:** under load llama-server already has 25 calls in flight and batches
  them itself; holding wakes only added the wait.
- **The free text costs about a third of the capacity.** Without it, capacity is ~6 wakes per
  second (200 bots on 30 s plans get a plan in ~2 s). Getting there is about the brain's
  behavior, not the harness: goals that last (a session's goal, not the next step) and chat
  only when there's something to say. That needs the prompt tuned against a quality check.

### Speed: what limits the brain, and what helped (2026-11-15)

Capacity is measured closed-loop (`tools/bench/throughput.py`): workers fire real wake-ups
back to back, tight prompt (~800 tokens), grammar-limited ~36-token plans.

| Setting | Wake-ups/s | One call p50 |
|---|---|---|
| Baseline: 25 slots, default batches | 7.3 | 3.2 s |
| 16 slots | 7.0 | 2.2 s |
| 32 slots | 7.1 | 4.3 s |
| **`-ub 2048 -b 4096`** | **8.1** | 3.0 s |
| `-ub 1024` / `-ub 4096 -b 4096` | 7.8 / 8.3 | 2.9 s |
| 16 slots + `-ub 2048 -b 4096` (new default) | **8.2** | **1.85 s** |
| KV cache f16 / q4_0 (vs q8_0) | 7.2 / 7.4 | no change |
| CPU threads 4 / 8 | 8.4 / 8.3 | no change |
| No grammar, forgiving parser | 9.4 | 1.6 s |

- **The GPU is the limit**, not the CPU or the queue: more slots only add waiting, threads and
  KV type change nothing. Per decode step the cost grows linearly with the streams (~0.55 ms per
  stream; 1 stream 3.6 ms, 25 streams 17 ms, 64 streams 40 ms).
- **Sampling is free** (greedy, top-k 1 and the default chain run at the same speed).
- **The grammar costs ~10 ms per step at 25 streams** (CPU, one thread; `[^\n]{1,60}` vs `+`
  makes no difference). Without it capacity is +16%, but 31% of replies had format slips
  (`hp1` for `hp<1`, `[5]` for `5`). A forgiving parser recovers most: 93% valid, so about +8%
  in valid plans, in exchange for the hard guarantee. Kept the grammar.
- **Prompt cache works for conversations**: a second wake-up in the same slot reads only the
  new message (748 of 1,435 tokens), so rules, identity and memory at the head are free.
- **Sending only what changed doesn't work**: the screen is relative to the bot, so when it
  moves 10 yards every unit's distance and direction change. A line diff was bigger than the full
  screen (692 vs 584 tokens), even with distances in 5/10-yd steps and a stable order. And a
  conversation of ~800-token messages fills a 4k slot in 4-5 wake-ups.
- **The options list repeats the screen** (225 of ~810 tokens read per wake-up): numbering the
  screen's own entries instead would cut the prompt by about a quarter.
- A benchmark artifact worth knowing: a burst of requests right after a warm-up looked 2.5x
  slower; the server was busy for ~7 s (saving 63 MB prompt copies to host RAM per slot). It
  doesn't happen in steady state (same capacity with `--cache-ram 0`), so no setting needed.

Where the time goes now, per wake-up (~120 ms of GPU): reading ~550 new tokens (~60 ms), writing
~36 tokens (~25 ms in a batch), the rest scheduling. Next levers: number the screen's own
entries instead of a separate options list (-25% reading), and the brain's plan length (a
plan twice as long halves the wake-ups). Beyond that, a different server or model.

**Prompt cache A/B (realistic replay, 16 slots, `-ub 2048 -b 4096`, 30 s plans):** the host
prompt cache (`--cache-ram`, default 8 GB) on vs off. 100 bots: 0.5 s / 1.2 s vs 0.6 s / 1.3 s
(wake to plan, median / p95). 200 bots (GPU saturated): 3.2 s / 7.6 s vs 6.2 s / 11.9 s, one run
each. No gain from turning it off, so it stays at the default. An earlier improvement seen
with it off (200 bots 6.5 s to 4.4 s) also changed the slot context from 12k to 4k; the 4k is
now the default. The stall in the burst test (`decode_bench.py`, since removed: 25 streams took 9.4 s instead
of 3.8 s with the cache on) only appears when every slot has just finished a warm-up request;
it doesn't show in steady state or in the replay.

### Numbered screen instead of a separate options list

`render_tight(numbered=True)` (`MERGED=1` in `brain_load.py`): the screen's own entries carry
the option numbers, so nothing is written twice. Same server settings, closed loop, two rounds:

| | Wake-ups/s | One call p50 | Prompt |
|---|---|---|---|
| Separate options list | 8.3 / 8.3 | 1.83 s / 1.84 s | 773 tok |
| **Numbered screen** | **8.7 / 8.6** | 1.80 s / 1.78 s | 726 tok |

Only +5%, not the ~25% the list's size suggested: reading is a smaller share of a wake-up than
assumed, and the list was mostly cheap to read. Replay, 200 bots, 30 s plans: 3.1 s / 6.7 s
(vs 3.2 s / 7.6 s). All plans valid. Kept: it removes duplicate text and is simpler to build.

**Is the Python bench the bottleneck? No.** During a run the client used 1% of one core;
llama-server's busiest threads 40-53%; the GPU 59-83% busy at ~370 W (of ~575 W). The GPU
idles part of each step while llama-server does its per-stream CPU work (grammar, sampling).

### Response time for 50 bots (current setup: numbered screen, 16 slots, bigger batches)

| | Wake to plan, median / p95 / worst | GPU busy |
|---|---|---|
| One bot, nothing else running | 0.21 s / 0.26 s | - |
| 50 bots, plans ~30 s (1.9 wakes per bot per minute) | 0.5 s / 0.9 s / 1.2 s | 35% |
| 50 bots, plans ~15 s (3.9 per bot per minute) | 0.8 s / 1.6 s / 2.8 s | 68% |

### Counts and how long plans last (2026-11-16)

`do` may repeat an option (`3x4`: option 3, four times; `COUNTS=1`). How long a plan lasts is
estimated from the brain's own choices with stand-in action times (attack 20 s, talk 8 s, loot
3 s, gather 10 s, use 8 s, explore 45 s, rest 30 s, town 60 s; `keep` runs to `look`): the bot
wakes when its steps are done or its `look` runs out, whichever is first. 50 bots, 193 wake-ups:

| Prompt | Plans with a count | `look` median | Estimated wake-ups per bot per minute |
|---|---|---|---|
| No counts | 0% | 10 s | 3.3 |
| Counts (example `3x10`) | 39% | 15 s | 2.5 (73 of the counts were `x10`: copied from the example) |
| Counts (example `3x4`) | 36% | 10 s | 3.1 |
| **Counts + `look` is "a safety net only: you are woken at once if anything unexpected happens, usually 60-300 s"** | 34% | **60 s** | **1.3** |

- The brain asked to look again after ~10 s however long its plan was, so counts alone barely
  helped; what moved it was telling it that surprises wake it anyway, so `look` is only a
  safety net. That is a prompt about the brain's own patience, not a C++ rule.
- Plans cost ~43 tokens (up from 37) and stay all valid. These are estimates; real durations
  need the bot to play the plans.

### A smaller model: Qwen3-4B-Instruct (2026-11-16)

Same prompts, greedy, 200 identical wake-ups, the 35B's plans as the reference
(`tools/bench/agreement.py`; a stand-in for a quality check, since there is no right answer):

| | 4B vs 35B |
|---|---|
| Same first option / same kind of action | 52% / 62% |
| `look` within 2x | 33% |
| Adds a chat line to a plan | 100% (35B: 46%) |
| Plan tokens | 72 (35B: 42) |
| Capacity, closed loop, 16 workers | **11.0 wake-ups/s** (35B: 8.7), one call 1.4 s (1.8 s) |

Only 26% faster, not the several-fold gain expected: the 35B-A3B activates only ~3B parameters
per token, about what a 4B dense model uses, so there is little to save per token; the 4B also
wrote longer plans (a chat line every time). Agreement is low and the 4B is chatty, so **not
worth switching**. Caveat: the 35B's agreement with itself at normal temperature (the noise
ceiling) is not measured. A smaller model would pay off only with a genuinely small one for
routine picks, judged by the real quality check.

### Found by these benches

- `nearby` listed **"World Invisible Trigger"** (level 80, hostile, 5 yd away) in 160 of the
  sampled snapshots, 23 of 50 bots: invisible server objects a player could not see. The brain
  even chose to attack one. To fix in `nearby_builder.cc` (filter what the client would not show).
