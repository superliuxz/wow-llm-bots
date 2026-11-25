# wow-llm-bots: Design

A local WoW 3.3.5a (WotLK) private server (AzerothCore + `mod-playerbots`) where LLM-driven bots
level, quest, chat, run dungeons, and raid alongside a human player on a single local GPU.

## Core Principles

1. **The brain decides; C++ is a pure harness.** C++ perceives, measures, enforces WoW client
   rules, runs geometry/pathing, and executes the brain's choices. Gameplay judgments (what is
   worth doing, target/quest/reward/gear choice, risk, when to rest, when to reply, boss tactics)
   belong to the LLM brain, never hard-coded heuristics in C++.
2. **Client-rule parity, no cheats.** Bots see only what a real 3.3.5a client sees (plus explicit
   client-parity addons where tagged): line-of-sight checks, nameplate/minimap ranges, hover-only
   tooltips (`ValidateSnapshotVisibility`), and zero invisible server triggers (146 display IDs
   filtered from client DBCs). `Playerbots.BotCheats` stays empty and `RandomPlayerbotMgr` stays
   off (`Playerbots.RandomBotAutologin = 0`) because random-bot manager cheats (free refresh,
   random teleports, free gold, random repositioning) break persistent character continuity.
3. **One brain per bot, shared GPU model.** Each bot has its own identity, memory, relationships,
   and decision log (`var/bots/<name>/`), while all bots share one local `llama-server` model
   with shared slots (measured 2–3× faster than pinned per-bot slots) and an identical `system`
   prompt (`content/RULES.md`) for prefix caching.
4. **Grammar-constrained plans.** Every LLM reply conforms to a per-call GBNF grammar compiled
   from `harness/grammar/plan.wlb` (`docs/specs/grammar-idl.md`), restricting `do:` to the
   current snapshot's numbered options and `order:` to validated standing-order syntax.

## Architecture & Speed Layers

```
worldserver (mod-llm-brain)  <-- Unix socket, BEVE frames -->  harness  <-- SSE HTTP -->  llama-server
  snapshots, immediate events  ------------------------------>  per-bot state, triggers,
  <----------------------------------- commands (choose/act/say)  prompts, orders, logs
```

Four latency tiers separate reflexes from deliberation:

| Tier | Latency | Owner | Role |
|---|---:|---|---|
| **Body (combat & movement)** | `< 50 ms` | `mod-playerbots` + `mod-llm-brain` | Class rotations, pathing, opcode-level execution, popup capture (`PopupWatch`). |
| **Standing orders** | `< 250 ms` | `harness` (`OrderEvaluator`) + `mod-llm-brain` (`act_executor`) | Brain-written `<when> -> <act>` boss/combat rules evaluated locally on every combat snapshot without an LLM round trip. |
| **Brain plans** | `0.1–2 s` | `harness` (`BotLoop`, `Scheduler`, `LlmClient`) | Woken by triggers (plan done/failed, urgent combat/death/whisper/popup, `watch:`, or `look:` timer). Streams `do:` on the first line (`FirstLineCallback`) and runs the plan until the next wake. |
| **Lifelong memory** | minutes–days | `harness` (`memory.md`, `relationships.md`, `identity.md`) | Condensed narrative memories and per-player relationship notes injected into the unified prompt builder (`AssemblePromptInput`). |

## Key Design Decisions

- **Unified prompt assembly:** Both live play (`bot_loop.cc`) and offline eval (`decide.cc`)
  assemble prompts through `AssemblePromptInput` (`harness/src/screen/prompt.{h,cc}`) with
  explicit token budgeting so long memories never truncate `around me:` options.
- **One state object per bot:** `BotSessionRegistry` (`server_module/src/bot_session.{h,cc}`)
  holds all per-bot module state with unified logout/map-change/disconnect resets; `BotLoop`
  uses an explicit `CallState` enum and `ResetBotSession`.
