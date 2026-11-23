# Harness spec

The harness (`harness/`) is the brain's process, separate from the game server. The server module
(`server_module/`, inside `worldserver`) sees the game and carries out commands; the harness reads
what the module sends, decides when a bot's brain should think, asks `llama-server`, and sends
commands back. They share `common/` (threads, sockets, framing) and the snapshot structs in
`snapshot/snapshot.h` (Glaze BEVE wire format).

```
worldserver (mod-llm-brain)  <-- Unix socket, BEVE frames -->  harness  <-- HTTP -->  llama-server
  snapshots, events  ---------------------------------------------->  per-bot state, triggers,
  <-------------------------------------------- commands (choose, say)  prompts, plans, logs
```

What the brain thinks and what a plan contains are specified in `docs/specs/triggers.md`,
`docs/specs/options.md`, and `docs/specs/grammar-idl.md`; measured costs live in `docs/results.md`.

## Parts

| Part | Responsibility |
|---|---|
| `screen_renderer` | Renders a snapshot as compact text with numbered options from `snapshot.options`. |
| `plan` / `orders` | Generated `.wlb` grammar (GBNF built per call from options/names), parser, validator, formatter. |
| `llm_client` | Streaming SSE HTTP client to `llama-server` on a thread pool (`TCP_NODELAY`, keep-alive, `FirstLineCallback`, `LlmCancelHandle`). |
| `bot_loop` | Per-bot state: latest snapshot, events since last look, active plan, goal, early `do:` dispatch, age logging. |
| `scheduler` | Urgency queue (`P0`–`P4`), bounded calls in flight, soft idle-slot affinity, `P0` preemption of lower-tier in-flight calls. |
| `decision_log` | `var/bots/<name>/events/<date>.jsonl`: snapshot, prompt, reply, choice, latency, and age metrics. |
| `module_connection` | Unix socket client to the server module: BEVE frames in, commands out, reconnect, delivery stats. |
| `triggers` | Client-equivalent default alerts, edge-latched watches, `expect` suppression, `look` timer. |
| `plan_runner` | Carries a plan out step by step (counts, expectations, per-kind safety timeouts) and sends `choose`. |
| `main` | `harness run` (live) and `harness decide <snapshot.json>` (offline eval entry point). |

## Core contracts

- **The harness numbers nothing itself.** `screen_renderer` puts each `snapshot.options` number on
  the line matching its `guid` (the rest under `also:`). Until module options are present, the
  harness sends no `Choose`: two numberings that disagree would hit the wrong target silently.
- **`"ok"` means accepted, not done.** A choice completes when later snapshots show it (target dead,
  loot gone, out of combat), not when the module accepts the command. `plan_runner` infers step
  completion from snapshots with per-kind safety timeouts.
- **Repeats re-resolve.** A plan's `3x4` (e.g. attack four boars) means "same kind and name": for
  each repeat `plan_runner` finds that option again in the newest snapshot rather than re-sending
  the old `(seq, option)` whose unit is already dead.

## Age: how old is what the brain acts on

`time_ms` in every snapshot is the module's wall clock (same machine clock everywhere). Every stage
is measured:

| Point | Measured by |
|---|---|
| built to handed to the socket | Module per-minute log (`sent n=... built to socket p50 p99 max` + replaced count). |
| built to received | `ModuleConnection::TakeDeliveryStats()` (per minute in `harness run`) and `fake_harness.py --stats`. |
| built to the question asked | `age_at_ask_ms` in the decision log (`bot_loop.cc`). |
| asked to the plan read | `first_line_ms` and `round_trip_ms` in the decision log. |
| built to the plan ready | `age_at_first_line_ms` and `age_at_plan_ms` in the decision log. |
| built to the command applied | Module `age_at_apply` from the command's `seq` (per choice and per minute in server log). |

## Snapshot delivery

- One snapshot per bot per interval (1 s), **staggered** (`i/N` of the interval) so a world tick
  builds one or two snapshots rather than all bots at once.
- Combat snapshots run every **250 ms**; urgent state flips (`in_combat`, `alive`, loss of control,
  attacker count, active-target completion) and alerting `ImmediateEvent`s emit a fresh snapshot
  immediately.
- If the harness is behind and the socket is full, a newer snapshot **replaces** the waiting one
  (latest wins); events are never dropped or reordered.
- `llama-server` connections stay open between calls (keep-alive).
