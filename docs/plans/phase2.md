# Phase 2 plan: harness skeleton

**Goal:** one bot that chats and makes decisions through the local LLM, end to end, with the
architecture we decided (thin server module + separate C++ harness + local llama-server).
Not yet: goals system, full perception, upkeep, learning, many bots (phase 3+).

**Done when:**
1. You whisper the bot in game and it answers in character, written by the LLM.
2. When the bot finishes an activity, the LLM picks the next one from numbered options
   (quest / grind / wander / rest), and the bot does it.
3. The world thread never waits on the harness or the GPU (world update time unchanged).
4. Killing the harness mid-game leaves the bot playing on stock AI; restarting it reconnects.
5. Every decision is logged (snapshot, prompt, raw reply, parsed choice, latency).
6. `decide(snapshot)` can be run offline on a saved snapshot (the eval entry point).

## Architecture

```
worldserver process                                harness process (ours)          llama-server
+----------------------------------------------+   +----------------------------+   +-----------+
| mod-playerbots (stock body: combat, quests)  |   | IO thread: socket client   |   | Qwen3.6   |
|        ^ steer (RPG state, chat)             |   | main sequence: per-bot     |   | 35B-A3B   |
| mod-llm-brain (ours, thin):                  |   |   brain, prompt builder,   |<->| HTTP      |
|  world thread: build snapshots, apply cmds   |   |   parser, scheduler        |   | localhost |
|  IO thread: Unix socket server, two queues   |<->| thread pool: HTTP calls    |   +-----------+
+----------------------------------------------+   | logs: var/bots/<name>/     |
            Unix socket server/run/brain.sock      +----------------------------+
            newline-delimited JSON
```

**Threading rule (server):** snapshots are built and commands applied only on the world thread
(`WorldScript::OnUpdate`, outside map updates, the same place playerbots' own
`PlayerbotWorldThreadProcessor` runs). The module's IO thread only moves bytes between the
socket and two lock-protected queues. Nothing in the module ever waits on the harness.

## Work items, in order

### A. Server module `server_module/mod-llm-brain` (AzerothCore module, Google/Chromium style)
1. Module skeleton (CMake, script loader), linked into worldserver via `tools/setup.sh` symlink.
   Config file `mod-llm-brain.conf`: enabled, socket path, bot names to attach to, tick interval.
2. IO thread + Unix socket server (Boost.Asio, already a server dependency); outbound/inbound
   queues; drop-oldest when the harness is gone (no unbounded growth).
3. Snapshot v0 (world thread, per attached bot, every few seconds and on events):
   identity (name, race, class, level, gold), HP/mana, zone/subzone, position, current RPG
   activity, quest log summary, nearby units (name, level, hostile/friendly, quest giver),
   recent chat addressed to the bot. Plus a numbered **options** list computed in C++
   (e.g. "1. do quest X", "2. grind near Y", "3. wander", "4. rest").
4. Events v0: whisper/say to the bot, activity finished (RPG status changed to idle).
5. Commands v0 (applied on the world thread): `choose_option {n}` -> map to playerbots RPG
   switches (`ChangeToDoQuest`, `ChangeToGoGrind`, `ChangeToWanderRandom`, `ChangeToRest`);
   `say {channel, text, to}` -> bot speaks (whisper reply / say / party).
6. Bot management for phase 2: attach to the one configured random bot (cheats already
   disabled). Our own bot manager replaces RandomPlayerbotMgr in phase 3.

### B. Harness `harness/` (separate process, Google/Chromium style, deps from third_party/)
1. CMake project into `out/harness`; links Abseil, libevent, cpp-httplib, nlohmann/json, GoogleTest.
2. `base/`: SequencedTaskRunner, ThreadPool, PostTask / PostTaskAndReplyWithResult,
   OnceCallback / RepeatingCallback (absl::AnyInvocable), WeakPtrFactory. Unit-tested.
3. `net/`: libevent IO thread, Unix socket client with reconnect + newline-JSON framing.
4. `llm/`: llama-server client (cpp-httplib on the thread pool, results posted back),
   per-bot slot pinning (keep each bot's prompt prefix cached), timeouts.
5. `brain/`: `decide(snapshot) -> Decision` (the single entry point used live and by evals):
   prompt builder (content/RULES.md -> bot identity -> snapshot -> options), reply parser
   (option number; strict validation; one retry, else no-op so the stock AI continues),
   chat reply generation (separate call after the decision).
6. `sched/`: priority queue v0 (chat reply > activity choice), bounded concurrency, stale
   request replacement per bot.
7. Logging: `var/bots/<name>/events/<date>.jsonl` (snapshot, prompt hash, reply, choice,
   latency), atomic writes.
8. CLI: `harness run` (live) and `harness decide <snapshot.json>` (offline, for evals).

### C. Content and memory
1. `content/RULES.md` v0 (framing, action space, behavior rules, chat style, output format).
2. `var/bots/<name>/identity.md` for the test bot (hand-written personality for now;
   generated personalities come later). `var/bots/` gets its own git repo.

### D. Tooling
1. `tools/build-harness.sh`, `tools/harness.sh` (run), and start/stop alongside the server
   and llama-server.
2. Save real snapshots from play as the first eval fixtures (`evals/cases/` seed).

## Rough size
A: ~1-1.5k lines C++. B: ~2-3k lines C++ plus tests. C/D: small. Build A first, against a
tiny fake harness (netcat-level), then B.

## Open questions for review
1. **Protocol:** newline-delimited JSON over a Unix socket (simple, debuggable with `socat`).
   Protobuf later only if it ever matters. OK?
2. **Phase-2 bot:** keep one random-playerbot (cheats disabled) as the test bot, and build our
   own bot manager in phase 3. OK?
3. **Brain trigger cadence v0:** on events (whisper, activity finished) plus an idle timer
   (every 60 s). OK, or should idle be slower?
4. **Who writes the C++:** I can write it all, or split it (e.g. you take `base/`, since you
   know the //base model best, and review the rest).
