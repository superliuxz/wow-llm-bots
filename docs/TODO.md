# TODO

Open work only (Now / Next / Later). Completed work lives in `git log`.
Last updated: 2026-11-24.

## In progress: Review 2 fixes (2026-11-24)

Findings from "Wow Llm Bots Review 2". Tick when fixed and tested.

### Team-play breakers

- [x] **T1 (High)** `config/llm-brain.overrides`, `worldserver.overrides`: the play config has 2 brain
  bots (`Testpriest`, `Gatda`), 2 identities, no random bots and no `Debug.LFG`. You can't form
  human + 4, and a Random Dungeon queue with 3 players never pops.
- [x] **T2 (High)** `config_parsing.cc`: only non-leader members get `-grind,-new rpg`. A bot leader
  keeps solo grind/RPG-travel strategies, wanders, hearths or flies off; in a dungeon it pulls
  trash by itself.
- [x] **T3 (High)** `bot_manager.cc`, `CombatStrategies = "+boost,+dps assist,-threat"`: every
  non-tank bot runs `-threat,+boost`, grouped or not. DPS opens with cooldowns, ignores threat
  and pulls mobs off you.
- [x] **T4 (High)** `bot_manager.cc` group policy: clearing `Wait here` / `Let X tank` emits nothing
  (the bot never comes back, a prot bot never tanks again). The policy silently disappears when
  the group changes while `+stay` stays on.
- [x] **T5 (High, mostly not a bug)** Once grouped with a real player, playerbots'
  `UpdateAIGroupMaster` -> `FindNewMaster` makes that player the bot's master, so `+follow`
  (taxi follow included) and master commands work; only an all-bot group has no master. Still to
  verify with a real client (see the human + 4 e2e item below). Original finding: `exec_social.cc`, `bot_manager.cc` (`AddPlayerBot(guid, 0)`, no master): follow
  works only on the same map within 8-25 yd. No option for taxi, hearth, boat/zeppelin or
  portals; playerbots' taxi and teleport-to-master follow never run.
- [x] **T6 (Med)** `exec_social.cc`: Dungeon Finder roles come from class, not spec.
- [x] **T7 (Med)** `exec_social.cc`: "upgrade" is item level only, no Disenchant vote, at most 2
  rolls shown at once. A warrior Needs a cloth spell-power robe over the mage.
- [x] **T8 (Med)** `triggers.cc`: no wake when the party enters combat, a member dies, or the healer
  is low on mana. A bot wakes only on its own state.
- [x] **T9 (Med)** `chat_scripts.cc`, `bot_loop.cc`: every non-trivial party line wakes all bots at
  P1; only the replies are muted. 4 prompts per "next pack".
- [x] **T10 (Med)** `chat_scripts.cc`: raid warnings are dropped; boss emotes and monster yells are
  never forwarded, yet the triggers treat them as P0.
- [x] **T11 (Med)** missing: nothing handles the human logging out, leaving the group, or leaving
  the Dungeon Finder group. Bots idle forever or stay stuck in the dungeon.
- [x] **T12 (Med)** `bot_manager.cc`: holding playerbots' auto-release only happens in dungeons. In
  the open world a bot runs back from the graveyard while your priest casts Resurrection.
- [x] **T13 (Low)** `chat_scripts.cc`: raid `/p` reaches every subgroup; `ext.raid` is never
  filled, so bots never use `/raid`.
- [x] **T14 (Low)** `exec_social.cc`: every member is offered "Mark nearest hostile with Skull",
  even on unpulled mobs.
- [x] **T15 (Low)** `bot_loop.cc`: the party-chat responder is the alphabetically first bot, with
  no check that it is online.
- [x] **T16 (Low, suspected; mostly not a bug)** With the human as master (T5), `!pb follow` /
  `!pb stay` work; plain words don't until the fast command path (Now #2). Original finding: no master + `CommandPrefix = "!pb "`: playerbots' `follow` / `stay` /
  `rti skull` from you probably do nothing; no fallback until the fast command path exists.

### New bugs from the audit fix

- [x] Scheduler aging inverts combat priority (`scheduler.cc`): P1-P3 age to P0 in 2-6 s and ties
  go to the oldest, so a fresh tank combat wake waits behind old "plan done" entries. Stop aging
  at P1.
- [x] The 350-token reserve applies almost always (`prompt.cc`): `order_names` includes every
  nearby unit, so the boss notes are dropped on the pre-pull compile wake. Reserve only on
  compile wakes and fill notes first there.
- [x] An order wake held back by backoff still stops the plan (`bot_loop.cc`): with
  `every 2s -> wake` the bot stands idle most of the time.
- [x] A rate-limited chat line is never answered (`bot_loop.cc`): the human's 4th quick line
  ("STOP") wakes nothing.
