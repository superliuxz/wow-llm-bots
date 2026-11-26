"""`wlb build`: our shared C++ (common/, the harness, the module's tests) and the server."""

import os

from wlblib.common import OUT, PLAY, ROOT, TEST, run, say

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


def common_dir(variant):
    return OUT / VARIANTS[variant][0]


def common(variant="default", tests=True, gtest_filter=None):
    """Builds common/ (and the harness and the module's tests) in out/common[-variant]; with
    `tests`, builds and runs the unit tests too (only those matching `gtest_filter`, if given).
    Returns the build directory."""
    from wlblib import setup  # The MSan libc++ is built on demand.

    out_dir, sanitizer, build_type, hardened = VARIANTS[variant]
    build = OUT / out_dir
    if _prebuilt() and not tests:
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
    if tests and variant != "server":
        run(["ninja", "-C", build, *targets, "unit_tests"])
        for test in UNIT_TESTS:
            run([build / test, "--gtest_brief=1",
                 *([f"--gtest_filter={gtest_filter}"] if gtest_filter else [])])
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
