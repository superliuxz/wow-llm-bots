# Self-Improving Human-Likeness Loop

A layered evaluation and tuning architecture for making bots play, move, and socialize like real
WoW players while guaranteeing stability, competence, and performance never regress
(`docs/design.md`, "Evals").

**The scorer is the key to everything:** whatever it rewards, the loop will produce. Status: the
layered scorer (sections 1–4, build steps 1–6) is **built and live-verified** (2026-11-19):
5 Hz observer-parity recording + explicit events (`behavior_trace_scripts.{h,cc}`,
`trace_parity.py`), pairwise Bradley–Terry ranking + `blind_rate.py`, Layer D calibration
(`scorer_calibration.py`, verified PASS on real `Hele` + live bot traces), Layer C `scikit-learn`
adversary suite (`adversary_suite.py`, `var/bench-venv`), Step 5 population metrics
(`population_metrics.py`), and `hill_climb.py` / `soak_scorer.py` switched to Layer A gates +
Layer B 95% CI ranking (old 0–100 composite score and `human_metrics.py` deleted). Remaining
non-code prerequisite before unattended tuning: recording human sessions across all situations
(questing, fighting, looting, town, dungeons, chat) and completing initial `blind_rate.py` batches.

---

## 1. Why the single 0–100 score was retired

The first scorer (`tools/bench/human_metrics.py`) compared session-wide averages of one human
(Hele) against six bots: path straightness, stops/min, jumps/min, instant `>90°` turns/min, idle %,
town %, aggro reaction p50, whisper reply p50.

- **It measures the activity mix, not behaviour.** Idle %, town % and stops/min depend on what each
  character happened to be doing. "Idle" is any moment not moving, fighting or casting: reading
  quest text, eating, being AFK. A human's 55 % idle is not a target.
- **It measures our own code and the recording.** Stops/min counts the module's ~70 yd walk steps;
  "instant turns" compare one trace tick with the next, and a server-set facing jumps where a mouse
  turn sweeps; aggro reaction is mostly LLM latency, and pulls that start with a cast flip
  `in_combat` after the action.
- **Matching it makes bots worse.** The cheapest way to 2.28 jumps/min is jumping at random, the
  cheapest idle % is standing still: both hit the number and look more robotic.
- **One player, single medians.** Matching one person's p50s learns their habits and ignores the
  spread; a whisper delay without the message's length, or what the player was doing, says little.
- **No competence.** A bot can match every number and play badly.

## 2. What game research found (sources in section 7)

1. **A human-vs-bot classifier tells human from bot but cannot rank bots.** In Microsoft's
   Navigation Turing Test (Bleeding Edge), classifiers told human from agent trajectories at up to
   85 % and agreed with human judges, yet their agreement on *which of two agents is more human*
   was low or negative. The authors advise against using such a classifier as a reward: agents
   exploit it. Classifiers on low-dimensional position data were more robust than ones on video,
   which overfit.
2. **Rankings that work come from pairwise comparisons with ratings.** MineRL BASALT (Minecraft)
   ranks agents from pairs of videos judged by people, fed into TrueSkill (a rating plus its
   uncertainty), with human players as reference entries; humans outranked every agent.
   Generative Agents ("Smallville") ranked believability the same way.
3. **Human judges are noisy, and humans don't score 100 %.** BotPrize (Unreal Tournament, 2012): the
   winning bots were rated ~52 % "human", above the human players' own average of 41.4 %. Togelius
   & Yannakakis argue for judges who watch (third person) over judges who play, whose judgment
   is distorted by taking part (Mario AI Turing track); results shift with protocol, context and judge expertise (Pacheco 2018; Even et al.
   2021; NTT); gamers spotted bot videos only 54 % of the time (modl.ai / Riot).
4. **Real bot detection works over hours and across characters.** On WoW, server-side detection
   flags movement paths taken again and again (Mitterhofer et al. 2009); input timing gives bots
   away (Gianvecchio et al.). On Lineage, Aion and Blade & Soul: self-similarity, how regularly a
   player repeats the same activities (AUC 0.83–0.96 alone, NDSS 2016); the intervals between
   actions (97 % on Aion); chat patterns, party play, social networks, and
   trajectories clustered across many players (NCSoft, Lineage W). A 60-second clip shows none of
   this.
