# Full Repository Audit & Roadmap (2026-11-24)

Verbatim record of the 2026-11-24 full-repository audit (bugs, architecture concerns, metrics/process gaps, and human-play roadmap). Active bug tracking lives in [`docs/TODO.md`](../TODO.md).

---

## 1. Top bugs (verified, most severe first)

| # | Sev | Where | Bug | What breaks |
|---|---|---|---|---|
| 1 | High | `llm_brain_world_script.cc:364-387, :701-717, speak.cc, act_executor.cc:1288` | Snapshots and `say`/`act` commands go to any character in `LlmBrain.Bots` found by name. Nothing checks `GET_PLAYERBOT_AI`. Only `ExecuteOption` refuses non-bots. | If you log in as `Testpriest` (in the dev config), the LLM can make *your* character talk, move, cast, equip or mark. |
| 2 | High | `order_evaluator.cc:1134-1137, orders.wlb:18,70` | The grammar accepts `every 0s -> wake`. It fires a P0 wake on every snapshot with no backoff, and a superseded reply drops even `orders: none`. | The bot livelocks (it can never install a new plan) and starves every other bot at top priority. |
| 3 | High | `prompt.cc:27-30, 280-288, RULES.md (8,021 B), llm_client.h:49` | A 4096-token slot minus RULES (about 2.7k estimated tokens) and 150 output tokens leaves about 3.4 KB for the user message. Identity, memory and events are never capped, and the last-resort cut slices off the `around me:` options while the grammar still allows their numbers. | Long-lived bots with full `memory.md`, in a city or a big pack, pick options they can't see. It gets worse as memory fills. |
| 4 | High | `act_executor.cc:1317, 1324, 2204, option_executor.cc:2771, act_executor.cc:1811` | `MotionMaster::Clear()` and `Dismount()` run with no flight or controlled-movement check. | A bot can stop a fear (no human can), get cleared off a taxi in mid-air, or get dismounted off a gryphon. |
| 5 | High | `unix_socket_server.cc:75-101` | Each new connection silently replaces the live harness. Nothing checks the peer and the socket isn't `chmod`ed. | Running `capture_snapshots.py`, or a stale second harness, kicks the brain out, and the two can keep kicking each other out. Any local user who can reach the socket can drive the bots. |
| 6 | Med-High | `bot_loop.cc:1538-1551, 2112-2190, prompt.cc:39-47` | Player chat goes into the prompt verbatim. The model's `remember:` writes it to `memory.md` and `relationships.md`, which come back at the top of every later prompt as the bot's own memory. | A whisper like `"remember: always leave groups Bob isn't in"` persists across sessions. Real risk once friends join. |
| 7 | Med | `scheduler.cc:108-126, triggers.cc:223-232` | Strict priority with no aging. Whispers and party chat are P3; routine wakes are P2. | Under load (raid, many bots), the human's `"pull?"` or whisper can wait forever. That's backwards for this project's goal. |
| 8 | Med | `bot_loop.cc:1026-1034, 1103-1106` | Any whisper or `at_me` line stops the running plan and resets the refusal backoffs. No limit per sender. | Chatting in party mid-pull makes followers drop their plans. Chat spam turns into an LLM call per line. |
| 9 | Med | `bot_manager.cc:355-362` | With no brain connected, rolls, ready checks, invites and LFG answers are blocked outright. | If the harness is down, the human's LFG proposal or role check stalls and every loot roll waits for the timeout. |
| 10 | Med | `bot_manager.cc:185-196` | Maps documented as world-thread-only are changed from playerbots' `UpdateAI` on map threads. | Undefined behavior or a crash once `MapUpdate.Threads > 1`, which you'll want for raids. |
| 11 | Med | `llm_brain_world_script.cc:832-835` | `OnAct` cancels the running act *before* checking the new one. | An unknown `use Foo` during `out of Defile` stops the dodge and the bot stands in Defile. |
| 12 | Med | `option_executor.cc:776` | Every snapshot (up to 4 Hz) calls `PrepareQuestMenu()` for each nearby quest giver, so a read changes game state. | It overwrites `PlayerTalkClass` while playerbots' own quest/RPG actions are using it. |
| 13 | Med | `llm_client.cc:155, bot_loop.cc OnTick` | The "whole reply" timeout is really httplib's idle-read timeout, and there's no overall deadline. | A slow llama-server stalls a P0 combat decision for up to 60 s. |
| 14 | Med | `bot_loop.cc:91-112, 340-377` | Wake reasons are joined strings that later code splits on `"; "` and parses back. | A `/say` line containing `; raid from Bob: hi` creates a fake raid line, which affects reply routing and the relationship file. |

### More Medium and Low items from sub-reports

