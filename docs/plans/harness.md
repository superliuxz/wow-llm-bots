# Harness plan (2026-11-16)

The harness (`harness/`) is the brain's process, separate from the game server. The server
module (`server_module/`, inside worldserver) sees the game and carries out commands; the harness
reads what the module sends, decides when a bot's brain should think, asks llama-server, and
sends commands back. They share `common/` (threads, sockets, framing) and the protobuf in
`server_module/proto/snapshot.proto`.

```
worldserver (mod-llm-brain)  <-- Unix socket, protobuf frames -->  harness  <-- HTTP -->  llama-server
  snapshots, events  ---------------------------------------------->  per-bot state, triggers,
  <-------------------------------------------- commands (choose, say)  prompts, plans, logs
```

What it does is specified in `docs/specs/triggers.md` (when the brain thinks, what a plan is)
and `docs/phase2-results.md` (what that costs). This plan is the build order.

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
2. **H2 `plan`:** grammar, parser, validator, with the vocabulary from `triggers.md`.
3. **H3** `llm_client`, `bot_state`, `scheduler`, `decision_log`, `harness decide`: the offline
   loop on recorded snapshots (this is also the eval entry point).
4. **H4** `module_connection` and `triggers`: live, against the running server.
5. **M (module side)**, in step with H4: the options list, the `choose` and `say` commands, chat
   events, then quest log, bags, gear, target, combat and party in the snapshot.
6. **Integration:** Testpriest decides and acts, end to end. Done when the phase 2 list in
   `docs/plans/phase2.md` holds.

## Conventions

Same rules as the rest of our C++ (CLAUDE.md): Chromium style and `base::` types, include what you
use, `tools/check-cpp.sh` (format, includes, all sanitizers) before commit, unit tests with the
code. Sources in `harness/src` next to their `_unittest.cc`; test data in `harness/testdata`;
build targets are in `common/CMakeLists.txt` (`wlb_harness`, `harness_unittests`) so they get the
same flags and sanitizers as everything else. The `harness` executable comes with H3.
