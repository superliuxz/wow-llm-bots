"""`wlb check`: fast static/default checks, plus optional sanitizer passes (CONTRIBUTING.md).

  wlb check              fast loop: format, snapshots, triggers, default build + unit tests,
                         includes, and screen goldens
  wlb check --sanitizers everything above plus ASan+UBSan, TSan, and MSan (--san / --full)
  wlb check sanitizers   build + unit tests under ASan+UBSan, TSan, and MSan only
  wlb check format       clang-format (--fix rewrites)
  wlb check includes     include what you use (--fix rewrites)
  wlb check snapshots    compile the proto, validate example snapshots, check the client-event table
  wlb check triggers     the standing-order grammar over docs/specs/trigger-conditions.tsv
"""

import concurrent.futures
import json
import os
import pathlib
import subprocess
import tempfile

from wlblib import build
from wlblib.common import OUT, PROTOC, ROOT, SERVER, VENV_PYTHON, Failed, output, run, say

# glibc's internal headers carry no IWYU mappings: include the public header (<pthread.h>) and
# mark it `// IWYU pragma: keep`.
IGNORED_HEADERS = "bits/.*,asm-generic/.*,asm/.*,linux/.*"
HEADER_FLAGS_20 = ["-xc++-header", "-std=c++20", "-Icommon",
                   "-Ithird_party/googletest/googletest/include",
                   "-Ithird_party/protobuf/src", "-Ithird_party/abseil-cpp"]
HEADER_FLAGS_26 = ["-xc++-header", "-std=c++26", "-Icommon",
                   "-Ithird_party/googletest/googletest/include",
                   "-Ithird_party/protobuf/src", "-Ithird_party/abseil-cpp"]


def _sources(*suffixes):
    files = []
    for top in ("common", "harness", "server_module"):
        for suffix in suffixes:
            files += sorted((ROOT / top).rglob(f"*{suffix}"))
    return files


def format_(fix=False):
    files = _sources(".h", ".cc")
    mode = ["-i"] if fix else ["--dry-run", "--Werror"]
    run(["clang-format", *mode, *files], cwd=ROOT)
    say("format ok" if not fix else "formatted")


def _in_compile_commands(build_dir, path):
    try:
        entries = json.loads((build_dir / "compile_commands.json").read_text())
    except OSError:
        return False
    return any(pathlib.Path(e["file"]).resolve() == path.resolve() for e in entries)


def _include_jobs():
    """(file, extra clang-include-cleaner args) for every file of ours, and notes on skips."""
    jobs, notes = [], []
    common_db, server_db = OUT / "common", SERVER
    if not (common_db / "compile_commands.json").exists():
        raise Failed("missing out/common/compile_commands.json: tools/wlb build common")
    have_server = (server_db / "compile_commands.json").exists()
    for f in (ROOT / "common").rglob("*.h"):
        jobs.append((f, ["--", *HEADER_FLAGS_20]))
    for f in (ROOT / "harness").rglob("*.h"):
        jobs.append((f, ["--", *HEADER_FLAGS_26, "-Iharness/src", "-Iout/common/gen"]))
    for f in (ROOT / "server_module").rglob("*.h"):
        jobs.append((f, ["--", *HEADER_FLAGS_20, "-Iserver_module/src", "-Iout/common/gen"]))
    for f in [*(ROOT / "common").rglob("*.cc"), *(ROOT / "harness").rglob("*.cc")]:
        jobs.append((f, ["-p", common_db]))
    for f in (ROOT / "server_module").rglob("*.cc"):
        if f.name.endswith("_unittest.cc"):
            jobs.append((f, ["-p", common_db]))
        elif not have_server:
            notes.append(f"{f.relative_to(ROOT)} not checked (no server build)")
        elif "/src/test/" in str(f) and not _in_compile_commands(server_db, f):
            # Test-only module code is only in a test build's compile commands.
            notes.append(f"{f.relative_to(ROOT)} not checked (out/server is a production build)")
        else:
            jobs.append((f, ["-p", server_db]))
    return jobs, notes


