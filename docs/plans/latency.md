# End-to-end latency plan (2026-11-16)

`wake -> plan` in `docs/phase2-results.md` (`~210-240 ms` solo, `0.5-1.0 s` for 50 bots on 30 s
plans, `1.9-2.6 s` for 100 bots on 15 s plans) is only the middle slice. True end-to-end latency is
from **something happening in the game world** to **the bot's body acting on it in the world**:

| Stage | Today | Main cause of delay |
|---|---|---|
| 1. World event → snapshot built | `0-1,000 ms` (`0-250 ms` combat) | Only `in_combat` and `alive` flips bypass the schedule; adds, stuns, step completion and `ImmediateEvent`s don't trigger fresh snapshots. |
| 2. Module ↔ harness IPC (`wlb_net`) | `~0.6-1.2 ms` | Already fast (`SendLatest` + Unix socket). |
| 3. Harness scheduler queue | `0 ms` idle / `0.5-3.0 s` busy | 16 shared slots (~7.8-8.6 wakes/s); an obsolete routine call in flight blocks an urgent wake for the same bot. |
| 4. Prompt prefill & prefix cache | `~60-150 ms` | Per-bot `bot_name` + `identity` inside `system` defeats `llama-server`'s hybrid-model checkpointing on shared slots. |
| 5. LLM decode & grammar | `~150-1,800 ms` | Blocking HTTP waits for all ~36-43 tokens (`goal` + `say`) before sending `do:`; GBNF disables GPU `--backend-sampling`. |
| 6. `ExecuteOption` → `PlayerbotAI` acts | `~1 ms` (`attack`) / **`1-20 s`** (`loot`/state) | Updating `AiObjectContext` values without resetting `PlayerbotAIBase::nextAICheckDelay`. |

## Note on HTTP vs. `libllama`

Swapping HTTP/JSON over loopback TCP (`cpp-httplib` keep-alive) for binary IPC or in-process calls
while keeping the same inference work saves only **`~2-4 ms`** per call (`~0.2 ms` socket RTT,
`~0.2 ms` JSON, `~0.4 ms` Jinja, `~1-1.5 ms` static re-tokenization and GBNF parse), which is
`< 1-2%` of a decision. Meanwhile keeping `llama-server` in its own process preserves the 16-21 GB
model in VRAM across `harness` restarts and keeps uninstrumented CUDA libraries out of `harness`'s
ASan/TSan/MSan builds.

