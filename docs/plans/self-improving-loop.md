# Self-Improving Human-Likeness Loop

A hill-climbing loop for making bots play and move more like humans while guaranteeing
performance, stability, and task progress never regress (`docs/design.md`, "Evals").

## 1. Fixed, tamper-checked scorer

The scorer lives in `tools/bench/soak_scorer.py` + `tools/bench/human_metrics.py` and is invoked
through one command:

```bash
tools/wlb test soak --suite varied --minutes 5 --runs 2 --score
```

### Varied scenario suites (anti-overfitting)

To avoid overfitting to one class, starting zone, or dungeon, `soak` runs a stratified mix of
open-world questing and 5-man Tank/Healer/3DPS dungeon parties using `new:<race>:<class>` fixtures
in `FixtureLoader` + `llmtest group`/`gear`/`tele`:

| Suite | Scenarios | Composition |
|---|---|---|
| `varied` (default tuning) | `soak_horde_rfc` + `soak_alliance_dm` | **Run 1:** 5 Horde questing classes (Orc War, Tauren Dru, Undead Mag, BE Pal, L15 Troll Hun) + 5-man **Ragefire Chasm** L15 (War/Pri/Rog/Mag/Sha). **Run 2:** 5 Alliance questing classes (Hum Pal, NE Hun, Dwf War, Gnm Loc, L15 Dra Sha) + 5-man **Deadmines** L20 (Pal/Dru/Rog/Mag/Hun). |
| `heldout` (generalization check, never used for KEEP/REVERT) | `soak_northrend_hol` + `soak_heldout_uk` | **Run 1:** 5 mid/high Horde questing bots (L15–L70 Silverpine/STV/Borean Tundra) + 5-man **Halls of Lightning** L80. **Run 2:** 5 unseen Alliance classes + 5-man **Utgarde Keep** L70. |
| `quest` (legacy) | `soak` | 10× L1 Priest questing in Valley of Trials. |

It prints a single composite **0–100 human-likeness score** plus hard guardrails:
- **Human-likeness distributions:** path straightness, stops/min, jumps/min, instant `>90°`
  turns/min, idle %, aggro reaction time p50, whisper reply delay p50. Compares directly against
  your recorded play traces in `var/human_baseline/human/` (or documented fallback targets until
  human traces exist).
- **Optional LLM judge (`--judge`):** anonymized paired short trace clips (human control vs bot);
  50% detection accuracy = 100 pts.
- **Hard guardrails (any failure rejects the change):** 0 crashes, 0 LLM errors, 0 invalid plans,
  `max_wakes/min/bot <= 20`, `do:` p95 `<= 2000 ms`, bounded stuck bots, bounded deaths, and
  optional offline eval non-regression.
- **Tamper check:** `tools/bench/hill_climb.py lock-scorer` records the SHA-256 of
  `soak_scorer.py` + `human_metrics.py` in `var/scorer.sha256` and every run checks it. For
  unattended overnight runs, copy the scorer and your human traces into a read-only directory
  outside the repo (e.g. `chmod -R a-w ~/.local/share/wlb-eval/`) and pass `--human-dir`.

## 2. Recording your human baseline (same trace format)

Enable unified behavior tracing on the play server (in `config/mod-llm-brain.overrides` or local
override):

```ini
LlmBrain.TraceDir = "/mnt/storage/wow-llm-bots/var/human_baseline"
LlmBrain.ObserveHumans = 1
```

Both your character (`var/human_baseline/human/<Name>/<date>.jsonl`) and bots
(`.../bot/<Name>/<date>.jsonl`) are logged with the exact same `TraceSample` / `TraceChat` schema.
Keep ~30% of your human trace files held out in a separate directory the agent and judge never see,
for periodic blind checks.

## 3. Running the overnight agent loop (with `/goal`)

Run while you are not playing (the isolated test stack shares the 5090 GPU with `llama-server`):

1. **Initialize baseline once:**
   ```bash
   python3 tools/bench/hill_climb.py init --minutes 2 --runs 2
   ```
2. **Each round** (repeat on a branch):
   1. Run `python3 tools/bench/hill_climb.py status` and pick the **worst metric or bot tell**.
   2. Form **one** concrete hypothesis and make **one** focused change.
   3. Evaluate:
      ```bash
      python3 tools/bench/hill_climb.py eval-candidate \
        --category code \
        --target jumps_per_min \
        --hypothesis "Add occasional organic jumps during long walks" \
        --minutes 2 --runs 2 --auto-revert
      ```
   4. `eval-candidate` verifies the scorer hash, runs `tools/wlb check`, runs the multi-run soak
      and scorer, logs the round to `var/experiments/ledger.jsonl`, and **keeps** the change only
      if `guardrails_ok` and `score > best + min_delta` (otherwise auto-reverts).

## 4. Split what may be changed

| What | How to tune it |
|---|---|
| **Numbers** (reaction delays, typo rate, chattiness, skill sliders) | Plain random / Bayesian search over a config knob — cheaper than an LLM agent. |
| **`content/RULES.md`, identities, prompt layout** | Agent, scored on soak + offline `tools/wlb test eval`. |
| **Code** (movement jitter, turning, pauses, playerbots settings) | Agent, with `tools/wlb check` unit tests required. Most bot tells live here. |

## 5. Guarding against gaming the metric

- **Human traces required (no fallback scoring):** `hill_climb.py` refuses to score when
  `var/human_baseline/human/*.jsonl` is empty (unless `--allow-fallback` is explicitly passed for a
  smoke test). Record several hours of real human play before running a tuning loop.
- **Full measurement lock manifest:** `lock-scorer` hashes not only `soak_scorer.py` and
  `human_metrics.py`, but also `tools/wlblib/cases/soak.py`, `snapshot/behavior_trace.{h,cc}`,
  `llm_brain_world_script.cc` (trace call sites), and all `config/tests/soak_*.llm-brain.overrides`
  into `var/scorer.sha256`.
- **Noise measurement (`2σ` + confirmation re-run):** `init` runs `--baseline-runs 5` to measure
  `baseline_std` (`σ`). `eval-candidate` requires `score > best + max(min_delta, 2*σ)` and re-runs
  any winning candidate once to confirm before keeping.
- **Progress guardrails + held-out gate on every KEEP:** fixed `xp/h` floor (default `3000 xp/h`)
  and fixed deaths cap (default `<= 2` deaths per run) must pass alongside all scorer guardrails;
  every confirmed KEEP also runs `--suite heldout` and reverts if any held-out guardrail fails.
- **Strict edit scope:** `eval-candidate` rejects edits outside `human_pacing*`, `content/RULES.md`,
  and `content/identities/*` by default. Bug fixes and C++ game-judgment changes go as separate
  reviewable PRs, never mixed into a scored round.
- **Blind judge & human review:** `--judge` is enabled by default alongside numeric metrics; do a
  short blind check of traces/clips yourself after each night.
