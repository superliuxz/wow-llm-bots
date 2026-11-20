# Self-Improving Human-Likeness Loop

A layered evaluation and tuning architecture for making bots play, move, and socialize like real
WoW players while guaranteeing stability, competence, and performance never regress
(`docs/design.md`, "Evals").

> **Why we retired the single 0–100 movement-average score:** Research on game Turing tests and
> MMO bot detection shows that (a) hand-picked per-minute averages (jumps/min, idle %, stops/min)
> measure one player's personal style rather than human-likeness and are easy to game, and
> (b) binary human-vs-bot classifiers cannot reliably rank two candidate bots against each other
> (Microsoft Navigation Turing Test, ICML 2021). The scorer below replaces the single scalar with
> hard gates, pairwise blind ranking anchored to human clips, and an explainable anti-bot
> adversary suite for diagnosis.

---

## 1. Layered scorer architecture (A–F)

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
  anchors has closed; count a change only when outside the uncertainty / bootstrapped 95% CI.
- **Third-person watching protocol** (Togelius & Yannakakis; Even et al. 2021): clips are judged by
  watching anonymized side-by-side **plots + event timelines** (not video, and not while playing
  alongside the bots) in **matched situations** (same activity, similar level band and zone type;
  names, level, zone, gear, class, and wall-clock session time stripped).
- **Judges:**
  - **You (`tools/bench/blind_rate.py`):** ground truth (a few minutes of blind ratings per round).
  - **LLM judge:** provides rating volume, and counts **only while it agrees with your blind
    ratings**.
- **Separate layers:** body (movement/combat clips), decisions (offline eval + loop detectors),
  and social (chat judged side-by-side against real human chat, where overly polished/articulate
  LLM prose counts as a **tell**, not a virtue — per Generative Agents "Smallville").

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
  - Self-similarity across sessions (Lee et al., NCSoft).
  - Collective / multi-bot trajectory clustering and habit similarity across bots vs across humans
    (NCSoft Lineage W 2024).
  - Chat phrasing repetition, party-play patterns, and whisper delay vs. message length.
  - Optional population pacing comparison (session length, zone/level progression) against the WoW
    Avatar History dataset (91,065 characters over 3 years).
- Hand metrics (`jumps/min`, `idle %`, walking turn rate deg/s, reaction from first hit) are kept
  strictly as **per-situation diagnostic distributions**, never as a reward.

### D. Meta-checks on the scorer itself (must pass before driving tuning)
Build a labelled calibration set:
- Frozen held-out human clips
- Current LLM bots
- Known-bad variants: (1) plain playerbots, (2) a **random-jumper** bot that fakes `jumps/min`,
  (3) an **instant-reply** bot (0.4 s whispers)

Before the scorer is allowed to drive any tuning loop, it must pass all three checks:
1. **Correct ranking:** humans > current bots > plain playerbots / random-jumper / instant-reply.
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
- **Strict edit scope:** tuning rounds edit only humanization/prompt/identity files; bug fixes and
  C++ game-judgment changes go separately (no hard-coded gameplay-judgment numbers in C++).

### F. Recording parity (prerequisite for everything above)
The server learns a human player's position and facing from client movement packets (every few
hundred ms), whereas a bot's facing changes instantaneously on the server. Polling both at 5 Hz
server-side would leak packet timing so every judge detects bots from packet cadence rather than
behaviour.
- **Record humans and bots the exact same way:** from what the server **broadcasts to other
  players** (`MSG_MOVE_*` / `SMSG_MONSTER_MOVE` / splines + cast, damage, loot, quest, chat
  broadcasts), with each packet's own timestamp — which is also what a real observer sees.
- Reconstruct observer-visible trajectories on a common timeline before feature extraction or
  rendering clips.
- Record **several sessions and activities** (questing, town, dungeons, chatting), ideally across
  more than one human player, holding out ~30% of sessions for testing.

---

## 2. Varied scenario suites (anti-overfitting)

To avoid overfitting to one class, starting zone, or dungeon, `soak` runs a stratified mix of
open-world questing and 5-man Tank/Healer/3DPS dungeon parties using `new:<race>:<class>` fixtures:

| Suite | Scenarios | Composition |
|---|---|---|
| `varied` (default tuning) | `soak_horde_rfc` + `soak_alliance_dm` | **Run 1:** 5 Horde questing classes + 5-man **Ragefire Chasm** L15. **Run 2:** 5 Alliance questing classes + 5-man **Deadmines** L20. |
| `heldout` (generalization check, never used for KEEP/REVERT) | `soak_northrend_hol` + `soak_heldout_uk` | **Run 1:** 5 mid/high Horde questing bots + 5-man **Halls of Lightning** L80. **Run 2:** 5 unseen Alliance classes + 5-man **Utgarde Keep** L70. |
| `quest` (legacy) | `soak` | 10× L1 Priest questing in Valley of Trials. |

---

## 3. Build order

1. **Recording parity (F):** observer-view broadcast recording (5 Hz-equivalent movement from
   broadcast packets/splines) + explicit timestamped events (`damage_taken`, `combat_start`,
   `loot`, `quest_accept/turnin`, `cast_start`, `chat in/out` with `text_len`), identical for
   humans and bots.
2. **Pairwise rating harness + `blind_rate.py` (B):** situation-segmented, metadata-stripped
   30–60 s clips rendered as plots/timelines; TrueSkill / Bradley–Terry ranking with human anchors
   and uncertainty; interactive blind rating CLI + LLM judge agreement tracking.
3. **Known-bad bots & scorer calibration suite (D):** held-out human vs current bots vs plain
   playerbots / random-jumper / instant-reply meta-tests.
4. **Adversary suite & diagnostic classifier (C):** per-situation `scikit-learn` classifier
   (diagnostic only) + published MMO anti-bot features (waypoint repetition, inter-action timing
   regularity, cross-session self-similarity, multi-bot clustering).
5. **Long-horizon & population metrics:** multi-session route repetition, inter-bot vs inter-human
   similarity, chat phrasing diversity, and optional WoW Avatar History pacing comparison.

---

## References
- Navigation Turing Test (Microsoft Research, ICML 2021)
- MineRL BASALT Competition & Retrospective (TrueSkill pairwise human/agent ranking)
- BotPrize (Unreal Tournament believability Turing test)
- Togelius & Yannakakis, "Assessing Believability"; Even et al. (2021) believability protocol
- Generative Agents ("Smallville", Stanford HAI — LLM polish vs human chat warning)
- Gran Turismo Sophy (reaction-rate caps & sportsmanship as hard gates)
- Mitterhofer et al. (2009), Server-side Bot Detection in MMOs (WoW waypoint repetition)
- Gianvecchio et al., "Battle of Botcraft" (WoW timing irregularity)
- NCSoft & Korea University (Lee et al. 2016; Aion 2023 action intervals; Lineage W 2024
  trajectory clustering; chat & party-play bot detection)
- World of Warcraft Avatar History dataset (MMSys 2011)
- Lopez-Paz & Oquab, "Revisiting Classifier Two-Sample Tests" (https://arxiv.org/abs/1610.06545);
  Conformal C2ST (https://arxiv.org/abs/2507.17026): a weak classifier "passing" proves nothing
