# Experimental / parked work

Code and docs parked here are **not** run by `tools/wlb check` or CI, and nothing outside
`experimental/` imports from here.

- **`human_likeness/`** — Phase D human-likeness layered scorer (`layered_scorer.py`,
  `trace_parity.py`, `pairwise_ranker.py`, `blind_rate.py`, `agreement.py`,
  `adversary_suite.py`, `population_metrics.py`, `known_bad_bots.py`,
  `scorer_calibration.py`, `hill_climb.py`, `scorer-lock.sha256`, and unit tests).
  - *Last commit where all tests passed:* `f703c90` (before the 2026-11-24 cleanup).
  - *How to bring back:* move the scripts back under `tools/bench/` (and `scorer-lock.sha256`
    to `config/`), re-enable their unit tests in `tools/wlblib/check.py`, and re-wire the
    optional human-likeness flags in `tools/wlblib/cases/soak.py`.
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
