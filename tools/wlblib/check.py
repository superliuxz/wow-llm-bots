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
import sys
import tempfile

from wlblib import build, wire
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


def _compile_commands_files(build_dir):
    """Set of resolved source paths listed in `build_dir/compile_commands.json` (empty if absent)."""
    try:
        entries = json.loads((build_dir / "compile_commands.json").read_text())
    except (OSError, ValueError):
        return set()
    return {pathlib.Path(e["file"]).resolve() for e in entries if "file" in e}


def _include_job_sort_key(job):
    """Longest-processing-time heuristic: check .cc files (which parse full TUs) before .h files,
    and largest files first, so heavy TUs like snapshot_wire.cc start immediately."""
    f = job[0]
    try:
        size = f.stat().st_size
    except OSError:
        size = 0
    return (0 if f.suffix == ".cc" else 1, -size, str(f))


def _include_jobs():
    """(file, extra clang-include-cleaner args) for every file of ours, and notes on skips."""
    jobs, notes = [], []
    common_db, server_db, server_test_db = OUT / "common", SERVER, OUT / "server-test"
    if not (common_db / "compile_commands.json").exists():
        raise Failed("missing out/common/compile_commands.json: tools/wlb build common")
    common_files = _compile_commands_files(common_db)
    server_test_files = _compile_commands_files(server_test_db)
    server_files = _compile_commands_files(server_db)
    have_server = bool(server_files)
    have_server_test = bool(server_test_files)
    for f in [*(ROOT / "common").rglob("*.h"), *(ROOT / "snapshot").rglob("*.h")]:
        jobs.append((f, ["--", *HEADER_FLAGS_20]))
    for f in (ROOT / "harness").rglob("*.h"):
        # harness/ for grammar/wlb_runtime.h; the generated grammar headers
        # (grammar/<name>.wlb.h, docs/specs/grammar-idl.md) are in out/common/gen.
        jobs.append((f, ["--", *HEADER_FLAGS_26, "-Iharness/src", "-Iharness",
                         f"-I{OUT / 'common' / 'gen'}"]))
    for f in (ROOT / "server_module").rglob("*.h"):
        jobs.append((f, ["--", *HEADER_FLAGS_20, "-Iserver_module/src"]))
    for f in [*(ROOT / "common").rglob("*.cc"), *(ROOT / "snapshot").rglob("*.cc"),
              *(ROOT / "harness").rglob("*.cc")]:
        jobs.append((f, ["-p", common_db]))
    for f in (ROOT / "server_module").rglob("*.cc"):
        resolved = f.resolve()
        if (f.name.endswith("_unittest.cc") or f.parent.name == "tools" or
                resolved in common_files):
            # Unit tests, tools, and AzerothCore-independent module files built by common/.
            jobs.append((f, ["-p", common_db]))
        elif have_server_test and resolved in server_test_files:
            jobs.append((f, ["-p", server_test_db]))
        elif have_server and resolved in server_files:
            jobs.append((f, ["-p", server_db]))
        elif not have_server and not have_server_test:
            notes.append(f"{f.relative_to(ROOT)} not checked (no server build)")
        else:
            notes.append(f"{f.relative_to(ROOT)} not checked (not in server compile_commands)")
    jobs.sort(key=_include_job_sort_key)
    return jobs, notes


def _run_includes(fix=False):
    """Runs the banned-header scan and clang-include-cleaner, returning `(lines, error)` without
    printing so callers can run it alongside other checks without interleaved output."""
    lines = []
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
        return banned, "includes: banned header included directly (see CONTRIBUTING.md)"
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
        lines.append(f"note: {note}")
    if fix:
        format_(fix=True)
    if problems and not fix:
        lines.extend(problems)
        return lines, f"includes: {len(problems)} files need changes (tools/wlb check includes --fix)"
    lines.append("includes ok")
    return lines, None


def includes(fix=False):
    """Every file includes exactly what it uses (clang-include-cleaner), and none includes absl
    or <pthread.h> outside common/base/ (CONTRIBUTING.md). A file the tool can't parse is a
    failure, not a pass."""
    lines, error = _run_includes(fix=fix)
    if lines:
        say("\n".join(lines))
    if error:
        raise Failed(error)


def snapshots(files=()):
    """Validates snapshot JSON files against wlb::snapshot::Snapshot (strict JSON keys +
    visibility rules) and verifies hover-field annotations and fixture options."""
    checker = OUT / "common" / "snapshot_check"
    if not checker.exists():
        build.common("default", tests=False)
    files = list(files) or [
        *sorted((ROOT / "harness/testdata/eval").glob("*.snapshot.json")),
        *sorted((ROOT / "harness/testdata/screens").glob("*.snapshot.json")),
    ]
    run([checker, *files])
    if wire.check_hover_fields() != 0:
        raise Failed("hover fields check failed")
    # Eval snapshots show the options the module offers (review 2026-11-17, finding 5).
    run(["python3", "-I", "-B", ROOT / "tools/snapshot/add_fixture_options.py", "--check"])


