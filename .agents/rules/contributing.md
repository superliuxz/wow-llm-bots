---
trigger: always_on
---

# Repository Rules (`CONTRIBUTING.md` & `docs/TODO.md`)

Always follow `CONTRIBUTING.md` and keep `docs/TODO.md` current on every task:

1. **Read `CONTRIBUTING.md` and `docs/TODO.md` at the start of every session.**
2. **Standard review-fix procedure (MUST follow in exact order):**
   - **Feedback → update `docs/TODO.md` with 100% honesty FIRST** — record every High, Medium, and Low finding verbatim (and fix any `Done` overclaims) **without modifying any code yet**.
   - **Fix and verify each item one by one** (High → Medium → Low).
   - **Check off an item in `docs/TODO.md` ONLY after it is implemented AND verified** (tests or live checks actually executed, never assumed), then commit and push.
3. **The brain decides; C++ is a pure harness:**
   - Never encode class/role/targeting or non-grey selling heuristics or hard-coded gameplay judgments in C++. Expose state and mechanically valid options transparently and let the LLM brain decide.
4. **Update `docs/TODO.md` in the same commit as the work** and re-read it before handing over.
5. **Double-check yourself; commits must create no extra bugs:**
   - Re-read your full `git diff` line-by-line before committing (check boundary conditions, state interactions, idempotency, and callers), run `tools/wlb check` (plus live verification for server/benchmark changes), and never ship unverified assumptions or new regressions.
6. **Never re-lock `scorer.sha256` without the user's explicit OK:**
   - Never set `WLB_CONFIRM_RELOCK`, run `tools/bench/hill_climb.py lock-scorer`, or modify `~/.local/share/wow-llm-bots/scorer.sha256` or tracked `config/scorer-lock.sha256` on your own (`hill_climb.py` also ignores `--lock-file` and checks `/proc` ancestors in agent runs); if locked measurement files changed, leave both lock files untouched and wait for the user's explicit approval in the current turn before relocking.
