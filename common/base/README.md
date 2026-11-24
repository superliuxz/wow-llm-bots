# `common/base/` (Frozen)

Minimal Chromium-style runtime primitives (`Lock`, `ConditionVariable`,
`WaitableEvent`, `Thread`, `ThreadPool`, `TaskQueue`, `MessagePumpEpoll`,
`OnceCallback`/`RepeatingCallback`, `WeakPtr`, `raw_ptr`, `RefCountedThreadSafe`,
`TimeTicks`/`TimeDelta`, and ASCII string helpers) shared across `common/`,
`snapshot/`, `harness/`, and `server_module/`.

**Status: frozen.** Every header here is actively used and covered by
`wlb_common_unittests`. Do not add new general-purpose primitives or rewrite
this layer unless fixing a verified bug.
