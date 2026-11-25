---
trigger: always_on
---

Read and follow `CONTRIBUTING.md` (and `docs/TODO.md` for open work) on every session.
Every behavior commit must include adversarial counter-tests, check `InspectPrompt` byte/token budget when prompts/rules/screens change, and pass full `tools/wlb test e2e` + `tools/wlb test eval` before stacking further commits.
