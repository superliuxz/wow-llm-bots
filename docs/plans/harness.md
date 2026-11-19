# Harness plan (2026-11-16)

The harness (`harness/`) is the brain's process, separate from the game server. The server
module (`server_module/`, inside worldserver) sees the game and carries out commands; the harness
reads what the module sends, decides when a bot's brain should think, asks llama-server, and
sends commands back. They share `common/` (threads, sockets, framing) and the snapshot structs in
`common/proto/snapshot.h` (Glaze BEVE wire format).

```
worldserver (mod-llm-brain)  <-- Unix socket, BEVE frames -->  harness  <-- HTTP -->  llama-server
  snapshots, events  ---------------------------------------------->  per-bot state, triggers,
  <-------------------------------------------- commands (choose, say)  prompts, plans, logs
```

What it does is specified in `docs/specs/triggers.md` (when the brain thinks, what a plan is)
and `docs/results.md` (what that costs). This plan is the build order.

## Parts

| Part | What | Needs the game |
|---|---|---|
| `screen_renderer` | a snapshot as the compact text the brain reads, with numbered options | no |
| `plan` | the plan grammar (GBNF, built per call from the bot's options), parser, validator | no |
| `llm_client` | llama-server over HTTP on a thread pool, results posted back to the bot's sequence | no |
| `bot_state` | per bot: latest snapshot, events since its last look, current plan, goal | no |
| `scheduler` | a queue by urgency (prompts and combat first), bounded calls in flight | no |
| `decision_log` | `var/bots/<name>/events/<date>.jsonl`: snapshot, prompt, reply, choice, latency | no |
| `module_connection` | the socket to the module: frames in, commands out, reconnect | yes |
| `triggers` | defaults from the client's alerts, the brain's watches, plan end, `look` timer | no (on recorded snapshots) |
| `plan_runner` | carries a plan out step by step (counts, expectations), sends `choose` | yes |
| `main` | wiring; `harness decide <snapshot.pb>` runs one decision offline | no |

## Steps

1. **H1 `screen_renderer`** (done): a C++ port of `tools/bench/prompt_render.py`, tested against
   its output on 10 real snapshots (`harness/testdata/screens`) and on hand-built cases.
2. **H2 `plan`** (done): the plan's grammar (built per call from the bot's options), parser,
   validator and formatter, with the vocabulary from `triggers.md`.
3. **H3** (done): `llm_client` (llama-server over HTTP, bounded in flight), the prompt builder and
   `content/RULES.md`, `decision_log` (`<root>/<bot>/events/<date>.jsonl`), `scheduler` (urgent first,
   one question per bot, a newer wake-up replaces a waiting one) and `harness decide <snapshot.pb>`:
   one decision offline on a saved snapshot (also the entry point for the quality check). Tried live
   with the 35B: valid plans in 210-310 ms. Not built yet: `bot_state`, which H4 needs.
4. **H4** (done 2026-11-16): `screen_renderer` numbers lines directly from `snapshot.options` by
   `guid` (`has_module_options`), `module_connection` (Unix socket client + reconnect + delivery
   ages), `triggers` (client defaults + edge-latched watches + `expect` suppression), `plan_runner`
   (infers step completion from snapshots with per-kind safety timeouts and re-resolves `xN` repeats
   by `(kind, name)`), `bot_loop` (early SSE `do:` dispatch, `Since your last look:`, `look` timer,
   and `age_at_ask` / `age_at_first_line` / `age_at_plan` logging), and `harness run` (`tools/wlb
   harness`). Tested end-to-end against a fake Unix-socket module and fake SSE `llama-server`.
