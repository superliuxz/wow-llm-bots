# wow-llm-bots: rules for working in this repo

Design and decisions: `docs/design.md`. Current phase plan: `docs/plans/`.

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
| `std::thread`, `std::this_thread` | `base::Thread`, `base::ThreadPool`, `base::PlatformThread` (`Sleep`, `CurrentId`) |
| `std::promise`, `std::future` | post a reply task (`PostTaskAndReplyWithResult`); block only in startup handshakes and tests, with `base::WaitableEvent` |
| Boost.Asio, libevent, custom event loops | `base::Thread` with `MessagePumpType::IO` + `CurrentIOThread::Get()->WatchFileDescriptor(...)`; sockets in `common/net` |
| shared state across threads | post tasks to the owning sequence (`SequencedTaskRunner::PostTask`) |

Loosened (the reason doesn't apply to us):

| Facility | Rule |
|---|---|
| `std::chrono` | Banned in APIs (use `base::TimeDelta`, `base::TimeTicks`); fine inside `common/base` implementation. |
| `std::to_string` | Fine for integers (its ban is about locale-dependent floats); never for floating point. |

Allowed as in Chromium: `std::unique_ptr`/`make_unique`, `std::optional`, `std::string_view`,
`std::atomic`, `std::invoke`/`apply`, standard containers, `<compare>`.

Layout mirrors Chromium: `base/functional/`, `base/memory/`, `base/task/`, `base/threading/`,
`base/synchronization/`, `base/message_loop/`, `base/time/`, `base/compiler_specific.h`.

**Safety build settings:** our code builds with `-Wthread-safety -Werror=thread-safety`.
Hardened standard library (`_GLIBCXX_ASSERTIONS`, libc++ extensive hardening) is on for
whole-program builds we own (`common/` tests, the harness); it is off for the copy of `common/`
linked into AzerothCore (`out/common-server`) and for the module's sources, because hardening
must cover a whole program and AzerothCore isn't built hardened.

**C++ standard:** `common/` and the server module stay C++20 (AzerothCore's `modules` target is
C++20 and includes our headers). The harness, a separate build, may use C++26.

**Include what you use.** Every file includes exactly the headers whose symbols it uses: no
unused includes, no relying on transitive includes. Check with `tools/check-includes.sh`
(`--fix` to apply). glibc's internal headers (`<bits/...>`, `<asm-generic/...>`) are never
included: use the public header and mark it `// IWYU pragma: keep`.

**Before showing C++ for review or committing it, `tools/check-cpp.sh` must pass:**
clang-format, include check, and build + unit tests in default, ASan+UBSan, TSan and MSan
(MSan links an instrumented libc++ built by `tools/build-msan-libcxx.sh`). New code comes
with unit tests. A syscall MSan has no interceptor for gets
`MSAN_UNPOISON` (`base/compiler_specific.h`), never a suppression.

Our AzerothCore module (`server_module/`) follows the same rules; AzerothCore's own APIs keep
their names. It must never block the world thread.

## Repo

- Public-ready private repo: no secrets or personal details in tracked files; those go in the
  `private/` submodule. Commits use the noreply email set in the repo config.
- Upstream code only in `third_party/` (pinned submodules); never edit it.
- Build outputs in `out/`, runtime data in `server/` and `var/` (untracked).
- Settings live in `config/*.overrides`, rendered by `tools/apply-config.sh`.