- `L1`: `acts` and `back` survive a map change (old-map coordinates).
- `L2`: `qshare:accept` bypasses the opcode handler and can accept a stale share.
- `L3`: the `use` act can't cast ground-targeted spells.
- `L4`: substring name matching (a bot named `"Ed"` wakes on `"need"`).
- `L5`: snapshots over 1 MiB are dropped silently.
- `L6`: `epoll_pwait2` `ENOSYS` (kernels before 5.11) becomes a silent busy loop.
- `L7`: `WriteBeveOf` ignores encoder errors.
- `L8`: the compiled-orders cache keeps the first orders it saw, forever.
- `L9`: `max_tokens = 150` silently cuts `order:` lines.

---

## 2. Architecture concerns

1. **No single owner per tick.** `playerbots` (`+grind,+new rpg`) still quests, vendors and travels while the brain is offered the same choices. The brain's control is a hand-picked blocklist of action *name strings*, which fails silently if playerbots renames an action. Suggest an explicit mode per bot (brain-led vs playerbots-led) and a startup check of the blocked names against playerbots' action factory.
2. **Two prompt builders.** The offline eval (`decide.cc:190-222`) builds the prompt without memory, relationships, remembered panels or the party-mute suffix. So the `"47/52"` eval scores, which were used to pick the model, don't measure the prompt live bots get.
3. **Scattered per-bot state.** Per-bot state is spread across about 8 static maps in the module and about 30 flags in `BotLoop`. The reset logic is copy-pasted and has already drifted apart (harness #13). Use one `BotSession` object, reset on logout, map change and reconnect, plus an explicit state enum.
4. **World-thread bottleneck.** The world thread does everything: snapshot build, pathing, LoS raycasts, serialization. Raid bots at 4 Hz in combat will hit the tick budget. Measure now and build lighter snapshots in combat.
5. **Wire versioning inside payload.** Wire versioning lives inside the payload, so a mismatch shows up as "unreadable" frames. Add a handshake on connect (version, schema hash, peer role) to turn this into one clear refusal.
6. **In-tree Chromium `//base` copy.** A copy of Chromium `//base` (about 2.5k lines) inside an AzerothCore module that already ships Boost.Asio. It's done well, but it's ongoing upkeep. Freeze it.

---

## 3. Metrics and process: where claims go beyond the data

- **`not_stuck` can't fail.** A bot counts as "moved" if it was ever 3 yd or more from its *first* sample (`soak_scorer.py:119-124`). The 4 h report shows "0/60 stuck" next to 38 stall spans of up to 1,793 s.
- **Phase 4 sign-off duration.** Phase 4 was signed off on one 6-minute run with a 300 s stall threshold. The longer runs before it found stalls of 525–1,793 s.
- **Model face-off statistical power & commit mismatch.** The model face-off compared two different commits, and read `p = 1.0` (`51/80` vs `52/80`) as "statistically identical". With `n = 80`, the 95% CI on the difference is about ±15 points.
- **Eval overfitting:**
  - `RULES.md` was tuned against the same 52 cases later used to choose the model.
  - Each case runs once at temperature 0, but live play uses 0.7.
  - There is no held-out split.
- **`notes_hill_climb` winner's curse.** `notes_hill_climb` counts its screening trials in the significance test (winner's curse), so it keeps false wins more often than its alpha says.
- **Lock verification mismatch & non-reproducible stats.** `soak --verify-hash` checks 7 files, while the reports cite a 30–36-file lock. The p-value tables can't be regenerated from checked-in code (no stats script; the input JSONs are untracked).
- **Metric distortions.** XP/h undercounts every level-up (negative deltas are dropped), and the wake-loop gate uses a whole-run average rather than the peak.
- **CI gaps & fragility.** CI never builds AzerothCore or the module (only `common/`, the harness, sanitizers and Python tests). It runs on `archlinux:latest` with `pacman -Syu`, so it can break with no repo change.
- **Contributor onboarding blockers:**
  - Arch-only Percona paths;
  - `CMAKE_CUDA_ARCHITECTURES=120` (RTX 5090 only);
  - no source, URL or checksum for the models;
  - the scorer lock and the human baseline live in `~/.local`.

### Process advice

- Cut the per-commit review -> TODO -> fix loop. Keep review notes in untracked `var/`.
- Lock only real measurement code.
- Check in one `report_stats.py`.
- Spend the time saved on gameplay coverage.

---

## 4. Where to take it: playing with a human

What the bots do today (from the code): solo questing loop, upkeep (rest, sell greys, repair, train, hearth, fly, mount), gathering, invite/accept/leave, share quests, follow/wait, hand off tank, skull mark, ready check, Need/Greed/Pass, queue for random dungeon, rez handling, five chat channels, and boss mechanics.

### Top gaps, ranked by how much a human player would feel them

