# wow-llm-bots: rules for working in this repo

Design and decisions: `docs/design.md`. Current phase plan: `docs/plans/`.

## C++ (our code: `common/`, `harness/`, `server_module/`)

**When in doubt, do what Chromium does.** Google/Chromium C++ style, clang-format
`BasedOnStyle: Chromium` (root `.clang-format`). Code in `third_party/` keeps its own style.

Use our `common/base` equivalents, never the std/Boost ones:

| Don't | Use |
|---|---|
| `std::thread`, `std::this_thread` | `base::Thread`, `base::ThreadPool`, `base::PlatformThread` (incl. `Sleep`, `CurrentId`) |
| `std::shared_ptr`, `std::make_shared`, `enable_shared_from_this` | `base::RefCountedThreadSafe<T>`, `base::scoped_refptr<T>`, `base::MakeRefCounted<T>` |
| `std::function` for callbacks, raw lambdas capturing `this` across threads | `base::OnceCallback` / `RepeatingCallback`, `base::BindOnce` / `BindRepeating` with a `base::WeakPtr` receiver |
| `std::chrono` in APIs | `base::TimeDelta` (`base::Milliseconds(n)`, `base::Seconds(n)`), `base::TimeTicks::Now()` |
| mutexes + shared state between threads | post tasks to the owning sequence (`SequencedTaskRunner::PostTask`, `PostTaskAndReplyWithResult`) |
| Boost.Asio / libevent / custom event loops | `base::Thread` with `MessagePumpType::IO` + `CurrentIOThread::Get()->WatchFileDescriptor(...)`; sockets in `common/net` |

`std::chrono` and `std::mutex`/`condition_variable` are fine *inside* `common/base`'s
implementation of these primitives, nowhere else.

**Include what you use.** Every file includes exactly the headers whose symbols it uses: no
unused includes, no relying on transitive includes. Check with `tools/check-includes.sh`
(`--fix` to apply). glibc's internal headers (`<bits/...>`, `<asm-generic/...>`) are never
included: use the public header and mark it `// IWYU pragma: keep`.

**Before showing C++ for review or committing it, `tools/check-cpp.sh` must pass:**
clang-format, include check, and build + unit tests in default, ASan+UBSan, TSan and MSan
(MSan links an instrumented libc++ built by `tools/build-msan-libcxx.sh`). New code comes
with unit tests. A syscall MSan has no interceptor for gets `base::MsanUnpoison`
(`base/sanitizers.h`), never a suppression.

Our AzerothCore module (`server_module/`) follows the same rules; AzerothCore's own APIs keep
their names. It must never block the world thread.

## Repo

- Public-ready private repo: no secrets or personal details in tracked files; those go in the
  `private/` submodule. Commits use the noreply email set in the repo config.
- Upstream code only in `third_party/` (pinned submodules); never edit it.
- Build outputs in `out/`, runtime data in `server/` and `var/` (untracked).
- Settings live in `config/*.overrides`, rendered by `tools/apply-config.sh`.
