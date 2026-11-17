"""`wlb setup`: one-time setup steps (each can be re-run).

  links       link our module and playerbots into AzerothCore's modules/ (it only builds modules
              found there), and compile_commands.json for clangd
  configure   configure the AzerothCore build in out/server (clang + lld + Ninja, Percona client)
  data        extract DBC, maps, vmaps and mmaps from the 3.3.5a client into out/server/data
  llama       build llama.cpp (CUDA, RTX 5090 = sm_120) into out/llama
  msan        build an MSan-instrumented libc++ into out/llvm/msan (for `wlb build common msan`)
  client-ui   extract the client's default UI code and event names into out/client-ui, for the
              client-event audit (docs/specs/coverage-client-events.md)

The 3.3.5a client is $WOW_CLIENT (default: WoW-3.3.5a next to the repo).
"""

import collections
import datetime
import os
import pathlib
import re
import shutil
import subprocess

from wlblib.common import OUT, ROOT, SERVER, Failed, output, run, say

AC = ROOT / "third_party" / "azerothcore-wotlk"


def client_dir():
    return pathlib.Path(os.environ.get("WOW_CLIENT", ROOT.parent / "WoW-3.3.5a"))


def links():
    def link(target, name):
        path = AC / "modules" / name
        if path.is_symlink() or path.exists():
            path.unlink()
        path.symlink_to(target)
        # Keep the link out of the submodule's git status.
        exclude = pathlib.Path(output(["git", "-C", AC, "rev-parse", "--git-path",
                                       "info/exclude"]).strip())
        exclude = (AC / exclude).resolve() if not exclude.is_absolute() else exclude
        exclude.parent.mkdir(parents=True, exist_ok=True)
        lines = exclude.read_text().splitlines() if exclude.exists() else []
        if f"/modules/{name}" not in lines:
            with open(exclude, "a") as f:
                f.write(f"/modules/{name}\n")

    link(ROOT / "third_party" / "mod-playerbots", "mod-playerbots")
    link(ROOT / "server_module", "mod-llm-brain")  # The name AzerothCore expects.
    db = ROOT / "compile_commands.json"  # For clangd (gitignored).
    if db.is_symlink() or db.exists():
        db.unlink()
    db.symlink_to(SERVER / "compile_commands.json")
    say("setup done")


def configure():
    run(["cmake", "-S", AC, "-B", SERVER, "-G", "Ninja",
         "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
         "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld", "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld",
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DCMAKE_INSTALL_PREFIX={SERVER}",
         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
         "-DAPPS_BUILD=all", "-DTOOLS_BUILD=all", "-DMODULES=static", "-DSCRIPTS=static",
         "-DMYSQL_CONFIG=/usr/bin/perconaserver_config",
         "-DMYSQL_INCLUDE_DIR=/usr/include/perconaserver",
         "-DMYSQL_LIBRARY=/usr/lib/libperconaserverclient.so",
         "-DMYSQL_ADD_INCLUDE_PATH=/usr/include/perconaserver"])


def data():
    client, bin_dir, out = client_dir(), SERVER / "bin", SERVER / "data"
    out.mkdir(parents=True, exist_ok=True)
    steps = [
        ("dbc + maps", [bin_dir / "map_extractor", "-i", client, "-o", out]),
        ("vmap extract", [bin_dir / "vmap4_extractor", "-d", client / "Data"]),
        ("vmap assemble", [bin_dir / "vmap4_assembler", "Buildings", "vmaps"]),
        ("mmaps (slow)", [bin_dir / "mmaps_generator", "--config",
                          bin_dir / "mmaps-config.yaml", "--threads", str(os.cpu_count())]),
    ]
    (out / "vmaps").mkdir(exist_ok=True)
    (out / "mmaps").mkdir(exist_ok=True)
    for i, (name, cmd) in enumerate(steps, 1):
        say(f"== [{i}/{len(steps)}] {name}  {datetime.datetime.now():%H:%M:%S}")
        run(cmd, cwd=out)
    say(f"== done  {datetime.datetime.now():%H:%M:%S}")
    run(["du", "-sh", "dbc", "maps", "vmaps", "mmaps"], cwd=out)


def llama():
    # CUDA 13.4 officially supports clang <= 22; clang 23 works with -allow-unsupported-compiler.
    build = OUT / "llama"
    run(["cmake", "-S", ROOT / "third_party" / "llama.cpp", "-B", build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
         "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld", "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld",
         "-DGGML_CUDA=ON", "-DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc",
         "-DCMAKE_CUDA_HOST_COMPILER=clang++", "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler",
         "-DCMAKE_CUDA_ARCHITECTURES=120", "-DBUILD_SHARED_LIBS=OFF",
         "-DLLAMA_BUILD_TESTS=OFF", "-DLLAMA_BUILD_EXAMPLES=OFF", "-DLLAMA_CURL=OFF"])
    run(["ninja", "-C", build, "llama-server", "llama-bench", "llama-cli"])