- **One typed action system:** Chosen options (`Command::Choose`) and standing-order acts
  (`Command::Act`) share a typed `BotAction` variant (`server_module/src/options/action_types.h`),
  a single validate-before-cancel dispatcher (`option_executor.cc`), and area-split executors
  (`exec_shared`, `exec_npc`, `exec_items`, `exec_social`, `exec_movement`, `act_executor`).
- **Bot lifecycle (`BotManager`):** Replaces `RandomPlayerbotMgr` (which auto-teleports,
  refreshes, and randomizes bots behind the brain's back) by logging configured characters in
  through playerbots' public login and re-applying configured strategies cleanly.
- **One tick owner per bot:** each managed bot is `brain-led` (a harness of this build is
  connected) or `playerbots-led` (none, a refused one, or a battleground); `BotManager::Update`
  sets the mode every world tick and logs each change. `BrainPolicyListener` gates playerbots
  actions from one table (`server_module/src/bot_manager/tick_owner.h`: popups are the brain's
  only while it leads, auto-equip never runs, the rest are world-state checks), and startup
  checks every gated name against playerbots' action factories so an upstream rename is logged.
- **Client-event coverage (2026-11-15 audit):** All 713 WoW 3.3.5a client UI events were audited
  once against the snapshot schema to ensure every gameplay-relevant client alert (combat, death,
  chat, popups, loot rolls, LFG, ready check, prompts) maps to either an immediate `Event` or a
  snapshot field; speculative unused UI panels (barber, auction house, calendar, etc.) were later
  pruned from `snapshot/snapshot.h` to keep the wire schema lean.
- **Combat & soak tracing:** `BehaviorTraceWriter` records compact 5 Hz samples plus combat/soak
  events (`spell_cast`, `damage_dealt`, `damage_taken`, `heal_done`, `mob_kill`, `died`,
  `xp_gain`, `quest_turnin`, `combat_start`, `combat_end`, `act_sent`, `option_chosen`) for `wlb trace` and
  `bench_bosses.py`.
- **Parked research (`experimental/`):** The automated human-likeness scorer stack,
  `notes_hill_climb`, and the self-improving loop design doc live in `experimental/` (excluded
  from `wlb check` and CI) so core development stays focused on enjoyable play with a human.

## Decided Product Rules

- **Realm:** PvE, Alliance only (Horde character creation disabled); bots choose Alliance races
  freely. New bots start at their race's starting area like a new character.
- **No cheats, human ways out:** release and corpse-run or resurrect at the spirit healer,
  hearthstone, the client's "Character Stuck", or asking for help. Playerbots' random-bot refresh,
  free revives and teleports stay off.
- **Bad behavior is chat-only:** banter and trolling in General is fine (no slurs or hateful
  content); never in gameplay: no ninja looting, rage-quitting, abandoning groups or sabotage.
- **Chat like a human:** as often as a human would (most bots quiet: "hi/ty/gg/brb"), lowercase,
  abbreviations and typos by personality, mostly between fights, typing delay by length.
- **Calls are followed by choice:** each bot decides whether to follow a call (who called, trust,
  its own perception, personality, skill, herd effect); bots can follow bad calls.
- **Knowledge by experience:** bots learn mechanics by dying to them, being told, or looking them
  up; ignorance that matters is enforced in code (options and reflexes), not by asking the model
  to forget.
- **Economy:** 100g starting gold for every bot and the user; the auction house and professions
  are used like humans do, by personality.

## Further Specs

- Wire structs & codecs: [snapshot/snapshot.h](../snapshot/snapshot.h) and [docs/specs/snapshot.md](specs/snapshot.md)
- Triggers & standing orders: [docs/specs/triggers.md](specs/triggers.md)
- Options & panels: [docs/specs/options.md](specs/options.md)
- Grammar IDL: [docs/specs/grammar-idl.md](specs/grammar-idl.md)
- Harness runtime: [docs/specs/harness.md](specs/harness.md)
- Current measured numbers: [docs/results.md](results.md)

## Legal note

A private server for yourself and friends is generally tolerated; public or monetized servers get
shut down. Keep it private.
