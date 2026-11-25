# Contributing to wow-llm-bots

Design: [docs/design.md](docs/design.md). Open work: [docs/TODO.md](docs/TODO.md) (update it in
the same commit when open priorities change; `git log` is the history of finished work).

## Core Rules

1. **The brain decides; C++ is a pure harness.** C++ perceives, measures, enforces client rules,
   and runs geometry/pathing/timing. Never hard-code class/role/targeting, non-grey selling, or
   gameplay judgments in C++ — expose facts and mechanically valid options and let the LLM decide.
2. **Verify before committing.** Re-read your `git diff`, run `tools/wlb check`, and rebuild
   `out/server-test` (`tools/wlb build server --test`) after any `server_module/` edit. Live-verify
   behavior changes when practical and note anything unverified honestly.
3. **Never re-lock scorer manifests without explicit user approval.** Never read or modify
   `~/.local/share/wow-llm-bots/relock.passphrase`, `~/.local/share/wow-llm-bots/scorer.sha256`,
   or `experimental/human_likeness/scorer-lock.sha256` on your own.
4. **No secrets or personal details in tracked files.** They go in the `private/` submodule;
   commits use the noreply email set in the repo config. Never publish or copy
   `private/BigFootAddOns` (proprietary addons) into the public GPL repo.
5. **Never run `tools/wlb remote` unprompted.** Only offload work to configured workers
   (`private/remotes.json`) when the user explicitly asks.

## C++ Style (`common/`, `snapshot/`, `harness/`, `server_module/`)

Follow Google/Chromium C++ style (`.clang-format`, `BasedOnStyle: Chromium`). `common/`,
`snapshot/`, and `server_module/` stay **C++20** (AzerothCore compatibility); `harness/` uses
**C++26**. `common/base/` is **frozen** (see `common/base/README.md`).

| Avoid | Use instead |
|---|---|
| `std::function`, `std::bind`, raw `this` lambdas across threads | `base::OnceCallback` / `RepeatingCallback` + `base::BindOnce`/`BindRepeating` with `base::WeakPtr` |
| `std::shared_ptr` / `enable_shared_from_this` | `std::unique_ptr`, or `base::RefCountedThreadSafe<T>` + `base::scoped_refptr<T>` |
| `std::mutex`, `std::condition_variable` | `base::Lock` + `base::AutoLock` + `base::ConditionVariable` with `GUARDED_BY(lock_)` (`-Werror=thread-safety`) |
| `std::thread`, `<pthread.h>` (outside `common/base/`) | `base::Thread`, `base::ThreadPool`, `base::PlatformThread` |
| `std::promise` / `std::future` | `base::PostTaskAndReplyWithResult` (`base::WaitableEvent` only in startup/tests) |
| `std::set`, `std::map`, `absl::*` | `std::vector` (C++20) or `std::flat_map`/`std::flat_set` (C++26 `harness/`); `common/base/strings/` |
| Non-owning raw `T*` **fields** | `base::raw_ptr<T>` (locals and params stay plain `T*`) |
| `std::chrono` in APIs; `std::to_string` on floats | `base::TimeTicks` / `base::TimeDelta`; `base::StringPrintf` |

- **IWYU:** Every file includes exactly what it uses (`tools/wlb check includes [--fix]`). Never
  include `<bits/...>` or `<asm/...>`; keep non-self-contained third-party headers via
  `// IWYU pragma: keep`.
- **World thread:** `server_module/` must never block the AzerothCore world thread.

## Checks & Tests

- **`tools/wlb check`** (~10 s): `clang-format`, `RULES.md` grammar sync, default build + unit
  tests (`wlb_common_unittests`, `llm_brain_unittests`, `harness_unittests`), snapshot validation
  + hover/fixture checks, `clang-include-cleaner`, C++ screen goldens, and Python bench/tool unit
  tests.
- **Do not run sanitizers locally** (`tools/wlb check sanitizers` — ASan/UBSan/TSan/MSan); leave
  those to CI unless explicitly asked.
- **Test targets (`tools/wlb test <target>`):** `unit`, `e2e` (6 grouped files in
  `tools/wlblib/cases/e2e_*.py` on isolated `out/server-test`), `perf`, `eval`, `soak`.

## Repo Layout

- `common/`, `snapshot/`, `server_module/`, `harness/`: production C++.
- `content/`: `RULES.md`, boss `notes.md`, character identities (model inputs, not docs).
- `tools/wlb` + `tools/wlblib/`: single CLI entry point (`check`, `build`, `test`, `server`,
  `db`, `config`, `console`, `llm`, `snap`, `trace`, `gen`, `remote`).
- `experimental/`: parked research code and docs (skipped by `wlb check` and CI; never imported
  by core code).
- `third_party/`: pinned upstream submodules — never edit.
- `out/` (untracked, regenerable) vs `var/` (untracked, persistent DB/logs/bot memory) vs
  `private/` (untracked/private submodule for secrets and `remotes.json`).
