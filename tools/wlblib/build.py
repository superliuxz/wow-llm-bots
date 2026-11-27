"""`wlb build`: our shared C++ (common/, the harness, the module's tests) and the server."""

import concurrent.futures
import os
import shlex
import subprocess
import sys

from wlblib.common import OUT, PLAY, ROOT, TEST, Failed, run, say

GRAMMAR_PARSE_ERROR = "parse: error parsing grammar"

# Set by `wlb test e2e -j N` for the processes it starts per shard: it built everything first, and
# ninja runs side by side on one build directory would race.
PREBUILT_ENV = "WLB_PREBUILT"


def _prebuilt():
    return os.environ.get(PREBUILT_ENV) == "1"

# The unit test suites; `ninja unit_tests` (common/CMakeLists.txt) builds them all.
UNIT_TESTS = ("wlb_common_unittests", "llm_brain_unittests", "harness_unittests")

# out/<dir>, sanitizer, build type, hardened std lib.
VARIANTS = {
    # RelWithDebInfo, hardened standard library.
    "default": ("common", "", "RelWithDebInfo", True),
    # Linked into AzerothCore, which isn't hardened: hardening must cover a whole program.
    "server": ("common-server", "", "RelWithDebInfo", False),
    "asan": ("common-asan", "address,undefined", "Debug", True),
    "tsan": ("common-tsan", "thread", "Debug", True),
    # Linked against an MSan-instrumented libc++ (`wlb setup msan`): the system libstdc++
    # isn't instrumented.
    "msan": ("common-msan", "memory", "Debug", True),
}


def _exec_unit_test(cmd):
    """Runs one gtest binary, returning `(result, error_message_or_None)` without writing output."""
    cmd = [str(c) for c in cmd]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        return result, f"failed ({result.returncode}): {shlex.join(cmd)}"
    if GRAMMAR_PARSE_ERROR in f"{result.stdout}\n{result.stderr}":
        return result, f"{GRAMMAR_PARSE_ERROR} during passing test: {shlex.join(cmd)}"
    return result, None


def _emit_unit_test(result, out=None, err=None):
    out = sys.stdout if out is None else out
    err = sys.stderr if err is None else err
    if result.stdout:
        out.write(result.stdout)
        out.flush()
    if result.stderr:
        err.write(result.stderr)
        err.flush()


def run_unit_test(cmd):
    """Runs one gtest binary, forwarding its output and failing on grammar parse errors.

    llama.cpp prints `parse: error parsing grammar:` directly to stderr and returns null;
    a test that only checks `!accepted` would pass silently unless we fail on that output.
    """
    result, error = _exec_unit_test(cmd)
    _emit_unit_test(result)
    if error:
        raise Failed(error)
    return result


def run_unit_tests(build_dir, gtest_filter=None, *, emit=True):
    """Runs all `UNIT_TESTS` binaries in `build_dir` concurrently, returning `(outputs, errors)`
    in deterministic `UNIT_TESTS` order. When `emit` is True, flushes stdout/stderr in order and
    raises `Failed` if any suite failed."""
    cmds = [
        [build_dir / test, "--gtest_brief=1",
         *([f"--gtest_filter={gtest_filter}"] if gtest_filter else [])]
        for test in UNIT_TESTS
    ]
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(cmds)) as pool:
        results = list(pool.map(_exec_unit_test, cmds))
    errors = [err for _, err in results if err]
    if emit:
        for result, _ in results:
            _emit_unit_test(result)
        if errors:
            raise Failed("; ".join(errors))
    return results, errors


def common_dir(variant):
    return OUT / VARIANTS[variant][0]


def common(variant="default", tests=True, gtest_filter=None, *, build_tests=False):
    """Builds common/ (and the harness and the module's tests) in out/common[-variant]; with
    `tests`, builds and runs the unit tests too (only those matching `gtest_filter`, if given).
    With `build_tests=True` and `tests=False`, compiles `unit_tests` without running them yet.
    Returns the build directory."""
    from wlblib import setup  # The MSan libc++ is built on demand.

    out_dir, sanitizer, build_type, hardened = VARIANTS[variant]
    build = OUT / out_dir
    if _prebuilt() and not tests and not build_tests:
        return build
    if not (build / "build.ninja").exists():
        flags, ldflags = [], ["-fuse-ld=lld"]
        if sanitizer:
            flags += [f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer",
                      "-fno-sanitize-recover=all"]
            ldflags += [f"-fsanitize={sanitizer}"]
            if sanitizer == "memory":
                libcxx = OUT / "llvm" / "msan"
                if not (libcxx / "lib" / "libc++.so").exists():
                    setup.msan()
                flags += ["-fsanitize-memory-track-origins", "-nostdinc++",
                          "-isystem", f"{libcxx}/include/c++/v1"]
                ldflags += ["-stdlib=libc++", f"-L{libcxx}/lib", f"-Wl,-rpath,{libcxx}/lib",
                            "-lc++abi"]
        run(["cmake", "-S", ROOT / "common", "-B", build, "-G", "Ninja",
             "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
             f"-DCMAKE_BUILD_TYPE={build_type}",
             f"-DCMAKE_C_FLAGS={' '.join(flags)}", f"-DCMAKE_CXX_FLAGS={' '.join(flags)}",
             f"-DWLB_HARDENED={'ON' if hardened else 'OFF'}",
             f"-DCMAKE_EXE_LINKER_FLAGS={' '.join(ldflags)}"], quiet=True)
    targets = ["wlb_base", "wlb_net", "wlb_snapshot",
               *([] if variant == "server"
                 else ["harness", "snapshot_check", "fixture_options",
                       *(["wlb_wire_py"] if variant == "default" else [])])]
    if (tests or build_tests) and variant != "server":
        run(["ninja", "-C", build, *targets, "unit_tests"])
        if tests:
            run_unit_tests(build, gtest_filter=gtest_filter)
    else:
        run(["ninja", "-C", build, *targets])
    return build


def server(test=False, install=True):
    """Builds common/ for the server, then builds and installs AzerothCore (with our module):
    the play server into out/server (production: no test-only code), or with `test` the
    isolated test server into out/server-test (the module's src/test/ compiled in). The first
    test build configures and compiles all of AzerothCore; later ones only what changed.
    `install=False` only compiles, so a running server keeps running meanwhile; installing
    afterwards is quick."""
    from wlblib import setup

    inst = TEST if test else PLAY
    if _prebuilt():
        return
    common("server", tests=False)
    if not (inst.root / "build.ninja").exists():
        say(f"configuring the {inst.name} server build in {inst.root.relative_to(ROOT)}")
        setup.configure(inst.root, inst.test_hooks)
    else:
        # Re-run cmake: it also picks up sources added or moved in the module since last time.
        run(["cmake", inst.root, f"-DLLM_BRAIN_TEST_HOOKS={'ON' if inst.test_hooks else 'OFF'}"],
            quiet=True)
    run(["ninja", "-C", inst.root, *(["install"] if install else [])])
    if install:
        say(f"installed the {inst.name} server")