def msan():
    """libc++/libc++abi instrumented with MemorySanitizer, matching the installed clang (MSan
    needs every byte of C++ code instrumented, the standard library included)."""
    version = re.search(r"clang version ([0-9.]+)", output(["clang", "--version"]))
    if not version:
        raise Failed("can't tell the clang version")
    src = OUT / "llvm" / "src"
    if not (src / "libcxx").is_dir():
        run(["git", "clone", "-q", "--depth", "1", "--branch", f"llvmorg-{version.group(1)}",
             "--filter=blob:none", "--sparse", "https://github.com/llvm/llvm-project.git", src])
        run(["git", "-C", src, "sparse-checkout", "set", "runtimes", "libcxx", "libcxxabi",
             "libunwind", "libc", "cmake", "llvm/cmake", "llvm/utils/llvm-lit", "third-party"])
    build = OUT / "llvm" / "msan-build"
    run(["cmake", "-S", src / "runtimes", "-B", build, "-G", "Ninja",
         "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_BUILD_TYPE=Release",
         "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi", "-DLLVM_USE_SANITIZER=MemoryWithOrigins",
         "-DLIBCXX_INCLUDE_BENCHMARKS=OFF", "-DLIBCXX_INCLUDE_TESTS=OFF",
         "-DLIBCXXABI_INCLUDE_TESTS=OFF", "-DLIBCXXABI_USE_LLVM_UNWINDER=OFF",
         f"-DCMAKE_INSTALL_PREFIX={OUT / 'llvm' / 'msan'}"])
    run(["ninja", "-C", build, "install-cxx", "install-cxxabi"])


# Event names in the UI code: registered, compared, or chat types ("CHAT_MSG_" .. type).
UI_EVENT = re.compile(r'RegisterEvent\("([A-Z_0-9]+)"\)|event *== *"([A-Z_0-9]+)"|'
                      r'"(CHAT_MSG_[A-Z_0-9]+)"')
EXE_NAME = re.compile(rb"[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+")
# A table of event names in Wow.exe: runs of names the UI is known to use, with gaps of at most
# 12 strings between them, at least 8 known names long.
TABLE_GAP, TABLE_MIN = 12, 8


def _exe_strings(path, min_len=4):
    """Printable ASCII runs of at least `min_len` in a binary, in order (like `strings`)."""
    return [m.group() for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, path.read_bytes())]


def client_ui():
    """The client's default UI code (FrameXML, GlueXML, Blizzard_* addons) and the event names
    it uses or that Wow.exe's event tables hold. Blizzard's code: untracked, in out/client-ui."""
    client, out = client_dir(), OUT / "client-ui"
    libmpq = AC / "deps" / "libmpq"
    out.mkdir(parents=True, exist_ok=True)
    run(["clang", "-O1", "-Wall", "-Wno-pointer-sign", f"-I{libmpq / 'libmpq'}", f"-I{libmpq}",
         f"-I{SERVER / 'deps' / 'libmpq'}", ROOT / "tools/audit/mpq_extract.c",
         SERVER / "deps/libmpq/libmpq.a", "-lz", "-lbz2", "-o", out / "mpq_extract"])
    shutil.rmtree(out / "Interface", ignore_errors=True)
    d, l = client / "Data", client / "Data" / "enUS"
    archives = [l / "patch-enUS-3.MPQ", l / "patch-enUS-2.MPQ", l / "patch-enUS.MPQ",
                d / "patch-3.MPQ", d / "patch-2.MPQ", d / "patch.MPQ",
                l / "lichking-locale-enUS.MPQ", l / "expansion-locale-enUS.MPQ",
                l / "locale-enUS.MPQ", d / "lichking.MPQ", d / "expansion.MPQ",
                d / "common-2.MPQ", d / "common.MPQ"]
    say(output([out / "mpq_extract", out, *archives]).splitlines()[-1])

    ui_events = set()
    for f in (out / "Interface").rglob("*"):
        if f.is_file():
            for m in UI_EVENT.finditer(f.read_text(errors="replace")):
                ui_events.add(next(g for g in m.groups() if g))
    (out / "events_ui.txt").write_text("".join(f"{e}\n" for e in sorted(ui_events)))

    # Wow.exe's event names, in file order; keep the dense runs of known ones (the tables).
    names = [s.decode() for s in _exe_strings(client / "Wow.exe") if EXE_NAME.fullmatch(s)]
    exe_events, run_start, last, count = set(), None, None, 0
    for i, name in enumerate(names):
        if name not in ui_events:
            continue
        if last is not None and i - last > TABLE_GAP:
            if count >= TABLE_MIN:
                exe_events.update(names[run_start:last + 1])
            run_start, count = None, 0
        if run_start is None:
            run_start = i
        last, count = i, count + 1
    if count >= TABLE_MIN:
        exe_events.update(names[run_start:last + 1])
    (out / "events_exe.txt").write_text("".join(f"{e}\n" for e in sorted(exe_events)))
    everything = ui_events | exe_events
    (out / "events_all.txt").write_text("".join(f"{e}\n" for e in sorted(everything)))
    say(f"events: {len(ui_events)} used by the UI, {len(exe_events)} in Wow.exe's tables, "
        f"{len(everything)} together")
