# WoW 3.3.5a Private Server: LLM-Driven Bots for 25-man ICC
 
Saved from chat on 2026-11-10. Use this as the starting context for follow-up chats.
 
## Goal
Private WoW 3.3.5a (12340) server where bots can clear 25-man ICC, with local LLMs: Qwen as the "brain" controlling bots, Gemma for voice/chat. Everything native C++ on Linux, no Docker, running on a single RTX 5090 (32 GB).
 
## Scope (updated 2026-11-14)
ALL 3.3.5a content, not just ICC: every 5-man dungeon (Classic, TBC, WotLK; normal and heroic) and every raid up to 25-man heroic ICC. ICC is the hardest endpoint, not the only target.
- Paths: project at /mnt/storage/wow-llm-bots, client at /mnt/storage/WoW-3.3.5a, compiler is clang (not gcc).
- mod-playerbots already ships boss-specific code in src/Ai/Raid/* (ICC ~20k lines, Ulduar, Naxx, SSC, BT, TK, Hyjal, Kara, ZA, Gruul, Mag, MC, BWL, Ony, AQ20, ZG, VoA, OS, EoE, RS) and src/Ai/Dungeon/* (most WotLK 5-mans plus a few TBC). Likely gaps: Trial of the Crusader, Halls of Reflection, most Classic/TBC dungeons, AQ40. Verify by testing.
- Strategy: test each instance from simplest to hardest, find where bots fail, then fix or add triggers. Unscripted content falls back on generic combat AI.

## Base stack
- AzerothCore **mod-playerbots fork** (liyunfan1223 / mod-playerbots org, "Playerbot" branch of azerothcore-wotlk). Stock AzerothCore will not work.
- Native build with cmake/gcc on Linux, MySQL 8 and Boost on the host.
- Client data (maps, vmaps, mmaps, DBC) extracted from the user's own 3.3.5a client.
- Config: `AiPlayerbot.Enabled = 1` in `playerbots.conf`. Bots added with `.playerbots bot add <name>` / `addclass`.
- Note: Docker on Linux adds essentially no latency; the user chose native anyway to reduce layers. Real bottleneck is LLM inference, not containers.
## Architecture (split by timescale)
| Layer | Timescale | Who |
|---|---|---|
| Reflexes (rotations, interrupts, movement, avoid hazards) | ~100-300 ms | Playerbots engine, C++ triggers/actions |
| Raid commander (phase strategy, target priority, cooldowns, stack/spread) | 1-3 s loop | Qwen |
| Chat, banter, callouts, natural-language replies | seconds | Gemma |
| Voice | 1-2 s round trip | Whisper (STT) + Piper/Kokoro (TTS); Gemma does not do speech output |
 
Qwen can NOT be in the combat reflex loop. It decides which strategies to enable and issues high-level commands (e.g. `co +x`, `nc`, `follow`, `attack`, `flee`, formations); triggers do the actual reacting.
 
## Models on the 5090
- Qwen3 30B-A3B at Q4 (~18 GB, fast MoE) as commander, thinking disabled (`/no_think`).
- Gemma 3 12B at Q4 (~7-8 GB) for chat.
- **Update 2026-11-14:** one model for thinking + chat (see Chat). Candidates for the live brain, all local and free: Qwen3.6-35B-A3B (official successor to Qwen3 30B-A3B, ~22 GB Q4, leaves ~10 GB for KV cache; main candidate), Nemotron 3.5 Lightning 30B-A3B (~27 GB Q5; single source, verify), gpt-oss-20b (lighter fallback). Dense Qwen3.8 27B (~24 GB Q6) for offline quality jobs (personalities, lessons, fine-tune data), run when bots aren't thinking. Pick by local test: chat style, stuck-reasoning, 25-bot burst latency, VRAM headroom. VRAM reality (measured 2026-11-14): desktop (KDE, Chrome, VS Code) already uses ~2.5 GB and the WoW client adds more while playing, so with a ~22 GB model only ~5-7 GB remain for KV cache. Benchmark with the game running. CUDA 13.4 installed; llama.cpp v1.0.0 pinned in third_party/, built by tools/build-llama.sh (clang 23 host compiler via -allow-unsupported-compiler, sm_120) into out/llama. Model: unsloth/Qwen3.6-35B-A3B-GGUF UD-Q4_K_XL (20.8 GiB, sha256-verified) in models/ (fallback UD-IQ4_XS 16.5 GiB if KV room is too tight).
- **llama-bench (2026-11-14, full offload, flash attention):** prompt processing ~13,000 tok/s (pp512/pp2048), generation ~267 tok/s single stream. => one decision (~1,500-token prompt + ~50-token reply) ~0.3 s uncached. 25-bot burst test (2026-11-14, tools/bench/burst_test.py, llama-server -np 25, 4096 ctx/slot, q8_0 KV): cold (no cache) 6.8 s; warm with ~1,560-token shared prefix cached: 2.2-2.9 s with JSON-schema enforcement, ~1.5 s without (prompt-only JSON, parse + validate). Invalid replies came from truncation at max_tokens (free-text "detail" too long). VRAM 26.6/32.6 GB with desktop + browser. Decision: no grammar enforcement; compact command vocabulary (~10-15 tokens, e.g. {"a":"move","to":"spread"}), strict parser, one retry or reflex fallback on invalid output; expected < 1 s for a 25-bot burst. Bots must stick to their slot (keep their prefix cached). **Numbered-options test (same day):** C++-computed options listed in the prompt, reply = option number (~2 tokens, max_tokens 3): warm 25-bot burst **0.36-0.64 s**, 25/25 valid (vs 1.7 s for JSON). Decision: brain decisions are "pick an option" (options computed by C++, matching the LoS/geometry design); decide first, generate the chat line separately afterwards (humans move, then type). Quality issue found: all 25 bots chose the same option regardless of personality/experience (new raiders should sometimes hesitate, keep attacking, or ask) -> first eval cases for personality/knowledge diversity. **Fix (decided): human variance comes from explicit, tunable layers, not LLM sampling luck:** (1) attention gate in C++ before the brain: did the bot notice the call at all (skill, overload, tunnel vision, personality); bots that miss it keep doing what they were doing; (2) knowledge-gated perception and options: veterans see "Defile in 3s" and a spread option; new players see an unexplained dark circle and get options like "ask what that is" / "keep attacking" (may still follow the leader via herd effect); (3) sample the choice from the model's per-option probabilities (llama.cpp logprobs), reshaped by personality (cautious vs reckless), instead of taking the top answer; (4) execution noise from the skill/mistake model (reaction delay, late move, wrong direction). Eval: population check, many personas on the same situation; outcome distribution compared with a plausible human raid (e.g. most spread in time, some late, a few miss it, new players ask); fail if all identical or random.
- Small STT/TTS models alongside.
- Serve with llama.cpp (CUDA) either linked as `libllama` or via `llama-server` on localhost. In-process saves about 1 ms over localhost HTTP, so choose by debuggability.
- WoW server runs on CPU.
## C++ module design
- AzerothCore module with a `WorldScript` hook that snapshots raid state every 1-2 s (boss, phase, debuffs, positions, low-HP players, cooldowns).
- **Threading rule:** the world thread must never wait on the GPU. Snapshot goes into a queue, a worker thread runs Qwen, results return via another queue and are applied on the next world tick. AzerothCore is not thread-safe, so the inference thread must never touch bots or the map.
- Constrain Qwen output with a GBNF grammar / JSON schema into a short whitelisted command list (20-40 tokens) so it can't hallucinate invalid commands.
- Keep a static prompt prefix and reuse the KV cache. Expected ~200-400 ms per decision.
- Existing community option for Gemma side: `mod-ollama-chat` / mod-playerbot-chatter style modules (check compatibility with the playerbots fork).
## Expected raid quality
- Good: Marrowgar, Deathwhisper, Gunship, Saurfang (stock bots handle these).
- Where the commander helps most: Festergut, Rotface, Putricide (phase awareness).
- Fiddly: Blood Council, Lanathel, Valithria (leave healer logic to the engine).
- Hardest: Sindragosa and Lich King (Tombs, Defile, Val'kyr, Harvest Soul need sub-second reflexes in C++; Qwen only picks phase-level strategy). Expect wipes while tuning.
- Over-gearing the bots is the biggest lever for feel.
- Hands-off 25-man Lich King kill is weeks of tuning, not a weekend.
## Triggers: what actually needs writing
Only mechanics where a player must DO something need a trigger/action pair, roughly 2-6 per boss (est. 10-20 total across the bosses where stock bots fail):
- Ground hazards (Defile, Bone Storm, ooze puddles, void zones): move out
- Targeted debuffs (Val'kyr grab, Frost Beacon, Unchained Magic, Gas Spore)
- Dispel/interrupt windows
- Add handling and target priority (Vile Spirits, Shambling Horrors, Cult Adherents)
- Positioning/phase transitions (tank swaps, spread vs stack)
Passive auras and raid damage healers can heal through need nothing. Many are reusable: one generic "standing in bad area, move out" trigger fed by a per-boss list of spell IDs.
 
**First reusable piece to write:** generic avoid-ground-effect trigger + action.
 
## Making it learn
1. **Memory (start here):** log every fight (boss, phase, commands issued, deaths, cause, wipe time). A second Qwen call writes a short lesson after each wipe/kill; store per-boss lessons in the bot's skills/*.md files (see Bot memory) and inject the top few into the commander prompt next pull. No training needed, human-editable.
2. **Fine-tuning (later):** log (state -> commands -> outcome), keep good pulls, LoRA/QLoRA on a smaller Qwen3 (4B-8B) or distill from the 30B. Train offline between sessions.
3. **RL:** probably not worth it.
- Build a scenario reset script (reset, rez, teleport, re-pull) to run ~50 attempts overnight and track kills, wipes and average wipe phase.
- Learning can only fix strategy, not slow or missing reflexes.
## Reflex speed
World tick ~50-100 ms, bot reaction governed by `ReactDelay` in `playerbots.conf` (default ~100 ms, verify per version), GCD 1-1.5 s. Speed is fine; the weakness is missing boss-specific triggers.
 
## Where the trigger code lives / references
- mod-playerbots repo (github.com/mod-playerbots/mod-playerbots) and its wiki (lists raids with programmed strategies).
- Source layout under `src/Ai/` (Base/Actions, Strategy, etc.); raid-specific code should be in a raid folder there, exact path not yet confirmed. Grep for `Icecrown`, `Sindragosa`, `Defile`, `Putricide`.
- Community project lathcf/azerothcore-playerbots-docker-automated advertises scripted strategies for several raids incl. ICC Lich King phase 3 (useful as a pattern reference).
- jrad7/mod-dungeon-clear shows how to extend playerbots from a separate module by subclassing its strategy/action/trigger/value classes without editing core files.
- Pattern: trigger class + action class + strategy wiring them, registered in the context files.
## Bot realism spec (decided 2026-11-14)
**Core principle: bots do whatever a human player does.** Every design decision is checked against this.

Supersedes the single-commander design above. Each bot is a "real player": it may only perceive and do what a human could through the 3.3.5a client (plus DBM-style timers).

**Action space (client-equivalent only):**
- Movement: move to point, follow player, face target, jump, stop
- Targeting: target unit, focus, tab-target nearest enemy, assist player
- Abilities: cast spell on target, use item, stop casting, cancel aura
- Interaction: loot, talk to NPC, use object, trade
- Communication: say/party/raid/whisper, emote, raid markers, raid warning, ready check

**Perception:** unit frames, visible auras, combat log, boss emotes/yells, chat, DBM-style boss timers (allowed). No hidden server state, no out-of-sight positions.

**Spatial concepts (LoS, range, facing, threat):** the model understands them from training; it perceives them only via the snapshot. Server computes them exactly (vmaps LoS, range, facing, threat) and states them per unit ("Tank: 32 yd, OUT OF LOS (pillar)"). LoS also filters perception: things behind walls don't appear. Reflexes handle routine cases (step to regain LoS); the brain handles tactics (LoS-pulling, hiding behind Ice Tombs, healer positioning). LLMs are weak at geometry from text, so C++ computes candidate positions ("pillar 8 yd N breaks LoS to boss") and the brain chooses. Tactics like LoS-pulling are learnable skills.

**Physics parity (requirement):** bots must obey the same physics as a client player. Today they don't: bots send no movement opcodes, so HandleFall/fall damage never triggers (see mod-playerbots PlayerbotAI.cpp:266, CheckMountStateAction.cpp:76) and their server-side paths avoid cliffs. The server module must simulate the client side for bots: real falls off edges, fall time -> fall damage on landing, mid-fall abilities (Blink, Slow Fall, Levitate, Divine Shield, possibly Charge; verify which work in 3.3.5a) interacting with the fall as for a client. Check other client-driven mechanics similarly (e.g. swimming/breath, fatigue).

**Death and stuck recovery (decided):** bots use only the human ways out, which are normal game features: release spirit then corpse run, or resurrect at the spirit healer (resurrection sickness, durability loss); hearthstone; the client's Help -> "Character Stuck" (spell 7355, Spell::EffectStuck: dead or ghost -> teleport to the graveyard/spirit healer so the run can be retried; alive -> uses the Hearthstone if present and off cooldown, else nudges ~5 yd in a random direction); asking for help. Problem detection offers "Stuck" as a brain option when a ghost makes no progress toward its corpse, alongside "try another route" and "resurrect at spirit healer". Playerbots' random-bot manager "refresh" (free revive + full heal + teleport to a chosen spot) and random level placement are cheats and are disabled (tools/disable-rndbot-cheats.sh pre-seeds its teleport/randomize events; long-term our own module manages our bots instead of RandomPlayerbotMgr). New bots start at their race's starting location like a newly created character.

**Learned tricks pattern:** for sub-second techniques (blink before landing, blink out of Defile, charge-cancel falls), the brain learns the trick (advanced concept, Wowhead, being taught, own experimentation) and that unlocks a reflex in C++ via the same known-list gate as boss mechanics (the list covers class tricks too). Execution timing quality comes from the skill slider (new players mistime it).

**Gap designs (proposed 2026-11-14; found by playing through as a bot):**

*Architecture note:* mod-playerbots' stated philosophy is "stability, performance, and predictability over behavioral realism" (its AGENTS.md). Don't fight it by heavily editing playerbots: put realism in our own module (e.g. mod-llm-brain), reuse playerbots' building blocks (movement, combat rotations, actions like trainer/vendor/repair/ReviveFromCorpse), and manage our 50 bots ourselves rather than as playerbots "random bots" (RandomPlayerbotMgr::Refresh resurrects dead bots, random teleports/levels). Verify feasibility when building.

1. **Goals & motivation:** per-bot `goals.md` with three levels: long-term (epic mount, raid, max profession; updated at milestones), session (set at login from identity, recent memory, level and how long it plans to play: "ding 30 tonight"), immediate (now: "turn in 3 quests, vendor"). Gamer-motivation traits shape goals (achiever pushes levels, explorer wanders, socializer hangs out). The brain steers playerbots' New RPG state machine (quest/grind/gather/travel/rest/social/AH) by choosing the activity instead of its random weighted transitions. Re-plan on: goal done, blocked, boredom (time on one activity > patience trait), invites/social events, mood (tired -> log off).
2. **Perception snapshot:** compact text with sections: self (HP/mana, buffs/debuffs, bags, durability, gold, XP, cooldowns); surroundings (zone/subzone; salient nearby units with nameplate info: name, level color, elite/rare, hostile/neutral/friendly, quest marker !/?, distance/bearing, LoS); minimap (nearby towns, flight master, gathering nodes if skilled); ground effects; recent "sounds"/events (hit from behind by X, boss yell, rare spawned); chat. Filtered by LoS, view distance and human-like attention (top-N salient units, not everything). Zone layout knowledge from concepts/ + Wowhead lookup.
3. **Upkeep without cheats:** set BotCheats = "" and turn off AutoLearnTrainerSpells, AutoLearnQuestSpells, AutoPickTalents, AutoUpgradeEquip, AutoEquipUpgradeLoot, AutoTeleportForLevel, AutoGearCommand, ReviveBotWhenSummoned (for our bots). Upkeep needs (new spells at trainer, talent points, bags full, durability low, out of food, riding at 20/40, class quests/pets) become immediate goals; existing playerbots actions execute them; bots pay gold/time like humans. Death: real corpse run or spirit healer (brain chooses; reuse ReviveFromCorpseAction), no instant revives.
4. **Realm type and factions (decided):** PvE realm (to start), all bots on the user's faction: Horde. Bots choose Horde races freely (orc, troll, tauren, undead, blood elf). Enforce via worldserver config (realm GameType PvE, disable Alliance character creation).
5. **Performance feedback:** server-side per-fight stats from combat events (damage/healing done, DPS/HPS, avoidable damage taken, deaths with a recap of the last ~5 s). Shown like Recount/Skada (bots can link meters in chat, as humans do) and fed into post-session reflection -> skills/*.md. Group feedback ("dps??", "nice heals") is also input.
6. **GPU scheduler (in harness):** priority classes: P0 combat/raid calls (deadline ~1 s), P1 group/danger, P2 questing/navigation/stuck, P3 chat replies, P4 reflection/planning (run when GPU idle or at session end). Bounded concurrency on llama-server; a newer request for the same bot replaces a stale one; on missed deadline the bot falls back to its reflex default (looks like human hesitation).
7. **User visibility/control (decided):** "bot mind viewer": a local web page served by the harness showing each bot's goals, current perception, recent decisions with reasoning, memory files, and stats. In-game GM commands (e.g. `.brain why <bot>`, `.brain pause <bot>`, `.brain stopall`). Every decision logged (JSONL). Global kill switch: all bots revert to reflex-only or log out.
8. **Real-life interruptions: DROPPED (user decision).** No AFKs/brb. Online presence is handled by schedules only. Bots still announce before logging off from a group ("gtg in 10 sry") and never leave mid-dungeon.
- **Item comparison:** tooltip stats + side-by-side compare from the server, stat weights from concepts/ (tiered, so new players misjudge sometimes). GearScore addon culture allowed (3.3.5a era), like DBM/Questie.
- **Addons (decided):** bots "use addons" by getting the same information server-side (no client, so no real Lua addons): DBM timers, Questie markers, GearScore (and judging others by it), Recount/Details-style meters. The user's client gets a real 3.3.5a addon pack.
  - Installed in the user's client (2026-11-14, no UI replacement): DBM (Zidras/DBM-Warmane), Questie (Aldori15/Questie, 335 branch), Skada (bkader/Skada-WoTLK), Omen, GearScoreLite Reborn (Arcitec), AtlasLoot Enhanced, Auctionator (alchem1ster/WotLK-Auctionator), Postal. Source list: github.com/NoM0Re/WoW-3.3.5a-Addons.
  - DBM choice (decided): keep Zidras/DBM-Warmane (maintained fork of the 2010 official DBM; some timers tuned to Warmane's scripts). Official 2010 DBM is the fallback. Bots' timers: parity with the user's DBM (bots never know more than the user; proposed default), and fix the user's DBM where testing against AzerothCore boss scripts (src/server/scripts) shows wrong timers.
  - Same addons are the reference for bots: sources kept in `reference/addons/`. Port DBM timer/warning data, GearScoreLite's formula, Questie-335's quest/objective data (it ships AzerothCore quest validators in tools/) and Skada-style meters server-side, so bots and the user see identical information.

**No cheats:** disable playerbots cheat options (free reagents, teleport-to-master, etc.).

**Two layers per bot:**
- Reflexes (C++ playerbots engine): rotations, dodging, interrupts. Human-paced reaction times.
- Brain (LLM): decides what to do and say via the action space above. Event-driven (phase change, raid call, low HP), not a fixed timer. Non-leaders can run a smaller model.

**Coordination:** every bot sees DBM timers itself (like each human running DBM) and can react on its own. On top of that, one bot is raid leader. It also makes calls ("Defile in 5, spread") in raid warning + voice (TTS). Other bots react to calls like humans would (may ignore/mis-execute per skill). Human players in the raid get the same warnings.

**Skill profile (tunable per bot):** a 0-1 skill slider mapping to reaction-time range, missed-interrupt chance, late-dodge chance, chance to ignore a call. Per-bot overrides allowed. Goal: human-paced and imperfect, like a real pug.

**Mistakes happen even with warnings:** DBM + raid leader calls do not guarantee correct play; that is what makes raids hard. Model mistakes by cause, not a flat dice roll:
- Overload: error chance rises with the number of simultaneous mechanics/tasks (hard fights get harder naturally)
- Tunnel vision: focused on rotation/target, misses the warning
- Late reaction: noticed, but too slow
- Wrong execution: moves to the wrong spot/direction, stacks instead of spreads
- Missed/ignored call: didn't process or chose not to follow the leader
Skill slider sets the baseline; current load scales each mistake type up or down.

**Call timing (decided):** bots receive a call exactly when a human would. Voice calls: when the TTS audio starts playing (never from the text before it's spoken), plus per-bot comprehension (~0.5 s) and reaction time (longer when busy). Text calls / raid warnings: when the message appears in chat (after the leader's typing delay, or instantly for a macro/DBM-style auto-warning), plus reading and reaction time. Before voice exists, only the text rule applies.

**Leadership:** 5-man: the user is raid leader, bots follow the user's calls. Raids: a bot is raid leader and directs all other bots; it can't control the user but assigns/warns the user and adapts if the user goes off-plan. Bots also listen to the user's calls in raids.

**Following calls (decided):** no hard rule; each bot's brain decides whether to follow any call, like a real person. It weighs: who called (leader authority, trust in the user, other bots), trust from memory (good past calls raise it, wipes lower it), its own perception (DBM, boss state), personality (loyal follower vs independent), skill (strong players spot bad calls; weak ones follow or panic), and herd effect (following the raid even on a bad call, since splitting is worse). Bots can follow wrong calls and ignore right ones. The bot leader likewise decides whether to back or overrule the user's call publicly. Cost: a call triggers a brain decision in every bot that hears it (~25 batched LLM calls; est. ~1 s, benchmark first). Raids may split on conflicting calls; that is accepted as realistic.

**Lifelong bots:** each bot plays WoW for real: levels 1-80 in real time, quests (Questie-style objective data allowed), farms gear, learns its class. Persistent memory per bot (decided, no SQLite):
  - `bots/<name>/identity.md` (personality, backstory, voice; always loaded, small), `relationships.md` (user and other bots), `skills/INDEX.md` (one line per skill: name + when it applies) + `skills/<topic>.md` (e.g. warrior-tanking, icc-lich-king, fishing, auction-house). Harness loads only skills relevant to the current context. The bot writes/edits its own skill files after sessions, wipes and discoveries; files have a size cap and get condensed.
  - Raw events: `bots/<name>/events/<date>.jsonl`, append-only; read by post-session reflection; stats via scripts.
  - Known-mechanics gate (C++ reflex layer): small table in the server's existing MySQL (`bot_known_mechanics(guid, mechanic_id, learned_at, source)`), written by the server module when the harness reports a lesson over the socket.
  - Shared rulebook `bots/RULES.md` (decided): one file for all bots, always loaded first, like a CLAUDE.md. Contains: framing ("you are a human player on a 3.3.5a WotLK private server, playing through the client"), the action space, perception limits (client + DBM/Questie/Wowhead only), common human/player behavior rules (decide calls yourself, trolling chat-only/no slurs, no ninja/rage-quit/sabotage), chat style, output format for the harness. Prompt order for KV-cache reuse: RULES.md (identical for all bots) -> identity.md (stable per bot) -> relevant skills/relationships -> current situation. Serious rules are also enforced in code (harness rejects forbidden/non-client actions); RULES.md guides, code guarantees. Open: how a bot answers "are you a bot?" if friends join.
  - Shared game knowledge `bots/concepts/` (decided): `INDEX.md` + topic files (combat: threat/aggro/LoS/range/GCD/interrupts/CC/DR; stats: 3.3.5a caps and stat effects; grouping: roles, pulling, markers, loot rules, etiquette; leveling: rested XP, chains, flight paths, hearth, durability; economy; culture-335: GearScore, LFG phrasing, achievement links, era memes). Loaded when relevant, not every prompt. Pins the model to 3.3.5a facts (Qwen mixes eras: no transmog/LFR/M+). Tagged basic / intermediate / advanced: basic for every bot; higher tiers only for bots whose experience or learned skills cover them (new players reach them via Wowhead lookup or being taught, then they move into the bot's own skills). Drafted once offline by a stronger model, reviewed by the user, kept short.
  - Distilling concepts/tricks (pipeline): (1) collect candidates from wikis (warcraft.wiki.gg / Wowpedia mechanics pages, e.g. "Fall damage" lists Blink, Charge/Intervene-with-target, Slow Fall, Levitate, Ice Block, Divine Shield, Disengage, Safe Fall, parachute items, deep water), Wrath Classic guides (Wowhead/Icy Veins; closest to 3.3.5a), and Qwen; (2) filter by the extracted 3.3.5a DBC spell/item data (drops later-expansion stuff like Heroic Leap, Rocket Jump, Displacer Beast, Wild Charge; also find candidates by effect type: feather fall/safe fall auras, charge/leap/teleport effects); (3) verify each on the server in scripted scenarios (also validates physics parity); (4) write verified items into advanced-tier concept files, tagged by class. Wiki text is CC BY-SA (fine for private use).
  - Three kinds of knowledge: RULES.md (how to behave, always), concepts/ (how WoW works, shared, tiered), <name>/skills/ (what this bot personally learned).
  - All file writes atomic (temp + rename). `bots/` can be kept in git to track how each bot learns; the user can read and edit bots' skills. Learning = brain revises its own settings (talents, ability priorities, gear choices, action bars) that the reflex layer executes; skill slider improves slowly with practice per situation. Playerbots random-bot cheats (teleport, random gear/levels) must be off.

**Personalities (decided):** traits sampled from realistic population distributions: Big Five (roughly normal), gamer motivation (progression / exploration / social / competition), WoW experience level (new / returning / veteran), habits (chattiness, schedule, risk-taking, patience). The LLM turns sampled traits into a short identity (background, voice, why they play), which drives race/class/name choice. Change over time: fast mood (tilt, happy, tired; resets per session) and slow drift from experience; core traits mostly stable.

**Knowledge by experience (decided):** bots really learn; ignorance is enforced in code where it matters, not by asking Qwen to forget.
- Hard gate (mechanics/reflexes): each bot has a known-mechanics list. A boss-mechanic trigger (e.g. move out of Defile) only exists in that bot's reflex layer once learned; before that, only slow generic reactions ("taking damage, move?"). Learning sources: experience (dying to it; slow, sticky), being told (raid leader/user explaining; fast, as good as the explanation), lookup (Wowhead/guides; theory only), watching others. Knowing unlocks the reaction; execution quality is still the skill slider, which improves with practice.
- Soft gate (quests, talents, gear, general play): prompts built from the bot's own memory/knowledge plus "act as a player with this experience". Some leakage of Qwen's knowledge accepted for low-stakes choices.
- Starting knowledge follows the experience trait: veterans know most mechanics, returning players some, new players almost none.

**Chat (decided):**
- One model (Qwen) does both thinking and talking, for consistency between decisions and speech and to free Gemma's ~7-8 GB VRAM. Gemma only as fallback if Qwen's casual chat sounds stiff (A/B test same prompts). Supersedes the Qwen-brain + Gemma-chat split above.
- Frequency "as often as a human": chattiness trait; most bots quiet ("hi/ty/gg/brb"), a few talkative. Channel-specific behavior (general, trade, LFG, party, guild, whisper).
- Human style: lowercase, abbreviations, typos, slang, varying by personality; mostly talk between fights; typing delay proportional to message length.
- Slang spans 2008-2026, set per bot by personality/experience: veterans lean classic WoW ("LF2M", "pst", "gogogo", old memes), newer/younger players lean modern internet slang, most mix both.
- Skim filter: a cheap pre-filter (addressed? relevant? chattiness) decides if a bot "reads" a message before any LLM call.
- With the user: whispers, party chat, replies; relationship memory.
- Voice: deferred (likely Discord voice with TTS + Whisper later). Until then raid leader calls are raid warning + text.

**Economy (decided):**
- Starting gold: 100g for every bot and for the user (the user is GM but chooses the same rules).
- Simulated auction house representing a ~10k-player realm: base on mod-ah-bot (fills listings, buys player posts; crude vendor-multiplier pricing), plus a virtual population layer: supply modeled from drop rates and virtual players per level range, per-item market prices driven by supply/demand (including bot and user activity), virtual buyers that purchase listings at market price, and a time-of-day realm schedule.
- Bots use the AH (and professions) like humans, by personality: flippers, crafters selling goods, lazy vendor-everything types, etc.

**Bad behavior (decided):** chat only. Trolls/banter in general chat allowed (classic WoW humor, no slurs or hateful content). Never in gameplay: no ninja looting, rage-quitting, abandoning groups or sabotage.

**Problem solving (decided):** when a bot gets stuck or can't do something, its brain figures it out like a human; no per-case scripts. Needs: (1) problem signals from C++ to wake the brain (no progress N s, pathfinding failure, objective stalled, repeated deaths); (2) perception it can reason with: a text description of the surroundings built from navmesh/vmaps/terrain (spatial reasoning from text is a known LLM weakness, test early); (3) human options: jump/strafe/back up/try another route, look it up ("Wowhead" allowed: a local lookup tool built from the server's own world DB (quest text/objectives, NPC/object spawns, drop sources, chains, rewards), exact for this server, no scraping; Wowhead-style tips come from Qwen's own knowledge or asking in chat; a lookup costs some in-game time like alt-tabbing), ask in chat or for help, give up (abandon quest, hearth, do something else). Routine frequent actions stay in C++ (autopilot); the brain handles the unusual. Bots may still fail, like humans.

Existing base: mod-playerbots "New RPG" strategy (src/Ai/World/Rpg/) already quests (via quest POI data), takes flights, grinds, gathers, rests. Limits: solo normal quests only (IsQuestCapableDoing), random weighted state machine (brain needs to steer it), cheats on by default (BotCheats = "food,taxi,raid", random teleports/levels: turn off).

**Population:** start with 50 bots, all with full brain (Qwen 30B) and memory (~1-2 LLM calls/s, fits one 5090). Long-term target up to ~5000 via tiers (core: full brain; population: small model, rarer calls; background: scripted no-cheat play, LLM every 10-30 min) and LOD promotion of bots near the user. Server CPU (9800X3D, 8c/16t) is the likely limit around 1000-3000 real-playing bots (unmeasured). Scale in steps, measuring world update time and GPU load.

**Fresh realm launch (decided):** all 50 bots start at level 1 when the server starts, like a fresh realm. They play independently at their own pace and may get ahead of the user ("they do theirs, I do mine"). No pre-leveled group; raids happen when bots actually reach 80.

**Character creation (decided):** each bot's brain picks its own race, class and name freely, driven by its personality. No enforced class mix; an organic imbalance (too many hunters, few tanks) is accepted.

**Schedules (decided):** the whole stack (server, LLMs) only runs while the user plays; bots only play during those sessions. Within a session, bots still follow their own human-like schedules: not every bot is online every session, logins/logouts are staggered (not all 50 at server start). Bot memory treats real time passing between sessions ("haven't played in a few days").

**Social (decided):** bots socialize like real humans: group for quests/dungeons, friends lists, whispers, guilds formed on their own. Raid group formation: left emergent (bot/user guilds with raid nights, friend bots via relationship memory, LFG pugs). Watch items: with 50 bots on schedules, 25-man may lack online 80s (10-man more realistic until population grows); free class choice may leave few tanks/healers (realistic fix: bots respec/reroll when their guild needs a role). Revisit when the user reaches 80.

**Harness:** thin C++ module in worldserver (exports snapshots per bot's perception, accepts client-equivalent commands over a local socket, never blocks the world thread) + separate harness process (prompts, LLM calls, validation, logging).
- Harness language (decided): **C++**. Still a separate process for crash isolation (bots fall back to reflexes; restart harness without restarting world). Prompts/personality templates are text files loaded and hot-reloaded at runtime, so tuning needs no recompile. Stack: no Boost. Abseil (StatusOr, flat_hash_map, AnyInvocable, strings, time); a small in-house "base" library with a task-runner/callback threading model (SequencedTaskRunner, ThreadPool, PostTask, PostTaskAndReplyWithResult, OnceCallback/RepeatingCallback on absl::AnyInvocable, WeakPtrFactory); libevent IO loop on an IO thread; cpp-httplib blocking calls on the thread pool for llama-server, replies posted back to the main sequence; nlohmann/json; GoogleTest. All deps vendored as pinned submodules in third_party/. Build: clang + CMake + Ninja, markdown/JSONL files for bot memory (no SQLite), llama-server over HTTP (libllama in-process later if needed). Python only for optional offline analysis scripts.
- Code style (decided): all our C++ (harness and server module) follows Google/Chromium C++ style; root `.clang-format` uses `BasedOnStyle: Chromium`. Upstream code in third_party/ keeps its own style. The server module stays thin.
- Performance note: LLM inference (200-400 ms) dominates; harness overhead ~1-2 ms per decision. Hot paths (reflexes, problem detection, chat skim filter, perception building) live in the C++ server module.

## Server features inspired by WoW Forever (decided 2026-11-14)
Borrow ideas, never assets. Implement in our own server module; bots' concepts/ files must describe these custom rules (they differ from stock 3.3.5a).
- **No automated Dungeon Finder** (done in config): `DungeonFinder.OptionsMask = 6` (Raid Browser listing board and seasonal bosses kept), `Playerbots.RandomBotJoinLfg = 0`. Groups form socially via chat ("LF2M Deadmines"). Phase-1 dungeon test must form its group by hand.
- **Camping** (to build in the server module): v1: a Basic Campfire (Cooking's Basic Campfire, so anyone can learn it) makes a campsite; sitting within a few yards for ~60 s makes the player Rested there (rest flag on while in range): rested XP accrues like an inn and logging out there counts as an inn logout (offline rested accrual). Campsites are shared: anyone nearby can use them. v2 (later): profession camp objects with buffs (tent tops up rested XP, cooking pot food buffs), a sleeping bag item for logout-anywhere, limits/cooldowns. Bots camp by personality and schedule (resting with friends at night is a social moment). Reference: WoW Forever camping (Blizzard "Gather Around the Campfire", Icy Veins / Warcraft Tavern camping guides).

## Evals (proposed 2026-11-14)
Four layers, fast/cheap to slow/realistic:
1. **Brain evals (offline, seconds):** cases in `evals/cases/` (tracked): perception snapshot + bot identity/known mechanics + recent chat, plus expectations. Checks: hard (parses, client-legal command, latency), expected/forbidden actions (move out of Defile; new player must not use unlearned mechanics; no loot it didn't win), judge-scored soft qualities (sounds like a 2008-2026 WoW player, in character, 3.3.5a-correct: no transmog/modern terms). Categories: chat style, call-following, stuck reasoning, quest choice, knowledge gating, era correctness, behavior rules, output format. Run on every prompt/model/RULES.md change; pass rate per category + diff vs last run.
2. **Scenario evals (in-game, minutes):** GM-scripted setups (stuck in cave, fall, call mid-pull) via config/tests + tools/bot-sampler.sh; measure success, time, deaths.
3. **Soak metrics (hours):** sampler JSONL -> XP/h, quests/h, stuck time, deaths, gold, chat frequency; compared with the stock baseline (phase 1a) and human pace.
4. **"Bot or human?" review (user):** flags on chat/behavior traces; every flag becomes a layer-1 case.
**Headline metric: human-likeness ("can you tell it's a bot?").** Component categories above are unit tests; the real measure is: (a) blind spot-the-bot test: paired short play traces (one human, one bot), the user and the judge guess the bot; detection rate 50% = indistinguishable, used as the phase sign-off criterion; (b) behavior distributions vs the human baseline: reaction times, chat frequency/length/typos/slang, movement (path straightness, jumps, idling), pacing (breaks, town time, session ends); (c) a bot-tells checklist, each tell a metric (too-fast or constant reply delay, perfect grammar, repeated phrasing, straight-line pathing, no idling, no breaks, never/always-same mistakes, unearned knowledge); new tells the user spots are added. **Human baseline = the user's own play**, logged since 2026-11-14: chat via the worldserver Chat logger (server/logs/Chat.log), character state every 60 s via tools/bot-sampler.sh (server/logs/human/), both untracked (personal data; curated excerpts may go to private/). Fine-grained movement/reaction timing needs the server module later.
Rules: evals call the same harness entry point as live play (`decide(snapshot)`), not a copy of prompts; cases are promoted from real logged play.
**Judge cost control (decided):** deterministic checks first (free); daily iteration uses a local judge (Qwen3.8 27B dense; swapped in when the bot model is unloaded; same-family bias measured by calibration); judgments cached by hash(case + output) so only changed outputs are re-judged; smoke set (~30 cases) for quick loops, affected categories for targeted changes, full set at milestones; cloud judge = Claude Opus 5.5 (`claude-opus-5-5`, pinned) via the Anthropic Message Batches API (50% off; est. ~$1.80 per full ~300-case run plus thinking tokens), used only for calibration against the user's labels (30-50 cases) and phase sign-off. API billing is separate from the user's Claude Pro plan (doesn't consume Pro/Claude Code limits); API account gets a spend limit; key lives in private/ or env, never in the main repo. Claude also avoids the same-family bias of a Qwen judge. OpenRouter not needed for judging. Open: eval runner language (C++/GoogleTest vs Python).

## Legal note
Private server for self/friends is generally tolerated; public or monetized servers get shut down.
 
## Next steps (pick up here) — build roadmap (2026-11-14, replaces the old commander-era list)
0. **Server up: DONE 2026-11-14.** Project-local Percona MySQL, DBs imported, config via config/*.overrides + tools/apply-config.sh (PvE, Horde-only, playerbots cheats/shortcuts off, 0 random bots), servers via tools/server-start.sh / server-stop.sh / wsc, GM account (private/accounts.md), realm reachable from this PC and LAN (Mac). User logged in and confirmed.
1. **Feasibility tests:** (1a solo leveling DONE 2026-11-14, see docs/phase1-results.md; 1b model burst DONE, see llama-bench notes; remaining: 5-bot dungeon with a hand-formed group, longer leveling run past the starting area) (a) stock New RPG leveling from level 1 with cheats off for a session or two: quests done, XP/hour, stuck time, deaths; (b) local model benchmark (Qwen3.6-35B-A3B vs alternatives): chat style, stuck reasoning, 25-bot burst latency, VRAM headroom.
2. **Skeleton:** our server module (perception snapshot export + command socket, never blocks world thread), C++ harness (llama-server client, scheduler, logging), bots/RULES.md, one bot identity. Done when one bot chats and decides via the LLM.
3. **One real bot:** goals, full perception, upkeep without cheats, memory files, mind viewer. Done when it levels 1-10 alone, believably.
4. **Depth:** physics parity (falls), stuck/problem solving with Wowhead lookup, learning loop (reflection -> skills/*.md, mechanics gate), meters/feedback.
5. **Society:** 10 bots; grouping, chat culture, guilds; AH simulation.
6. **Population:** 50 bots with schedules; 5-man dungeons with the user as leader.
7. **Raids:** bot raid leader, calls, DBM-parity timers, skill/mistake model.
Later: Discord voice (TTS/Whisper), scaling tiers toward 5000.

Open small decisions: "are you a bot?" deferred (no friends joining for now; the user is the only human); realism testing method (proposed: regular "bot or human?" review of chat logs/behavior traces; each flag becomes a fix).
