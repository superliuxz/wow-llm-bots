# Experimental / parked work

Code and docs parked here are **not** run by `tools/wlb check` or CI, and nothing outside
`experimental/` imports from here.

- **`human_likeness/`** — Phase D human-likeness layered scorer (`layered_scorer.py`,
  `trace_parity.py`, `pairwise_ranker.py`, `blind_rate.py`, `agreement.py`,
  `adversary_suite.py`, `population_metrics.py`, `known_bad_bots.py`,
  `scorer_calibration.py`, `hill_climb.py`, `scorer-lock.sha256`, and unit tests).
  - *Last commit where all tests passed:* `f703c90` (before the 2026-11-24 cleanup).
  - *How to bring back:* moving the scripts back is **not enough** (tried 2026-11-24 in a
    scratch worktree): the scorer's support was removed around them.
    - `tools/bench/soak_scorer.py` was split in `eef8b62`: `score_soak()` lost the layered
      scoring (`blind_ratings_path`, human whisper timing, Layer A/B/C checks) and the module-level
      `MIN_HUMAN_SAMPLES`, `scorer_sha256()`, `verify_scorer_hash()` and `_filter_role_rows()`
      that `hill_climb.py` calls. Restore it from `f703c90` (keep the current stall/XP fixes).
    - The behavior trace was shrunk to combat/soak events in `5e93be7`: the chat, whisper-timing
      and hub events these scorers read are no longer written. Restore that part of
      `server_module/src/snapshot/behavior_trace*.{h,cc}` and `docs/specs/trace-events.md`
      (now `experimental/docs/`).
    - Then move the scripts to `tools/bench/` (and `scorer-lock.sha256` to `config/`), re-enable
      their tests in `tools/wlblib/check.py`, re-wire the human-likeness flags in
      `tools/wlblib/cases/soak.py`, and ask the user to relock.
    - The simplest route is to start from `f703c90` for these files and merge forward.
- **`notes_hill_climb/`** — Automated boss-notes hill-climb loop (`notes_hill_climb.py` + test).
  Parked because its screening trials share data with the significance test (winner's curse);
  edit `content/notes/*/notes.md` by hand until a clean held-out confirm loop is needed.
  - *Last commit where all tests passed:* `f703c90`.
  - *How to bring back:* move back to `tools/bench/`, fix screening vs confirmation split, and
    re-add `notes_hill_climb_test.py` to `tools/wlblib/check.py`.
- **`docs/`** — Parked design docs (`self-improving-loop.md`, `trace-events.md`).
  - *Last commit where referenced:* `5e93be7`.
  - *How to bring back:* move back to `docs/plans/` or `docs/specs/` if the parked research loop
    is revived.
