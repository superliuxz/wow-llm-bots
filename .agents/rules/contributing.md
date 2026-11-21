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
   - Never read or modify `~/.local/share/wow-llm-bots/relock.passphrase`, run `tools/bench/hill_climb.py lock-scorer`, or modify `~/.local/share/wow-llm-bots/scorer.sha256` or tracked `config/scorer-lock.sha256` on your own. The passphrase file is not a technical barrier against an agent running as the same Unix user; what actually protects is this rule plus tracked `config/scorer-lock.sha256` exposing any relock in `git diff` (for an OS-level barrier, use a separate Unix user or hardware-key prompt). If locked measurement files changed, leave both lock files untouched and wait for the user's explicit approval in the current turn before relocking.
7. **Independent zero-context post-commit review in a separate conversation (`.agents/hooks.json`):**
   - Whenever `HEAD` advances to a new descendant commit after a `run_command` (e.g., `git commit`), `.agents/hooks/post_commit_review.py` automatically writes `var/review/patch_<short_sha>.patch` (`last_reviewed..HEAD`, also `var/review/head.patch`) and spawns a detached, read-only `agy --mode plan --print=<prompt>` reviewer in its own top-level conversation (minimal env, zero shared chat history, non-blocking; opt out with `WLB_SKIP_COMMIT_REVIEW=1`).
   - Review output is written to `var/review/review_<short_sha>.md` (symlinked at `var/review/latest.md`). When the user shares or points to review findings, follow Rule #2 (`docs/TODO.md` first → fix & verify one by one → commit & push).