Dropping `llama-server` HTTP for a custom `libllama` runner only pays off later for what HTTP's
per-request API blocks: masking ~15 valid token IDs on structured fields while running GPU
`--backend-sampling` on free text (`~200-250 ms` saved under 16-slot load), plus direct logit
access for personality-weighted option sampling (Tier 3, #8).

## Done

- **Align `RULES.md` with `PlanGrammar` (`-40 ms` solo, measured, `2fea543`):** `llama.cpp`
  validates the sampled token in `O(1)` when it fits the grammar and scans all 248,320 vocab tokens
  on one CPU thread only when rejected (`common/sampling.cpp`). Adding a literal grammar-valid
  example to `content/RULES.md` cut solo median latency from `280 -> 239 ms` (Gemma 4 26B) and
  `252 -> 206 ms` (Qwen3.6-35B) while raising eval scores (`9/15 -> 13/15` and `6/15 -> 11/15`).
- **Tier 1 (2026-11-16, measured):**
  1. **Reset `PlayerbotAI` check delay on choices (`option_executor.cc`):** calls
     `ai->ResetActionDuration()` on `kAttack` and `kLoot` so `PlayerbotAIBase::UpdateAI` acts on
     the next world tick (`~1-50 ms`) instead of sleeping out `LootDelay` (1 s) / `RepeatDelay`
     (2 s) / `MaxWaitForMove` (5 s) / `RpgDelay` (10 s) / `SitDelay` (20 s).
  2. **`do:` on line 1 + SSE streaming early dispatch + `TCP_NODELAY` (`plan.{h,cc}`,
     `llm_client.{h,cc}`, `RULES.md`):** reordered plan format to
     `do -> expect -> watch -> look -> goal? -> say?`, enabled SSE streaming (`"stream": true`,
     `"stream_options": {"include_usage": true}`) with `FirstLineCallback` firing on the first
     `'\n'`, and enabled `client.set_tcp_nodelay(true)` on `LlamaConnection` (fixing a 40 ms Nagle
     stall on reused keep-alive sockets). Solo median time to `do:` dropped from **`206 -> 79 ms`**
     (Qwen3.6-35B; full reply `152 ms`) and **`239 -> 91 ms`** (Gemma 4 26B; full reply `224 ms`);
     under 100% 16-slot GPU saturation, `first-line` p50 is **`0.40 s`** (Qwen3.6) and **`0.42 s`**
     (Gemma 4) vs `1.85-1.96 s` previously (**~4.5x faster**).
  3. **Optional `goal?` (`plan.{h,cc}`, `prompt.{h,cc}`, `RULES.md`):** `Current goal:` is passed
     in `user` when active, and `goal:` is optional in `PlanGrammar`.
  4. **Hybrid-model checkpoint split + soft slot affinity (`prompt.{h,cc}`, `scheduler.{h,cc}`):**
     `LlmRequest::system` carries only shared `RULES.md` while `You are <bot>.`, `identity`, and
     `Current goal:` live at the head of `user`, caching **522-523 tokens** across distinct bots on
     shared slots instead of resetting `n_past = 0` on Qwen3.6-35B; `Scheduler` spreads bots across
     idle slots `0..max_in_flight-1` and reuses each bot's last slot when idle without waiting on
     busy slots. Combined with optional `goal?`, Qwen3.6-35B 16-slot capacity rose **`7.8 -> 11.3
     wake-ups/s` (+45%)** and 100-bot 15 s wake-to-plan dropped **`2.1 / 3.2 s -> 0.8 / 1.5 s`**
     (Gemma 4 26B: `8.6 -> 9.1 /s`, 100-bot 15 s `1.9 / 2.6 s -> 1.5 / 2.1 s`).

## Tier 2: perception & scheduling (build with H4)

5. **Event-driven snapshot fast-paths + snapshot on alerting `ImmediateEvent`s (`-125 to -1,000 ms`)**
   - *Where:* `server_module/src/snapshot/snapshot_schedule.{h,cc}`,
     `server_module/src/llm_brain_world_script.cc`.
   - *Problem:* `SnapshotSchedule::Flags` currently tracks only `{in_combat, alive}`. Loss of
     control (stun/fear/charm), a new attacker joining mid-fight, and active option completion
     (target died or `loot_target` cleared) wait up to `250-1,000 ms` for the next regular tick.
     Also, an `ImmediateEvent` (whisper, invite, error) arrives in `< 1 ms` while the harness's
     stored snapshot for that bot can be up to `999 ms` old.
   - *Fix:* add loss-of-control state, attacker count, and active-target completion to
     `SnapshotSchedule::Flags`, and send a fresh `SendLatest` snapshot (~85 µs) whenever an alerting
     `ImmediateEvent` is emitted.
   - *Verify:* `snapshot_schedule_unittest` + live combat/step-transition age measurements in H4.

6. **Preemptive in-flight cancellation + P0-P4 priority tiers (`-0.8 to -1.8 s` on surprise combat)**
   - *Where:* `harness/src/scheduler.{h,cc}`, `harness/src/llm_client.{h,cc}`.
   - *Problem:* if a bot has a routine `look` request in flight when an urgent combat trigger
     arrives 100 ms later, `Scheduler` waits for the obsolete call to finish (~1.8 s under load)
     before starting the urgent one, doubling reaction latency and burning a GPU slot.
   - *Fix:* track in-flight request urgency and cancellation handles (`httplib::Client::stop()`,
     which aborts slot generation in `llama-server` on socket close); cancel an obsolete lower-tier
     in-flight call when a `P0` combat wake arrives, and expand `Urgency` to the P0-P4 classes in
     `docs/design.md`.
   - *Verify:* `scheduler_unittest` and `llm_client_unittest` cancellation tests.

## Tier 3: raid scale & engine level (later)

7. **Standing orders (`when -> act`) (`800-3,000 ms -> < 50 ms` on raid bursts)**
   - *Where:* `harness/src/plan.{h,cc}`, `server_module/` (`docs/specs/triggers.md`, "Standing
     orders").
   - *Problem:* a raid-wide mechanic waking 25 bots in the same instant exceeds 16-slot GPU
     capacity (`~2-3 s` tail).
   - *Fix:* the brain writes up to 4 `when -> act` orders ahead of a pull (gated by learned
     mechanics); C++ executes matching orders on the world thread within 1 tick (`0` LLM calls).

8. **GPU `--backend-sampling` or thin `libllama` runner (`-20 to -40 ms` solo / `-200 to -330 ms` loaded)**
   - *Where:* `tools/wlblib/llm.py` or a dedicated `libllama` binary over `common/net`.
   - *Problem:* any GBNF grammar disables GPU `--backend-sampling` across the entire completion
     (`~10 ms/step` CPU overhead across active batches).
   - *Fix:* either run unconstrained with `--backend-sampling` + `ParsePlan` (with 1 grammar retry
     on rare format slips, `+16%` throughput), or replace `llama-server` HTTP with a thin `wlb_net`
     Unix-socket daemon linking `libllama` that masks ~15 valid token IDs on `do/expect/watch/look`
     and switches to GPU `--backend-sampling` on `goal`/`say`.