5. **A judge that "passes" a bot proves nothing unless it is shown to catch bad ones.** The power of
   a classifier two-sample test depends on the classifier (Lopez-Paz & Oquab; Conformal C2ST).
6. **LLM text can look "more human than human".** Smallville's agents were rated more believable
   than humans role-playing them: judges reward polish.
7. **Human limits are hard rules.** Gran Turismo Sophy capped its control loop near human reaction
   speed and treated sportsmanship as a constraint, not a trade-off.

---

## 3. Layered scorer architecture (A–F)

### A. Gates (must pass, never traded off)
- **Zero broken loops or spam:** 0 crashes, 0 LLM errors, 0 invalid plans, `max_wakes/min/bot <= 20`,
  `do:` p95 `<= 2000 ms`, no stuck loops, death loops, or repeated-refusal loops (e.g.
  `Inventory is full`, repeated `use` on the same object).
- **Progress floor:** fixed XP/h or quests/h floor and bounded deaths. A bot that stands around is
  human-looking in a useless way — progress is a hard floor, never traded off.
- **Human timing limits (GT Sophy):** reaction and whisper reply delays must fall within human
  bounds (no impossible sub-human precision or instant 0.4 s replies).
- **Decision quality floor:** offline plan eval (`tools/wlb test eval`) non-regression.

### B. Ranking (what drives tuning)
- **Pairwise blind comparisons** (MineRL BASALT): compare candidate vs current version, with real
  human clips mixed in as anchors.
- **TrueSkill / Bradley–Terry ratings with uncertainty:** read as how far the gap to the human
  anchors has closed.
- **Keep / revert rule:** a candidate is kept only when its rating beats the current version's
  outside the uncertainty (bootstrapped 95 % CI), the A gates pass, and a confirmation run agrees;
  otherwise it is reverted.
- **Your time is the bottleneck, and the pace of the loop is set by it.** A rating with useful
  error bars needs dozens of comparisons per candidate, and the LLM judge counts only while it
  agrees with you. So you rate in batches (e.g. ~30 pairs at a sitting, a few times a week), the
  LLM judge's agreement with you is re-checked every few rounds rather than every round, and the
  loop runs a handful of rounds a week, not dozens a night.
- **Third-person watching protocol** (Togelius & Yannakakis; Even et al. 2021): clips are judged by
  watching anonymized side-by-side **plots + event timelines** (not video, and not while playing
  alongside the bots) in **matched situations** (same activity, similar level band and zone type;
  names, level, zone, gear, class, and wall-clock session time stripped).
- **Judges:**
  - **You (`tools/bench/blind_rate.py`, to build in step 2):** ground truth (a few minutes of blind ratings per round).
  - **LLM judge:** provides rating volume, and counts **only while it agrees with your blind
    ratings** (rank correlation re-checked every few rounds, on a fresh batch of your ratings) and
    still catches the known-bad bots (D).
- **Separate layers:** body (movement/combat clips), decisions (offline eval + loop detectors),
  and social (chat judged side-by-side against real human chat in the same situation, where overly
  polished/articulate LLM prose counts as a **tell**, not a virtue — per Generative Agents
  "Smallville"). A gain in one layer is never paid for in another, and each tell is fixed in the
  layer that owns it (most movement tells come from playerbots' movement and our walk code, not the
  brain).

### C. Finding tells — explainable adversary suite (diagnostic only, never a reward)
Instead of matching one player's session averages, run detectors drawn from published MMO
anti-bot literature ("would a real MMO's detector flag it?") to diagnose *what to fix next*:
- **Per-situation movement/action classifier:** lightweight `scikit-learn` classifier on 5 Hz
  observer-view position/facing + event features per situation (`travel`, `solo_fight`, `looting`,
  `town`, `chatting`). Retrained every round on held-out human data the tuner never sees; reports
  accuracy + 95% CI and top feature importances.
- **Published MMO detector features:**
  - Server-side waypoint / route repetition (Mitterhofer et al., WoW).
  - Inter-action timing regularity / entropy ("Battle of Botcraft", Gianvecchio et al., WoW;
    NCSoft Aion 2023 action-interval detector).
  - Self-similarity: how regularly a player repeats the same activities (Lee et al., NDSS 2016).
  - Collective / multi-bot trajectory clustering and habit similarity across bots vs across humans
    (NCSoft Lineage W 2024).
  - Chat phrasing repetition, party-play patterns, and whisper delay vs. message length.
  - Optional population pacing comparison (session length, zone/level progression) against the WoW
    Avatar History dataset (91,065 characters over 3 years, sampled every 10 minutes).
