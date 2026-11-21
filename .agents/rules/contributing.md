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
   - Never read or modify `~/.local/share/wow-llm-bots/relock.passphrase`, run `tools/bench/hill_climb.py lock-scorer`, or modify `~/.local/share/wow-llm-bots/scorer.sha256` or tracked `config/scorer-lock.sha256` on your own (`hill_climb.py` requires the external passphrase on `/dev/tty`, while tracked `config/scorer-lock.sha256` exposes any relock in `git diff`); if locked measurement files changed, leave both lock files untouched and wait for the user's explicit approval in the current turn before relocking.
7. **Independent zero-context subagent review after every commit (`.agents/hooks.json`):**
   - After every `git commit` (unless the user explicitly says to skip review), `.agents/hooks/post_commit_review.py` writes `var/review/head.patch` (`git show HEAD --stat -p` + status + recent log). Invoke a fresh `research` subagent (`invoke_subagent` with `TypeName: "research"`, `Role: "Independent Commit Reviewer"`) using **only** this neutral prompt (do not add your own rationale, excuses, or summary):
     `"Independent cold review of commit <short_sha>: 1) Read CONTRIBUTING.md, docs/TODO.md, and docs/design.md. 2) Read var/review/head.patch and inspect the touched files, headers, tests, and callers in the repo. 3) Audit from two angles: (a) Code & C++ rules (Chromium C++ style, C++20 in common/snapshot/server_module vs C++26 in harness, banned std:: facilities vs common/base replacements, 'the brain decides; C++ is a pure harness', boundary conditions, state interactions, algorithmic complexity claims, and unit/live test coverage); (b) Honesty & stats/docs (every claim in the commit message, docs/TODO.md, and any report matches the actual code and var/ artifacts, with multiple-comparison correction where applicable). Report findings grouped by High, Medium, and Low (or 'No findings' if clean)."`
   - After calling `invoke_subagent`, stop calling tools and wait for the subagent's review message. If the reviewer reports any High, Medium, or Low findings, follow Rule #2 (`docs/TODO.md` first → fix & verify one by one → commit & push).