def triggers():
    """The standing-order audit tables against the real parser (harness/src/plan/orders.cc): every
    `fits` row parses, every row has a known verdict. Also part of the unit tests."""
    build.common("default", tests=False)
    run(["ninja", "-C", OUT / "common", "harness_unittests"])
    build.run_unit_test([OUT / "common" / "harness_unittests", "--gtest_filter=OrdersTest.*Audit*"])


def rules(fix=False):
    """RULES.md's reply form is generated from plan.wlb's line descriptions
    (docs/specs/grammar-idl.md): stale, or anchors not found, fails."""
    run(["python3", "-I", "-B", ROOT / "tools/grammar/wlb_gen_test.py", "-q"])
    command = ["python3", "-I", "-B", ROOT / "tools/grammar/wlb_gen.py", "--rules",
               ROOT / "harness/grammar/plan.wlb", ROOT / "content/RULES.md"]
    run(command + (["--fix"] if fix else []))
    say("rules ok" if not fix else "rules up to date")


def goldens():
    """The screen renderer's output matches the reviewed goldens.

    Regression goldens only: they were last written by C++ itself (`render-screen --rewrite`) after
    the Python prototype was retired, so a pass shows nothing changed, not that the text is right.
    Read every golden diff by hand before committing a rewrite.
    """
    harness_bin = OUT / "common" / "harness"
    if not harness_bin.exists():
        build.common("default", tests=False)
    run([harness_bin, "render-screen", ROOT / "harness/testdata/screens"])


BENCH_TEST_SCRIPTS = (
    "tools/bench/soak_suites_test.py",
    "tools/bench/dodge_timeline_test.py",
    "tools/bench/stall_check_test.py",
    "tools/bench/chatter_check_test.py",
    "tools/notes/extract_boss_notes_test.py",
    "tools/knowledge/export_knowledge_test.py",
    "tools/knowledge/backfill_quest_poi_test.py",
    "tools/eval/mine_traces_test.py",
    "tools/wlblib/remote_test.py",
    "tools/wlblib/eval_plans_test.py",
    "tools/wlblib/testing_test.py",
)


def _run_bench_tests(scripts=BENCH_TEST_SCRIPTS):
    """Runs Python unit-test scripts concurrently, returning `(results, errors)` in input order."""
    def one(rel):
        cmd = ["python3", "-I", "-B", str(ROOT / rel), "-q"]
        result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        err = None if result.returncode == 0 else f"failed ({result.returncode}): {rel}"
        return rel, result, err

    workers = max(1, min(len(scripts), os.cpu_count() or 1))
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(one, scripts))
    errors = [err for _, _, err in results if err]
    return results, errors


def _emit_bench_results(results):
    for _, result, _ in results:
        if result.stdout:
            sys.stdout.write(result.stdout)
            sys.stdout.flush()
        if result.stderr:
            sys.stderr.write(result.stderr)
            sys.stderr.flush()


def bench_tests():
    """Unit tests for the soak checks, trace analyzers, generators, and remote worker."""
    results, errors = _run_bench_tests()
    _emit_bench_results(results)
    if errors:
        raise Failed("; ".join(errors))


def sanitizers():
    """Build and run unit tests under ASan+UBSan, TSan, and MSan."""
    for variant in ("asan", "tsan", "msan"):
        say(f"== build + unit tests ({variant})")
        build.common(variant)
    say("SANITIZERS PASSED")


def all_(with_sanitizers=False):
    """Fast checks in the order that fails fastest; independent post-build checks run
    concurrently with buffered, deterministic output. Optionally followed by *SAN."""
    say("== format")
    format_()
    say("== RULES.md reply form")
    rules()
    say("== build (default)")
    build_dir = build.common("default", tests=False, build_tests=True)
    # snapshot_check, harness, and compile_commands.json are now up to date from the default build.
    say("== snapshots")
    snapshots()
    say("== screen goldens")
    goldens()
    # Run the three heavier independent post-build checks concurrently, then flush in order.
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        unit_fut = pool.submit(build.run_unit_tests, build_dir, None, emit=False)
        inc_fut = pool.submit(_run_includes, False)
        bench_fut = pool.submit(_run_bench_tests)
        unit_results, unit_errors = unit_fut.result()
        inc_lines, inc_error = inc_fut.result()
        bench_results, bench_errors = bench_fut.result()
    failures = []
    say("== unit tests (default)")
    for result, _ in unit_results:
        build._emit_unit_test(result)
    failures.extend(unit_errors)
    say("== includes")
    if inc_lines:
        say("\n".join(inc_lines))
    if inc_error:
        failures.append(inc_error)
    say("== bench & loop unit tests")
    _emit_bench_results(bench_results)
    failures.extend(bench_errors)
    if failures:
        raise Failed("; ".join(failures))
    if with_sanitizers:
        sanitizers()
    say("ALL CHECKS PASSED")