- Hand metrics (`jumps/min`, `idle %`, walking turn rate deg/s, reaction from first hit) are kept
  strictly as **per-situation diagnostic distributions** (whole distributions, not medians), never
  as a reward.

### D. Meta-checks on the scorer itself (must pass before driving tuning)
Build a labelled calibration set:
- Frozen held-out human clips
- Current LLM bots
- Known-bad variants: (1) plain playerbots, (2) a **random-jumper** bot that fakes `jumps/min`,
  (3) an **instant-reply** bot (0.4 s whispers), (4) a bot that walks the same loop

Before the scorer is allowed to drive any tuning loop, it must pass all three checks:
1. **Correct ranking:** humans > current bots > plain playerbots / random-jumper / instant-reply /
   same-loop walker.
2. **Repeatability:** scoring the same bot twice yields consistent ratings within uncertainty.
3. **Human agreement:** automatic rankings agree with your `blind_rate.py` ratings, and a
   known-better change measurably moves the rating.

Keep the random-jumper bot permanently in the regression set so rate-faking is always caught.

A judge that can't tell a bot from a human proves nothing by itself: the power of a classifier
two-sample test depends on the classifier, and a weak or badly trained one "passes" a bot that
differs (Lopez-Paz & Oquab; Conformal C2ST). A judge's verdict on our bots counts only while it
still catches the known-bad variants above.

### E. Anti-gaming safeguards
- **Frozen held-out human set** outside the repo (read-only), never seen by tuning.
- **Retrain diagnostic classifiers every round**; never optimize against a frozen classifier.
- **Cap candidate evaluations** per hypothesis/round.
- **Commit on KEEP** so a later `REVERT` never discards earlier kept changes; lock all
  measurement, trace, and soak config files in a read-only manifest outside the repo.
- **Strict edit scope:** tuning rounds edit only humanization/prompt/identity files
  (`human_pacing*`, `RULES.md`, identities); bug fixes and C++ game-judgment changes go separately
  (no hard-coded gameplay-judgment numbers in C++).
- **So the loop can't fix the biggest tells itself.** Most movement tells live in our walk code
  and in playerbots' movement, outside that scope, and they need review. Expect the large gains
  to come from reviewed fixes that the scorer *measures* (before and after, with the same B
  ranking), and the overnight loop to tune what's within its scope.

### F. Recording parity (prerequisite for everything above)
The server learns a human player's position and facing from client movement packets (every few
hundred ms), whereas a bot's facing changes instantaneously on the server. Polling both at 5 Hz
server-side would leak packet timing so every judge detects bots from packet cadence rather than
behaviour.
- **Record humans and bots the exact same way:** from what the server **broadcasts to other
  players** (`MSG_MOVE_*` / `SMSG_MONSTER_MOVE` / splines + cast, damage, loot, quest, chat
  broadcasts), with each packet's own timestamp — which is also what a real observer sees.
- **Hide the packet type, not only its timing.** A human's movement reaches other players as
  `MSG_MOVE_*` heartbeats, a bot's as splines (`SMSG_MONSTER_MOVE`). Both are rebuilt into one
  common form (position and facing at fixed times, events on the same timeline) before any
  feature is extracted or any clip rendered, and nothing downstream sees which packets they came
  from; otherwise a judge learns the packet type instead of the behaviour.
- **Situation labels from the events.** A labeller cuts every trace into situations (travel, solo
  fight, looting, in town, chatting, in a group / dungeon) from the recorded events and state, so
  clips can be matched and diagnostics compared per situation; without it every clip is "mixed"
  and nothing matches.
- **Human clips for every situation.** Matched comparisons need you (and ideally a few other
  players) in each situation: questing, fighting, looting, town, dungeons, chat. Today there is one
  112-minute session. Record that spread over several sessions, holding out ~30 % for testing; one
  player's sessions mostly measure closeness to that player.

---

## 4. Build order