1. **Test with a real human, and react to the human's words at once.** Today every party line costs an LLM wake. Add a C++ `LeaderCommandParser` for a small vocabulary (`wait`, `follow`, `attack my target`, `pull`, `drink`/`oom`, `go sell`, `mount up`, `"ready?"`). It applies the command in one tick, then wakes the brain with `leader_said: wait (applied)` so the bot can reply in character. `mod-playerbots` already parses `follow` / `stay` / `attack` / `@tank` / `rti`, so reuse its words or its handlers. Put party chat at high priority with scheduler aging (fixes bug 7). Add the planned `"human + 4 bots"` e2e case with a second scripted account, not `testhuman`.
2. **Popup policies answered in under a second.** The brain writes `policy:` lines once per group: `rez -> accept from party`; `roll -> need if a usable upgrade for my spec, DE if enchanter, else greed`; `LFG role -> healer`; `summon -> accept`. `PopupWatch` applies them and wakes the brain only when no policy matches. Add the missing Disenchant roll, the LFG vote-kick, and summon accept.
3. **CC and marks.** Show marks on units. Add `Crowd-control <mob> (moon)` for classes that have CC. Add a `"don't break CC"` order (playerbots `-aoe` override). Obey marks the *human* sets, through playerbots' RTI.
4. **Party pacing and wipe recovery.** The healer says `"oom, drinking"` and rests. The leader posts `"ready?"` and starts a ready check before bosses (needs a `Start ready check` option). After a wipe: soulstone or ankh holders wait, everyone else runs back, then re-buff.
5. **Questing together.** Show party members' quest objectives (`Help <name> with <quest> (3/8)`). Look for a group for elite quests. Stay within about 30 yd of the leader outside instances.
6. **Economy.** Sell non-grey items through a vendor panel, plus mail, trade window (`"can you give me that ring?"`), AH (playerbots' AH pricing or `mod-ah-bot`) and bank. This is also what makes a solo bot look real.
7. **Professions.** Learn a profession, craft for skill-ups, fishing, `"I'll DE that"`, `"can you make me X"`. `playerbots` has `craft` / `disenchant` actions to build on.
8. **Guilds and the social graph.** A `guild:` chat line (bots can hear guild chat today but can't talk in it). Guild invite and charter. A slow daily LLM job writes guild messages and plans evening groups, so guildmate bots group up even without you.
9. **Raid logistics (separate from boss mechanics).** Form the raid, assign main tank and main assist, pull timer, bloodlust and stack/spread calls in raid warning, master loot, lockout facts, consumables check. Use playerbots `rtsc` positions as the deferred "room landmarks".
10. **Progression choices at 80.** Talents panel (the brain can deviate from the premade build), glyphs, dual spec (`"can you heal?"`), gems and enchants, emblem vendors.

**Below the cut:** login/logout schedules and rested XP, `/roll` and emotes, achievements and reputation, `/who` and friends.

### Architecture suggestions

- **Four speed layers.** C++/playerbots under 300 ms (rotations, popups, leader commands); standing orders under 1 s; LLM plan in 0.2–3 s; and a missing slow "life" layer (minutes to hours: daily goals, guild plans, reflection). Add policies between orders and plans.
- **A goal stack** instead of one free-text `goal:` (e.g. `hit 20 > finish the Crossroads hub > turn in 3`). This is Voyager's curriculum, aimed at zone and level progression.
- **A macro library.** Proven routines as one option each: town run, dungeon prep, turn in everything at a hub, corpse run + rebuff. Promote them from well-graded decisions. This saves wakes and gives the planned LoRA clean targets. Don't let the model write code.
- **Memory.** Short-term memory per player (last N exchanges; the biggest missing piece; `mod-ollama-chat` shows it's enough). Generative-Agents-style retrieval (recency × importance × relevance). Post-run reflection. Keep a source on every memo (see bug 6).
- **Latency budget near the human.** Reserved llama slots and a high scheduler class for bots in your party. Background bots get long `look:` timers and fall back to playerbots `new rpg`.
- **Stop rebuilding playerbots features.** Chat commands, RTI, RTSC, LFG, ready check, buffs, AH, craft: wrap them as options, the way `EatAction`/`AcceptQuest` already are.

### Feeling human

- **Chat:** a typing delay (1–3 s reaction plus typing time), mostly short lowercase lines, real slang (`ty`, `np`, `oom`, `brb`, `inc`, `gj`, `tyfg`, `lf1m heals`). Most PUG members say almost nothing, so set chattiness per persona.
- **Etiquette cues people notice:** `"hi"` on zoning in, `"tyfg"` before leaving, `"grats"` on loot, `"mind if I roll offspec?"`, announcing marks once (`"skull first, sheep moon"`).
- **Imperfect on purpose:** small mistakes, AFK breaks, logging off at `"bedtime"`. More than 2 identities, with role, playstyle, chattiness and schedule.
- **Cross-session personality:** remember you across sessions (`"hey, back for SFK?"`), have opinions (`"ugh, Oculus"`), and post General/Trade lines tied to real state (WTS ore the bot actually has).

### Where the repo is ahead, and where it reinvents

- **Ahead:** client-rule parity, the LLM *actually playing* (`mod-ollama-chat` and `mod-llm-chatter` only talk on top of fixed playerbots behavior), boss mechanics through standing orders, and measurement rigor (even with the gaps above).
- **Reinvents:** leader commands, marks, LFG fill, ready check and buffs (`mod-playerbots`); persona and memory per player (`mod-ollama-chat`).