5. **M (module side)**, in step with H4: the options list, the `choose` and `say` commands, chat
   events, then quest log, bags, gear, target, combat and party in the snapshot.
   **M1 (done 2026-11-16):** every snapshot carries `options` (`server_module/src/options/options.cc`):
   one "Attack <name>" per hostile or neutral NPC name (the nearest of that name), one
   "Loot <name>" per lootable corpse name, and "Keep doing what I'm doing" last; facts only, nothing
   ranked. `Command.Choose` (option, seq) is resolved against the bot's last 16 snapshots and
   carried out with playerbots' own attack action and loot strategy; a failure comes back as an
   `error` event, and each choice logs `age_at_apply`. Tried live (`config/tests/choose`: one
   level-1 priest next to a boar, its body's own grinding off, `fake_harness.py --choose Attack
   Loot`): it attacked, killed and looted the boar (age_at_apply 1-2 ms: the fake harness answers at
   once, so that is the plumbing only; the real number comes with H4). Each option carries the
   `guid` of the thing it acts on; history is kept by time (30 s), not by count; a refused attack
   leaves the body's target as it was; loot is re-checked when applied. Not yet: talk (playerbots
   opens a gossip window only for a bot's master), gather, use, and "go to" (options exist only for
   units with a name, within nameplate range, 20 yd: with grinding off, a bot can't choose anything
   farther away until it can walk there).

   **For H4, decided by this step:**
   - **The harness numbers nothing itself.** The screen puts each `snapshot.options` number on
     the line of the thing with that `guid` (the rest under "also:"), and until it does, the
     harness sends no `Choose`: two numberings that disagree would hit the wrong target silently.
   - **"ok" means accepted, not done.** A choice is done when the snapshots show it (the target
     dead, the loot gone, out of combat), not when the module says ok. Proposed: the harness
     infers it from later snapshots, with a timeout per kind; a module "step done / failed" event
     only if inferring proves unreliable. (Open, decide before `plan_runner`.)
   - **Repeats re-resolve.** A plan's `3x4` (attack four boars) is "the same kind and name": for
     each repeat the runner finds that option again in the newest snapshot, never re-sends the
     old (seq, option), whose unit is dead by then.
6. **Integration:** Testpriest decides and acts, end to end. Done when the phase 2 list in
   `docs/plans/phase2.md` holds.

## Conventions

Same rules as the rest of our C++ (CONTRIBUTING.md): Chromium style and `base::` types, include what you
use, `tools/wlb check` (format, includes, all sanitizers) before commit, unit tests with the
code. Sources in `harness/src` next to their `_unittest.cc`; test data in `harness/testdata`;
build targets are in `common/CMakeLists.txt` (`wlb_harness`, `harness_unittests`) so they get the
same flags and sanitizers as everything else. The `harness` executable comes with H3.

## Age: how old is what the brain acts on

A bot acts on a snapshot that was true when the module built it. Every step after that makes it
older, and each step is measured (`time_ms` in the snapshot is the module's wall clock, the same
machine's clock everywhere):

| Point | Measured by | Today |
|---|---|---|
| built to handed to the socket | the module, per minute in the server log (`sent n=... built to socket p50 p99 max`, and how many snapshots a newer one replaced) | done |
| built to received | `ModuleConnection::TakeDeliveryStats()` (per minute in `harness run`) and `tools/snapshot/fake_harness.py --stats` | done |
| built to the question asked | `age_at_ask_ms` in the decision log (`bot_loop.cc`) | done |
| asked to the plan read | `first_line_ms` and `round_trip_ms` in the decision log | done |
| built to the plan ready | `age_at_first_line_ms` and `age_at_plan_ms` in the decision log | done |
| built to the command applied | the module, from the `seq` a command names: `age_at_apply` (per choice and per minute in the server log) | done (module) |

The last one is the number that matters: how old the picture was when the bot acted on it.

## How snapshots reach the harness

- One snapshot per bot per interval (1 s), **spread over the interval**: bot i of N is first due i/N
  of an interval in, so a world tick builds one or two snapshots, never all 50.
- A bot in combat is sent snapshots every 250 ms, and a bot whose combat or life state changed since its
  last snapshot is sent one at once: the brain doesn't learn about a fight up to a second late.
- If the harness is behind and the socket is full, a newer snapshot **replaces** the waiting one
  (latest wins); events are never dropped or reordered.
- llama-server connections stay open between questions (keep-alive).
