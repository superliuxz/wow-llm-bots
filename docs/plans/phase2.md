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
            length-prefixed protobuf frames
```

**Threading rule (server):** snapshots are built and commands applied only on the world thread
(`WorldScript::OnUpdate`, outside map updates, the same place playerbots' own
`PlayerbotWorldThreadProcessor` runs). The module's IO thread only moves bytes between the
socket and two lock-protected queues. Nothing in the module ever waits on the harness.

## Work items, in order

### A. Server module `server_module/mod-llm-brain` (AzerothCore module, Google/Chromium style)
1. Module skeleton (CMake, script loader), linked into worldserver via `tools/setup.sh` symlink.
   Config file `mod-llm-brain.conf`: enabled, socket path, bot names to attach to, tick interval.
2. IO thread + Unix socket server (epoll IO thread on `common/net`, done in A2); outbound/inbound
   queues; drop-oldest when the harness is gone (no unbounded growth).
3. Snapshot v0 (world thread, per attached bot, streamed every 1 s; LLM is not called per snapshot):
   identity (name, race, class, level, gold), HP/mana, zone/subzone, position, current RPG
   activity, quest log summary, nearby units (name, level, hostile/friendly, quest giver),
   recent chat addressed to the bot. Plus a numbered **options** list computed in C++
   (e.g. "1. do quest X", "2. grind near Y", "3. wander", "4. rest").
4. Events v0 (sent immediately, also visible in the next snapshot): whisper/say to the bot, activity finished (RPG status changed to idle).
5. Commands v0 (applied on the world thread): `choose_option {n}` -> map to playerbots RPG
   switches (`ChangeToDoQuest`, `ChangeToGoGrind`, `ChangeToWanderRandom`, `ChangeToRest`);
   `say {channel, text, to}` -> bot speaks (whisper reply / say / party).
6. Bot management for phase 2: attach to the one configured random bot (cheats already
   disabled). Our own bot manager replaces RandomPlayerbotMgr in phase 3.

### B. Harness `harness/` (separate process, Google/Chromium style, deps from third_party/)
1. CMake project into `out/harness`; links `common/` (wlb_base, wlb_net), Abseil, cpp-httplib, nlohmann/json, GoogleTest.
2. `base/` and `net/` now live in the shared `common/` library (done, see below), used by both
   the server module and the harness.
3. (in `common/net/`) Unix socket client with reconnect + length-prefixed framing (done in A3).
4. `llm/`: llama-server client (cpp-httplib on the thread pool, results posted back),
   per-bot slot pinning (keep each bot's prompt prefix cached), timeouts.
5. `brain/`: `decide(snapshot) -> Decision` (the single entry point used live and by evals):
   prompt builder (content/RULES.md -> bot identity -> snapshot -> options), reply parser
   (option number, plus one fill-in per blank when an option has blanks, none in phase 2;
   strict validation; one retry, else no-op so the stock AI continues),
   chat reply generation (separate call after the decision).
6. `triggers/` + `sched/`: change detection on each snapshot (decides whether to think), 10 s fallback timer; priority queue v0 (chat reply > activity choice), bounded concurrency, stale
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

### E. End-to-end tests (planned)
Three layers, all on an **isolated test stack** (own ports, socket path and config; databases
restored from a "golden" copy of the freshly imported DBs for every run, so tests never touch
the user's play server and always start from the same state). One runner: `tools/e2e.sh`.
1. **Module e2e** (from A3-A5): real worldserver + fake harness; snapshot fields correct,
   `choose_option` / `say` change the bot in game.
2. **Brain e2e** (with B): real server + harness + **fake LLM** (canned HTTP replies):
   deterministic decision loop, no GPU.
3. **Full stack** (milestones): + llama-server + an AzerothGhost headless player
   (`third_party/azerothcore-wotlk/e2e`, Go) that logs in and whispers the bot; checks the
   phase 2 "done when" items. AzerothGhost needs `Warden.Enabled = 0` (known login bug).

## Rough size
A: ~1-1.5k lines C++. B: ~2-3k lines C++ plus tests. C/D: small. Build A first, against a
tiny fake harness (netcat-level), then B.

## Shared C++ library `common/` (built 2026-11-14)
Both ends of the socket use the same Chromium-style code (rules and reasoning: CLAUDE.md). Layout
mirrors Chromium: `base/functional/` (Once/RepeatingCallback, BindOnce/BindRepeating with WeakPtr
receivers), `base/memory/` (WeakPtr, RefCountedThreadSafe/scoped_refptr), `base/task/`
(SequencedTaskRunner, TaskQueue, ThreadPool, PostTaskAndReplyWithResult), `base/threading/`
(PlatformThread, Thread with Options{MessagePumpType}, CurrentIOThread), `base/message_loop/`
(MessagePumpDefault, MessagePumpEpoll = MessagePumpForIO with WatchFileDescriptor),
`base/synchronization/` (Lock/AutoLock with thread-safety annotations, ConditionVariable,
WaitableEvent), `base/time/` (TimeDelta, TimeTicks on CLOCK_MONOTONIC), `base/compiler_specific.h`
(MSAN_UNPOISON), `base/thread_annotations.h`. `net/`: LineFramer, LineConnection,
UnixSocketServer, UnixSocketClient. No third-party runtime deps (libevent dropped). Plain C++20.
28 tests; `tools/check-cpp.sh` = format + include-what-you-use + default/ASan+UBSan/TSan/MSan
(MSan against instrumented libc++); `-Werror=thread-safety`; hardened std lib in `out/common`,
not in `out/common-server` (linked into AzerothCore).

## Decisions (review 2026-11-14)
1. **Wire format** module <-> harness (revised 2026-11-15): binary protobuf messages from
   `server_module/mod-llm-brain/proto/snapshot.proto` (lite runtime, as Chromium), one per
   length-prefixed frame (`common/net/frame_codec.h`: 4-byte big-endian length + payload)
   over the Unix socket. JSON (canonical protobuf mapping) only in harness tooling: recordings, decision
   logs, eval cases. The LLM never
   sees this JSON: the harness renders snapshots into compact plain-text prompts (fewer tokens,
   natural to read). Prompt format is an eval question later.
2. **Bot manager:** phase 2 borrows one random bot (cheats disabled). Phase 3: our module owns
   bot lifecycle (character creation by each bot's brain, human-like login schedules, no cheats)
   and RandomPlayerbotMgr is no longer used for our bots.
3. **Snapshots vs thinking:** the module streams every attached bot's snapshot to the harness every **1 s** (no LLM). The harness keeps the latest per bot and runs cheap change detection on each one (whisper/chat to the bot, activity finished, bags full, low health, friend nearby, ...). The LLM is called **only when a snapshot has something to act on**, plus a **10 s fallback** check-in if nothing triggered (50 bots ~ 5 calls/s). Trigger rules live in the harness (no server rebuild to tune). For testing: console command `brain think <bot>` and a verbose decision log.
4. **Authorship:** Claude writes the code in small reviewable commits (each builds and is tested);
   the user reviews.