def includes(fix=False):
    """Every file includes exactly what it uses (clang-include-cleaner), and none includes absl
    or <pthread.h> outside common/base/ (CONTRIBUTING.md). A file the tool can't parse is a
    failure, not a pass."""
    banned = []
    for f in _sources(".h", ".cc"):
        rel = str(f.relative_to(ROOT))
        in_base = rel.startswith("common/base/")
        for lineno, line in enumerate(f.read_text().splitlines(), 1):
            stripped = line.lstrip()
            if not stripped.startswith("#include"):
                continue
            if "\"absl/" in stripped or "<absl/" in stripped:
                banned.append(f"{rel}:{lineno}: {stripped} (use common/base/strings/)")
            elif not in_base and ("<pthread.h>" in stripped or "\"pthread.h\"" in stripped):
                banned.append(f"{rel}:{lineno}: {stripped} (use base::Thread / base::Lock)")
            elif "<set>" in stripped or "<map>" in stripped:
                banned.append(f"{rel}:{lineno}: {stripped} (use std::vector or std::flat_map/flat_set)")
    if banned:
        say("\n".join(banned))
        raise Failed("includes: banned header included directly (see CONTRIBUTING.md)")
    jobs, notes = _include_jobs()
    mode = "--edit" if fix else "--print=changes"

    def check(job):
        f, args = job
        result = subprocess.run(
            ["clang-include-cleaner", mode, f"--ignore-headers={IGNORED_HEADERS}", f,
             *map(str, args)], cwd=ROOT, capture_output=True, text=True)
        rel = f.relative_to(ROOT)
        if result.returncode != 0:
            errors = [l for l in result.stderr.splitlines() if "error" in l][:5]
            return f"== {rel}: clang-include-cleaner failed\n" + "\n".join(errors)
        if result.stdout.strip() and not fix:
            return f"== {rel}\n{result.stdout.strip()}"
        return None

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        problems = [p for p in pool.map(check, jobs) if p]
    for note in notes:
        say(f"note: {note}")
    if fix:
        format_(fix=True)
    if problems and not fix:
        say("\n".join(problems))
        raise Failed(f"includes: {len(problems)} files need changes (tools/wlb check includes --fix)")
    say("includes ok")


def snapshots(files=()):
    """Compiles the snapshot proto for Python (out/proto), validates the example snapshots (or
    `files`), and checks the client-event table against the proto. Creates the tools venv with
    the protobuf runtime matching protoc."""
    if not PROTOC.exists():
        build.protoc()
    # The Python runtime must match protoc's version (protoc 36.2 <-> protobuf 7.36.2).
    want = "7." + output([PROTOC, "--version"]).split()[1]
    if not VENV_PYTHON.exists():
        run(["python3", "-m", "venv", VENV_PYTHON.parent.parent])
    have = subprocess.run([VENV_PYTHON, "-I", "-c",
                           "import google.protobuf as p; print(p.__version__)"],
                          capture_output=True, text=True).stdout.strip()
    if have != want:
        run([VENV_PYTHON.parent / "pip", "install", "-q", f"protobuf=={want}"])
    gen = OUT / "proto"
    gen.mkdir(parents=True, exist_ok=True)
    proto = ROOT / "server_module" / "proto"
    run([PROTOC, f"-I{ROOT / 'server_module'}", f"-I{ROOT / 'third_party/protobuf/src'}",
         f"--python_out={gen}", proto / "snapshot.proto", proto / "snapshot_options.proto"])
    files = list(files) or [
        *sorted((ROOT / "docs/specs/examples").glob("*.json")),
        *sorted((ROOT / "harness/testdata/eval").glob("*.snapshot.json")),
    ]
    run([VENV_PYTHON, "-I", ROOT / "tools/snapshot/validate_snapshot.py", gen, *files])
    run([VENV_PYTHON, "-I", "-B", ROOT / "tools/audit/check_client_events.py", gen])


def triggers():
    run(["python3", "-I", ROOT / "tools/check-trigger-conditions.py"])


def goldens():
    """The screen renderer's goldens come from the Python prototype: it must still write them."""
    run([VENV_PYTHON, "-I", ROOT / "tools/snapshot/make_screen_fixtures.py", "--check"])


def sanitizers():
    """Build and run unit tests under ASan+UBSan, TSan, and MSan."""
    for variant in ("asan", "tsan", "msan"):
        say(f"== build + unit tests ({variant})")
        build.common(variant)
    say("SANITIZERS PASSED")


def all_(with_sanitizers=False):
    """Fast checks in the order that fails fastest; optionally followed by *SAN."""
    say("== format")
    format_()
    say("== snapshots")
    snapshots()  # Also regenerates out/proto, which the goldens check reads.
    say("== trigger table")
    triggers()
    say("== build + unit tests (default)")
    build.common("default")
    # The include check reads the default build's compile commands.
    say("== includes")
    includes()
    say("== screen goldens vs the prototype")
    goldens()
    if with_sanitizers:
        sanitizers()
    say("ALL CHECKS PASSED")