1. **Recording parity (F):** observer-view broadcast recording, rebuilt into one common
   trajectory form for humans and bots (fixed-time position and facing, no packet type), +
   explicit timestamped events (`damage_taken`, `combat_start`, `loot`, `quest_accept/turnin`,
   `cast_start`, `chat in/out` with `text_len`) + the situation labeller. First, so every session
   you play from then on counts; then record yourself across every situation (and friends if
   possible).
2. **Pairwise rating harness + `blind_rate.py` (B):** situation-segmented, metadata-stripped
   30–60 s clips rendered as plots/timelines; TrueSkill / Bradley–Terry ranking with human anchors
   and uncertainty; interactive blind rating CLI that works in batches + LLM judge agreement
   tracking; the keep / revert rule (B).
3. **Known-bad bots & scorer calibration suite (D):** held-out human vs current bots vs plain
   playerbots / random-jumper / instant-reply / same-loop walker meta-tests.
4. **Adversary suite & diagnostic classifier (C):** per-situation `scikit-learn` classifier
   (diagnostic only) + published MMO anti-bot features (waypoint repetition, inter-action timing
   regularity, cross-session self-similarity, multi-bot clustering).
5. **Long-horizon & population metrics:** multi-session route repetition, inter-bot vs inter-human
   similarity, chat phrasing diversity, and optional WoW Avatar History pacing comparison.
6. **Switch the loop over:** `hill_climb.py` keeps or reverts on the B rating (with the A gates),
   and the composite 0–100 score and its per-minute averages are deleted.

---

## 5. Varied scenario suites (anti-overfitting)

To avoid overfitting to one class, starting zone, or dungeon, `soak` runs a stratified mix of
open-world questing and 5-man Tank/Healer/3DPS dungeon parties using `new:<race>:<class>` fixtures
in `FixtureLoader` + `llmtest group`/`gear`/`tele`:

| Suite | Scenarios | Composition |
|---|---|---|
| `varied` (default tuning) | `soak_horde_rfc` + `soak_alliance_dm` | **Run 1:** 5 Horde questing classes (Orc War, Tauren Dru, Undead Mag, BE Pal, L15 Troll Hun) + 5-man **Ragefire Chasm** L15 (War/Pri/Rog/Mag/Sha). **Run 2:** 5 Alliance questing classes (Hum Pal, NE Hun, Dwf War, Gnm Loc, L15 Dra Sha) + 5-man **Deadmines** L20 (Pal/Dru/Rog/Mag/Hun). |
| `heldout` (generalization check, never used for KEEP/REVERT) | `soak_northrend_hol` + `soak_heldout_uk` | **Run 1:** 5 mid/high Horde questing bots (L15–L70 Silverpine/STV/Borean Tundra) + 5-man **Halls of Lightning** L80. **Run 2:** 5 unseen Alliance classes + 5-man **Utgarde Keep** L70. |
| `quest` (legacy) | `soak` | 10× L1 Priest questing in Valley of Trials. |

---

## 6. Running the loop

- **One command:** `tools/wlb test soak --suite varied --minutes 5 --runs 2 --score --human-dir ~/.local/share/wow-llm-bots/human_baseline`
  (layered scorer: `tools/bench/soak_scorer.py`, `trace_parity.py`, `pairwise_ranker.py`,
  `adversary_suite.py`, `population_metrics.py`; `--judge` adds order-debiased LLM pairwise judging).
- **Blind rating ground truth:** `tools/bench/blind_rate.py rate --source human=... --source current=... --source candidate=... --pairs 30`,
  `blind_rate.py llm-agree`, and `blind_rate.py summary`.
- **Layer D calibration:** `tools/bench/hill_climb.py calibrate --human-dir ~/.local/share/wow-llm-bots/human_baseline`
  (verified PASS on real `Hele` + live bot traces: `heldout_human 1000.0 > current_bots 175.1 > plain_playerbots 46.6 > same_loop_walker -68.9 > random_jumper -101.4 > instant_reply -128.2`).
- **Tamper check:** `tools/bench/hill_climb.py lock-scorer` writes a read-only (0444) SHA-256 lock
  manifest (`~/.local/share/wow-llm-bots/scorer.sha256`) covering all 28 measurement, trace, and
  soak config files (`LOCKED_PATTERNS` in `hill_climb.py`).
