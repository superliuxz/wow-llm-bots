"""`wlb build`: protoc, our shared C++ (common/, the harness, the module's tests) and the server."""

import re

from wlblib.common import OUT, PROTOC, ROOT, SERVER, run, say

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


def protoc():
    """protoc from the pinned third_party/protobuf and abseil, with clang, into out/protoc."""
    run(["cmake", "-S", ROOT / "tools" / "protobuf", "-B", OUT / "protoc", "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang",
         "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"])
    run(["ninja", "-C", OUT / "protoc", "protoc"])
    run([PROTOC, "--version"])


def common_dir(variant):
    return OUT / VARIANTS[variant][0]


def common(variant="default", tests=True):
    """Builds common/ (and the harness and the module's tests) in out/common[-variant]; with
    `tests`, builds and runs the unit tests too. Returns the build directory."""
    from wlblib import setup  # The MSan libc++ is built on demand.

    out_dir, sanitizer, build_type, hardened = VARIANTS[variant]
    build = OUT / out_dir
    if not PROTOC.exists():  # Generated proto code needs the pinned protoc.
        protoc()
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
    targets = ["wlb_base", "wlb_net", "wlb_proto"]
    if tests and variant != "server":
        run(["ninja", "-C", build, *targets, *UNIT_TESTS])
        for test in UNIT_TESTS:
            run([build / test, "--gtest_brief=1"])
    else:
        run(["ninja", "-C", build, *targets])
    return build


def server(test=False):
    """Builds common/ for the server, then builds and installs AzerothCore (with our module)
    into out/server. `test` compiles in the module's test-only code (src/test/: the fixture
    reset); a production build contains none of it. Switching only rebuilds the module."""
    common("server", tests=False)
    hooks = "ON" if test else "OFF"
    # Re-run cmake: it also picks up sources added or moved in the module since last time.
    run(["cmake", SERVER, f"-DLLM_BRAIN_TEST_HOOKS={hooks}"], quiet=True)
    run(["ninja", "-C", SERVER, "install"])
    say(f"installed a {'test' if test else 'production'} build")


def built_flavor():
    """'test' or 'production': what out/server was last configured as, or None."""
    try:
        cache = (SERVER / "CMakeCache.txt").read_text()
    except OSError:
        return None
    match = re.search(r"^LLM_BRAIN_TEST_HOOKS:BOOL=(\w+)", cache, re.M)
    return "test" if match and match.group(1) == "ON" else "production"
