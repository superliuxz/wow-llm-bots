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
