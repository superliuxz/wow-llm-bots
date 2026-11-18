"""`wlb check`: fast static/default checks, plus optional sanitizer passes (CONTRIBUTING.md).

  wlb check              fast loop: format, snapshots, triggers, default build + unit tests,
                         includes, and screen goldens
  wlb check --sanitizers everything above plus ASan+UBSan, TSan, and MSan (--san / --full)
  wlb check sanitizers   build + unit tests under ASan+UBSan, TSan, and MSan only
  wlb check format       clang-format (--fix rewrites)
  wlb check includes     include what you use (--fix rewrites)
  wlb check snapshots    compile the proto, validate example snapshots, check the client-event table
  wlb check triggers     the standing-order grammar over docs/specs/trigger-conditions.tsv
  wlb check rules        RULES.md's reply form matches harness/grammar/plan.wlb (--fix rewrites)
"""

import concurrent.futures
import json
import os
import pathlib
import subprocess
import tempfile

from wlblib import build
from wlblib.common import OUT, ROOT, SERVER, Failed, run, say

# glibc's internal headers and Glaze's umbrella internals carry no IWYU mappings: include the
# public header and mark it `// IWYU pragma: keep`.
IGNORED_HEADERS = "bits/.*,asm-generic/.*,asm/.*,linux/.*,glaze/.*"
HEADER_FLAGS_20 = ["-xc++-header", "-std=c++20", "-Icommon", "-I.",
                   "-Ithird_party/googletest/googletest/include"]
HEADER_FLAGS_26 = ["-xc++-header", "-std=c++26", "-Icommon", "-I.",
                   "-Ithird_party/googletest/googletest/include"]


def _sources(*suffixes):
    files = []
    for top in ("common", "snapshot", "harness", "server_module"):
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
    for f in [*(ROOT / "common").rglob("*.h"), *(ROOT / "snapshot").rglob("*.h")]:
        jobs.append((f, ["--", *HEADER_FLAGS_20]))
    for f in (ROOT / "harness").rglob("*.h"):
        # harness/ for grammar/wlb_runtime.h; the generated grammar headers
        # (grammar/<name>.wlb.h, docs/plans/grammar-idl.md) are in out/common/gen.
        jobs.append((f, ["--", *HEADER_FLAGS_26, "-Iharness/src", "-Iharness",
                         f"-I{OUT / 'common' / 'gen'}"]))
    for f in (ROOT / "server_module").rglob("*.h"):
        jobs.append((f, ["--", *HEADER_FLAGS_20, "-Iserver_module/src"]))
    for f in [*(ROOT / "common").rglob("*.cc"), *(ROOT / "snapshot").rglob("*.cc"),
              *(ROOT / "harness").rglob("*.cc")]:
        jobs.append((f, ["-p", common_db]))
    for f in (ROOT / "server_module").rglob("*.cc"):
        if f.name.endswith("_unittest.cc") or f.parent.name == "tools":
            # Unit tests and tools (server_module/tools/) are built by common/.
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
    """Validates snapshot JSON files against wlb::snapshot::Snapshot (strict JSON keys +
    visibility rules) and checks docs/specs/client-events.tsv against the reflected C++ schema."""
    checker = OUT / "common" / "snapshot_check"
    if not checker.exists():
        build.common("default", tests=False)
    files = list(files) or [
        *sorted((ROOT / "docs/specs/examples").glob("*.json")),
        *sorted((ROOT / "harness/testdata/eval").glob("*.snapshot.json")),
        *sorted((ROOT / "harness/testdata/screens").glob("*.snapshot.json")),
    ]
    run([checker, *files])
    run(["python3", "-I", "-B", ROOT / "tools/audit/check_client_events.py", checker])
    # Eval snapshots show the options the module offers (review 2026-11-17, finding 5).
    run(["python3", "-I", "-B", ROOT / "tools/snapshot/add_fixture_options.py", "--check"])


def triggers():
    """The standing-order audit tables against the real parser (harness/src/orders.cc): every
    `fits` row parses, every row has a known verdict. Also part of the unit tests."""
    build.common("default", tests=False)
    run([OUT / "common" / "harness_unittests", "--gtest_filter=OrdersTest.*Audit*"])


def rules(fix=False):
    """RULES.md's reply form is generated from plan.wlb's line descriptions
    (docs/plans/grammar-idl.md): stale, or anchors not found, fails."""
    run(["python3", "-I", "-B", ROOT / "tools/grammar/wlb_gen_test.py", "-q"])
    command = ["python3", "-I", "-B", ROOT / "tools/grammar/wlb_gen.py", "--rules",
               ROOT / "harness/grammar/plan.wlb", ROOT / "content/RULES.md"]
    run(command + (["--fix"] if fix else []))
    say("rules ok" if not fix else "rules up to date")


def goldens():
    """The screen renderer's goldens come from the Python prototype: it must still write them."""
    run(["python3", "-I", ROOT / "tools/snapshot/make_screen_fixtures.py", "--check"])


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
    say("== RULES.md reply form")
    rules()
    say("== build + unit tests (default)")
    build.common("default")
    # snapshot_check and compile_commands.json are now up to date from the default build.
    say("== snapshots")
    snapshots()
    say("== includes")
    includes()
    say("== screen goldens vs the prototype")
    goldens()
    if with_sanitizers:
        sanitizers()
    say("ALL CHECKS PASSED")
