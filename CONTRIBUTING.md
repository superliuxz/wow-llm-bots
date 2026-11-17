# wow-llm-bots: rules for working in this repo

Design and decisions: `docs/design.md`. Current phase plan: `docs/plans/`. **Open work:
`docs/TODO.md`: read it first in every session, and keep it current (next section).**

## Keep docs/TODO.md current

`docs/TODO.md` is the one list of unfinished work, so that whoever (or whichever model) comes next
knows where things stand without this conversation. Update it **in the same commit** as the work:

- You start something: put it under "In progress". You finish it: move it to "Done" (keep about
  ten) and take it off the other lists.
- You defer, skip or only partly do something, find a bug you don't fix, or decide something you
  don't build: add a line to "Next", "Later" or "Loose ends", with a pointer to the document that
  has the detail. Never leave a known gap only in a chat message or a commit body.
- A plan or spec changes what is open: change the same lines in TODO.md.
- Something you could not verify (a command you did not run, a claim you could not check) goes in
  "Loose ends", with what is needed to verify it.
- Keep it short: one entry per item, detail in the plan or spec it points to. Update "Last updated".

Before ending a session or handing over, re-read TODO.md and make it match the repo.

## The brain decides; C++ is a pure harness

Our C++ perceives, measures, estimates, executes, and does math an LLM is bad at (geometry,
routing, timing). Judgments (what's worth doing, risk, priorities, when to act) belong to the
LLM brain: expose them as parameters the brain sets, with neutral defaults. No hard-coded
"if this then that" game logic in C++: it defeats the purpose (docs/design.md).

## C++ (our code: `common/`, `harness/`, `server_module/`)

**When in doubt, do what Chromium does.** Follow the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) and the
[Chromium C++ style guide](https://chromium.googlesource.com/chromium/src/+/main/styleguide/c++/c++.md)
(naming, comments, ownership, headers, etc.); clang-format `BasedOnStyle: Chromium` (root
`.clang-format`). Code in `third_party/` keeps its own style.

**Before using any `std::` facility, look up how Chromium does it:** check Chromium's
[allowed/banned C++ features list](https://chromium.googlesource.com/chromium/src/+/main/styleguide/c++/c++-features.md)
and whether `//base` has an equivalent. If Chromium bans or wraps it, use (or add to
`common/base`) the Chromium-style equivalent instead. Only use it directly if Chromium allows it.

**Why Chromium restricts things:** C++ makes memory and lifetime bugs easy, and chasing them
is miserable. Most bans exist for safety (use-after-free through callbacks, unclear ownership,
unchecked locking); some only because `//base` already has an equivalent. **Chromium's list is
the default. Deviate only when the ban's reason clearly doesn't apply, and write the reason in a
comment next to the code.**

Strict (safety reasons), use `common/base`:

| Don't | Use |
|---|---|
| `std::function`, `std::bind`, lambdas capturing `this` posted across threads | `base::OnceCallback` / `RepeatingCallback`, `base::BindOnce` / `BindRepeating` with a `base::WeakPtr` receiver |
| `std::shared_ptr`, `std::make_shared`, `enable_shared_from_this` | single ownership (`std::unique_ptr`), or `base::RefCountedThreadSafe<T>` + `base::scoped_refptr<T>` (`base::MakeRefCounted`) |
| `std::mutex`, `lock_guard`, `unique_lock`, `std::condition_variable` | `base::Lock` + `base::AutoLock` + `base::ConditionVariable` (`base/synchronization/`), with `GUARDED_BY(lock_)` on guarded members (`base/thread_annotations.h`); `-Werror=thread-safety` checks it at compile time |
| `std::thread`, `std::this_thread`, `<pthread.h>` (outside `common/base/`) | `base::Thread`, `base::ThreadPool`, `base::PlatformThread` (`Sleep`, `CurrentId`); `<pthread.h>` is encapsulated inside `common/base/` only |
| `std::promise`, `std::future` | post a reply task (`PostTaskAndReplyWithResult`); block only in startup handshakes and tests, with `base::WaitableEvent` |
| Boost.Asio, libevent, custom event loops | `base::Thread` with `MessagePumpType::IO` + `CurrentIOThread::Get()->WatchFileDescriptor(...)`; sockets in `common/net` |
| shared state across threads | post tasks to the owning sequence (`SequencedTaskRunner::PostTask`) |
| `absl::*` / `#include "absl/..."` | `common/base/` (`base/strings/` for `StrCat`, `StringPrintf`, `JoinString`, `SplitString`, `TrimWhitespaceASCII`, `StringToInt`, `NumberToString`, ...). `third_party/abseil-cpp` exists solely as `protobuf`'s internal build dependency and is never included or linked directly by our code (`common/`, `harness/`, `server_module/`). |
| `std::set`, `std::map`, `std::multiset`, `std::multimap` (`<set>`, `<map>`) | `std::vector` (in C++20 `common/` and `server_module/`) or C++23 `std::flat_map` / `std::flat_set` (`<flat_map>`, `<flat_set>` in C++26 `harness/`) — contiguous storage instead of per-node red-black tree allocations |
| raw `T*` pointer **fields** (non-owning) | `base::raw_ptr<T>` (`base/memory/raw_ptr.h`); make the pointee a `base::RawPtrTarget` so destroying it while a `raw_ptr` still points to it crashes immediately ("dangling raw_ptr"); `base::MayBeDangling<T>` only for fields that dangle by design (with a comment); delete through one with `ClearAndDelete()`. Locals and parameters stay plain `T*`; owning pointers are `std::unique_ptr` / `scoped_refptr`. |

Loosened (the reason doesn't apply to us):

| Facility | Rule |
|---|---|
| `std::chrono` | Banned in APIs (use `base::TimeDelta`, `base::TimeTicks`); fine inside `common/base` implementation. |
| `std::to_string` | Fine for integers (its ban is about locale-dependent floats); never for floating point. |

Allowed as in Chromium: `std::unique_ptr`/`make_unique`, `std::optional`, `std::string_view`,
`std::atomic`, `std::invoke`/`apply`, standard containers, `<compare>`.

Layout mirrors Chromium: `base/functional/`, `base/memory/`, `base/strings/`, `base/task/`,
`base/threading/`, `base/synchronization/`, `base/message_loop/`, `base/time/`,
`base/compiler_specific.h`.

**Safety build settings:** our code builds with `-Wthread-safety -Werror=thread-safety`.
Hardened standard library (`_GLIBCXX_ASSERTIONS`, libc++ extensive hardening) is on for
whole-program builds we own (`common/` tests, the harness); it is off for the copy of `common/`
linked into AzerothCore (`out/common-server`) and for the module's sources, because hardening
must cover a whole program and AzerothCore isn't built hardened.

**C++ standard:** `common/` and the server module stay C++20 (AzerothCore's `modules` target is
C++20 and includes our headers). The harness, a separate build, may use C++26.

**Include what you use.** Every file includes exactly the headers whose symbols it uses: no
unused includes, no relying on transitive includes. Check with `tools/wlb check includes`
(`--fix` to apply). glibc's internal headers (`<bits/...>`, `<asm-generic/...>`) are never
included: use the public header and mark it `// IWYU pragma: keep`.

**Before showing C++ for review or committing it, `tools/wlb check` must pass:**
clang-format, snapshot/trigger validation, default build + unit tests, include check, and screen
goldens (~5 s). Run `tools/wlb check sanitizers` (or `tools/wlb check --sanitizers`) to also build
and test under ASan+UBSan, TSan and MSan (MSan links an instrumented libc++ built by
`tools/wlb setup msan`) before committing C++ changes that touch memory, lifetimes, threading or
syscalls. New code comes with unit tests. A syscall MSan has no interceptor for gets
`MSAN_UNPOISON` (`base/compiler_specific.h`), never a suppression.

**Four test targets**, each its own `tools/wlb test <target>`: `unit` (gtest suites, no game or
GPU; the `unit_tests` CMake target; `tools/wlb check sanitizers` runs it under every sanitizer), `e2e` (live
scenarios against a real worldserver, pass or fail), `perf` (what things cost, each against a
stored baseline and a hard limit: `tools/wlblib/cases/perf_baselines.json`) and `eval` (how well the brain decides, scored). Cases are modules in
`tools/wlblib/cases/<target>_<name>.py`. A change to what the bot sees or does gets a unit test, and
an e2e case when it only shows in the game.

Our AzerothCore module (`server_module/`) follows the same rules; AzerothCore's own APIs keep
their names. It must never block the world thread.

## Repo

- Public-ready private repo: no secrets or personal details in tracked files; those go in the
  `private/` submodule. Commits use the noreply email set in the repo config.
- Upstream code only in `third_party/` (pinned submodules); never edit it.
- Everything generated is in `out/<name>` (untracked) and may be deleted at any time (`out/server` is the
  installed game server, rebuilt by `tools/wlb build server` and friends). Anything that must survive is
  in `var/` (untracked): the MySQL database, logs, bot memory. Never put state worth keeping in `out/`.
- Settings live in `config/*.overrides`, rendered by `tools/wlb config`.