- [x] Chat arriving during a call drops that call's plan (superseded): a human typing every few
  seconds can keep a bot from ever installing a plan.
- [x] `SanitizeRememberNote` rejects what the human teaches ("remember: always heal me first" is
  dropped as an imperative echo); a paraphrase still gets through, and condensing strips the
  `[heard in chat]` tag. Filter by sender (trust leader and party), not keywords.
- [x] Group policy is wiped on every logout or relog (`bot_session.cc`).
- [x] A hung (not crashed) harness locks out every replacement (`unix_socket_server.cc`), and popups
  stay blocked.
- [x] playerbots' auto-equip runs whenever the harness is down (`bot_manager.cc`), undoing the
  brain's gear choices.

### Old findings still open

- [x] Rest holds the healer's reflexes while the tank pulls (server M4).
- [x] `to` / `away` / `kite` still queue movement on a taxi or while feared (server H2, partial).
- [x] Party responder has no online check (harness #6, partial; same as T15).
- [x] An invalid plan keeps its early steps (harness #12).
- [x] Random Dungeon picks the highest max level, probably Heroic at 80 (server L2).
- [x] Raid warning is P0 for every bot (harness #20).

### Python, tools and docs

- [x] Stall gate broken by the trace shrink: `stall_check.PROGRESS_EVENTS` lists ~15 events the
  module no longer writes (`quest_accept`, `loot`, `vendor_repair`, `go_interact`, ...); the spec
  test that would catch it was parked.
- [x] `no_wake_loop` reads `peak_wakes_per_min`, and the XP fallback reads `xp_max`; nothing writes
  either.
- [x] `docs/results.md` misreports the Qwen boss row (44/37/42 vs source 46/33/39), dropped the
  one significant result (p_holm = 0.0208), calls p50 "tail latency", and says 52 cases (now 66).
- [x] The doc distill dropped live rules: no secrets or personal data in tracked files / noreply
  email; never copy `private/BigFootAddOns` into the public repo; the legal note and decided
  product rules; the playerbots audit doc that 4 code comments still cite; open TODO items
  ("confirm the single responder in a real session", loot etiquette from play-with-human).
- [x] About 20 stale references to deleted or moved files.
- [x] Screen goldens now check C++ against C++ (`--rewrite`): labeled regression-only, read every
  golden diff by hand.
- [x] The perf baselines (`tools/wlblib/cases/perf_baselines.json`) were recorded by the retired
  Python tool: re-recorded 2026-11-24 with the C++ replay on a fresh capture and verified (brain,
  snapshots, walk). The new brain numbers are slower than the old ones (50 bots wake->plan p50
  1.2 s vs 0.4 s) because the capture and tool changed: an A/B on the same capture shows today's
  harness faster than the pre-review one (6.9 vs 6.3 wakes/s, p50 1.26 s vs 1.48 s).
- [x] Restoring parked code doesn't work as documented: `hill_climb.py` imports an API that no
  longer exists. Tried in a scratch worktree; `experimental/README.md` now lists what a restore
  really needs (the pre-split scorer and the pre-shrink trace events).

### Left from Review 2

- [x] **Human + 4 e2e with a real client account:** `e2e_human_party` logs a scripted 3.3.5a
  client (`tools/wlblib/wowclient.py`, no playerbots AI) in as a real player: the bots accept its
  invite and playerbots makes it their master (T5/T16), one bot answers its party line (single
  responder), and a creature's boss emote wakes the bots (T10). Still worth one session with your
  own client.
- [x] **Play preset characters:** `LlmBrain.CreateBots` created Drogbar, Zenji, Rokzul and Mabaku
  on the play server (all online in Durotar; Gatda already existed as an orc rogue, so the tank is
  Drogbar); `e2e_create_bots` checks a restart keeps them.
- [x] **Intermittent e2e cases** (`lfg_proposal`, `orders`, `playability` failed once in a full
  run): `playability` sent its party line before Telda was in the world (now waits for her
  online); `orders` counted a real combat wake (kiting into a boar) as a kite timeout (combat
  wakes now allowed, timeouts still fail); `lfg_proposal` relied on LFG's one-shot teleport, which
  a bot in combat or falling misses (now clicks `Teleport to LFG dungeon` like a player).
  Verified: fresh full `wlb test e2e`, 27/27.

## Now (team play first: the gaps a human feels in the first hour)

1. **Make human + 4 bots possible and test it.** A play preset with 4+ bots covering tank, healer
   and DPS, with identities (T1-T5). Call `PlayerbotAI::SetMaster(leader)` when a bot joins a
   human-led group (playerbots' taxi/teleport follow and command words for free). A "human + 4"
   e2e case with a raw-packet second account (`wlblib/wire.py` speaks the frames) that invites 4
   bots, queues LFG, ready-checks, rolls, types "wait"/"follow" and rezzes a dead bot.
2. **[done] Chat commands that apply at once.** (`chat/leader_command.{h,cc}`, applied in
   `ApplyLeaderCommand`; `e2e_human_party` checks wait/follow.) A pure `ParseLeaderCommand(text) -> BotAction` in
   `server_module/src/chat/`, run only for the group leader or when the bot is named. Words:
   wait, follow, attack my target, pull, drink, mount, `@heal`, `rti skull`. Runs through
   `ValidateAction` / `ExecuteAction`, then wakes the brain with `leader_said: wait (applied)`
   so the bot replies in character. This is also the trusted channel "chat is untrusted" needs.
3. **[done] Popup policies in C++** (`Answer popups automatically`, `AnswerPopupsByPolicy`,
   summon and kick-vote options; `e2e_popup_policy`; summon and kick vote not live-triggered yet) (rez, ready, roll, role, boot, summon), written once by the brain and
   answered in under 1 s. Add the Disenchant vote, the LFG vote-kick and summon accept.
4. **Follow marks and do CC.** `Attack <mob> (skull)` options sorted by mark,
   `Crowd-control <mob> (moon): Polymorph`, and a "don't break CC" `-aoe` override. Make "human is
   tank -> bots never pull" a C++ gate, not a prompt rule.
5. **Party awareness.** P1 wakes for party combat start, a member died, healer under 30% mana. A
   `party_state:` line on screen. The healer says "oom, drinking"; the tank waits; a leader
   `Start ready check` option before bosses.
6. **Wipe protocol.** Wait if a soulstone or ankh is up, otherwise release and run back. Hold 10 s
   to rebuff and drink. Hold auto-release everywhere when a resurrector is nearby (T12).
7. **Loot manners.** Main-spec, armor type and primary stat facts. Disenchant. BoE -> Greed.
   "grats" at most once. Never Need against something the human said they need.
8. **Questing together.** Party objective progress on screen (`Help X with Y (3/8)`). An escort
   waits until everyone has the quest. A 40 yd stray check outdoors done in C++, without the LLM.
9. **Dungeon Finder life.** Spec-correct roles (T6). Leave and teleport out when the human leaves
   (T11). Deserter and Luck of the Draw facts. Boot votes.
10. **Less chatter, sensibly timed.** A chat budget per party, a 1-3 s typing delay, one responder
    per question, no stating the obvious.

Lessons from official AI companions (Follower Dungeons, FFXIV Duty Support, Dragon's Dogma pawns,
Guild Wars heroes):
- Copy: match the player's pace (when the human tanks, NPCs never pull); wait patiently when the
  player goes AFK; no blame, low stress; direct tactical control (Guild Wars-style flags: instant
  "stay here / go there / kill skull").
- Avoid: body pulls and drifting healers (the top Follower Dungeons complaint); late heals;
  last-second dodges (they look like near-deaths); repetitive, obvious or sycophantic chatter;
  replies slower than about 1.5-2 s for commands; carrying the human so hard dungeons feel empty.

## Next (multi-session depth & architecture)

- **Short-term per-player memory & richer personas:** Store the last N exchanges per player
  (source-tagged so player chat never injects trusted instructions) and add more identities.
- **Single tick owner per bot:** Replace the string-blocklist between `mod-llm-brain` and
  `mod-playerbots` with an explicit per-bot mode (`brain-led` vs `playerbots-led`) and a startup
  check against playerbots' action factory.
- **Socket handshake on connect:** Exchange wire version, schema hash, and peer role on Unix socket
  connect so mismatched builds or stray tools fail with one clear refusal.
- **Latency & world-thread polish:** Measure 4 Hz combat snapshot cost at raid scale (build lighter
  combat snapshots if needed), reserve llama slots for the human's party, and add a real held-out
  eval split run at live temperature (`0.7`).

## Later (broader WoW systems)

- **Economy & upkeep:** Vendor panel for selling non-grey items, mailbox, player trade window,
  Auction House (wrapping playerbots' AH pricing or `mod-ah-bot`), and bank.
- **Professions:** Learn professions, craft skill-ups, fishing, and party disenchanting/crafting
  requests (wrapping playerbots' `craft`/`disenchant` actions).
- **Guilds & LFG social graph:** `guild:` chat output, guild invite/charter, LookingForGroup channel
  posting, and slow background guild life jobs.
- **Raid logistics & level-80 progression:** Main tank/assist assignment, pull timers,
  Bloodlust/stack/spread raid warnings, master loot, lockout/consumable checks, RTSC room
  landmarks, talent deviations, glyphs, dual spec, gems/enchants, and emblem vendors.
- **Contributor portability:** Replace Arch-only Percona paths and RTX 5090
  `CMAKE_CUDA_ARCHITECTURES=120` with auto-detection, document model download URLs/checksums, and
  check in one `report_stats.py` if benchmarks resume.