- **Recording human sessions** (5 Hz observer-parity + explicit events):
  ```ini
  LlmBrain.TraceDir = "/home/will/.local/share/wow-llm-bots/human_baseline"
  LlmBrain.ObserveHumans = 1
  ```
- **A round** (on a branch, while not playing): `hill_climb.py init --minutes 5 --runs 2` once;
  then `hill_climb.py status` to inspect Layer C diagnostic tells, make one hypothesis/change, and
  run `hill_climb.py eval-candidate --category code --target <tell> --hypothesis "<one sentence>" --minutes 5 --runs 2 --auto-revert`.
  Keeps only when Layer A gates pass (including XP/h floor, deaths ceiling, human timing limits,
  no refusal loops), candidate Layer B Bradley–Terry rating beats current outside bootstrapped 95%
  CI, a confirmation run agrees, and `--suite heldout` gates pass.
- **What a scored round may change** (`ALLOWED_EDIT_GLOBS` in `hill_climb.py`, enforced):
  `server_module/src/options/human_pacing*`, `content/RULES.md` and `content/identities/*`. Anything
  else (other movement code, playerbots settings, bug fixes) is a separate reviewed change measured
  with the same Layer B ranking.

---

## 7. References

- Navigation Turing Test (Devlin et al., Microsoft Research, ICML 2021):
  https://arxiv.org/abs/2105.09637
- MineRL BASALT (TrueSkill pairwise human/agent ranking): https://arxiv.org/abs/2107.01969;
  2022 retrospective https://arxiv.org/abs/2303.13512; BEDD dataset https://arxiv.org/abs/2312.02405
- BotPrize (Unreal Tournament believability Turing test):
  https://en.wikipedia.org/wiki/Computer_game_bot_Turing_test
- Togelius & Yannakakis et al., "Assessing Believability":
  https://pure.itu.dk/en/publications/assessing-believability/; Mario AI Turing track (2012);
  Even et al. (2021) believability protocol https://www.frontiersin.org/articles/10.3389/fcomp.2021.774763/full;
  Pacheco et al. (2018) https://gameai.eecs.qmul.ac.uk/2018/06/15/pacheco2018studying/
- Human-like bots for tactical shooters (modl.ai / Riot, 2025): https://arxiv.org/abs/2501.00078;
  Counter-Strike behavioural cloning (Pearce & Zhu): https://arxiv.org/abs/2104.04258
- Generative Agents ("Smallville", LLM polish vs human chat warning): https://arxiv.org/abs/2304.03442
- Gran Turismo Sophy (reaction-rate caps & sportsmanship as hard gates): https://ai.sony/blog/blog-043/
- Mitterhofer et al. (2009), Server-side Bot Detection in MMOs (WoW waypoint repetition):
  https://www.eurecom.fr/publication/2860/download/rs-publi-2860.pdf
- Gianvecchio et al., "Battle of Botcraft" (WoW timing irregularity):
  https://www.eecis.udel.edu/~hnw/paper/hop.pdf
- NCSoft & Korea University: Lineage W trajectory clustering (2024) https://arxiv.org/abs/2501.10461;
  self-similarity in Lineage, Aion and Blade & Soul (Lee et al., NDSS 2016)
  https://seal.cs.ucf.edu/doc/ndss16.pdf; multimodal bot detection on behavioural characteristics
  (Kang et al. 2016) https://arxiv.org/abs/1606.01426; Aion action
  intervals (2023) https://www.koreascience.or.kr/article/JAKO202364360080927.pdf; chatting patterns
  https://scholar.korea.ac.kr/item/070dfc78-7260-4b1e-9b37-4a5c35aa197a; party play
  https://pure.korea.ac.kr/en/publications/online-game-bot-detection-based-on-party-play-log-analysis/;
  survey https://pmc.ncbi.nlm.nih.gov/articles/PMC4844581/
- World of Warcraft Avatar History dataset (MMSys 2011):
  https://homepage.iis.sinica.edu.tw/~swc/pub/world_of_warcraft_avatar_history.html
- Lopez-Paz & Oquab, "Revisiting Classifier Two-Sample Tests" https://arxiv.org/abs/1610.06545;
  Conformal C2ST https://arxiv.org/abs/2507.17026: a weak classifier "passing" proves nothing
