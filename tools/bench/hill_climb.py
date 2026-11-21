#!/usr/bin/env python3
"""Self-improving hill-climbing loop controller (docs/plans/self-improving-loop.md).

Commands:
  lock-scorer      Write read-only (0444) SHA-256 lock manifest covering all
                   layered scorer modules, trace writer/scripts, soak runner,
                   and soak suite configs.
  calibrate        Run Layer D scorer calibration meta-checks (humans > current
                   > known-bad bots, repeatability, adversary detection).
  init             Verify (or create if absent) lock manifest, record baseline
                   traces & Layer A/B/C baseline in ledger.jsonl.
  eval-candidate   Verify exact lock manifest, enforce edit scope, require
                   verified human traces, run `wlb check` + soak, enforce Layer A
                   gates (including XP/h floor, deaths ceiling, human timing
                   limits, refusal-loop check), require candidate Layer B
                   Bradley-Terry rating to beat current outside bootstrapped
                   95% CI + confirmation run + `--suite heldout` on KEEP.
  status           Print experiment ledger summary + current Layer C tells.
"""

from __future__ import annotations

import argparse
import datetime
import fnmatch
import hashlib
import hmac
import json
import os
import pathlib
import re
import shutil
import stat
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
VAR = ROOT / "var"
LEDGER = VAR / "experiments" / "ledger.jsonl"
BASELINE_TRACES_ROOT = VAR / "experiments" / "baseline_traces"
EXTERNAL_STATE_DIR = pathlib.Path.home() / ".local" / "share" / "wow-llm-bots"
DEFAULT_LOCK_FILE = EXTERNAL_STATE_DIR / "scorer.sha256"
DEFAULT_PASSPHRASE_FILE = EXTERNAL_STATE_DIR / "relock.passphrase"
REPO_APPROVED_LOCK_DIGEST = ROOT / "config" / "scorer-lock.sha256"
DEFAULT_HUMAN_DIR = EXTERNAL_STATE_DIR / "human_baseline"
DEFAULT_BLIND_RATINGS = EXTERNAL_STATE_DIR / "blind_ratings.jsonl"

# Every file that produces, configures, or gates the soak score is locked.
LOCKED_PATTERNS = [
    "tools/bench/soak_scorer.py",
    "tools/bench/trace_parity.py",
    "tools/bench/pairwise_ranker.py",
    "tools/bench/adversary_suite.py",
    "tools/bench/population_metrics.py",
    "tools/bench/known_bad_bots.py",
    "tools/bench/scorer_calibration.py",
    "tools/bench/blind_rate.py",
    "tools/bench/hill_climb.py",
    "tools/wlblib/cases/soak.py",
    "tools/wlblib/testing.py",
    "server_module/src/snapshot/behavior_trace.h",
    "server_module/src/snapshot/behavior_trace.cc",
    "server_module/src/snapshot/behavior_trace_scripts.h",
    "server_module/src/snapshot/behavior_trace_scripts.cc",
    "server_module/src/snapshot/snapshot_builder.cc",
    "server_module/src/snapshot/snapshot_schedule.h",
    "server_module/src/snapshot/snapshot_schedule.cc",
    "server_module/src/llm_brain_world_script.cc",
    "server_module/conf/mod-llm-brain.conf.dist",
    "server_module/src/config/llm_brain_config.cc",
    "config/llm-brain.overrides",
    "config/playerbots.overrides",
    "config/tests/soak*.llm-brain.overrides",
]

# Allowed file globs for scored humanization rounds (bug fixes & C++ game
# judgment belong in separate proposals, not mixed into scored rounds).
ALLOWED_EDIT_GLOBS = [
    "server_module/src/options/human_pacing*",
    "content/RULES.md",
    "content/identities/*",
]

DEFAULT_MIN_XP_PER_HOUR = 3000.0
DEFAULT_MAX_DEATHS_PER_RUN = 2

sys.path.insert(0, str(ROOT / "tools" / "bench"))
sys.path.insert(0, str(ROOT / "tools"))
import scorer_calibration  # noqa: E402
import soak_scorer  # noqa: E402
import trace_parity  # noqa: E402
from wlblib.cases import soak as soak_case  # noqa: E402


def _git(*args: str) -> str:
    res = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    return res.stdout.strip()


def _sha256_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _locked_files() -> list[pathlib.Path]:
    seen: set[pathlib.Path] = set()
    files: list[pathlib.Path] = []
    for pat in LOCKED_PATTERNS:
        matched = sorted(ROOT.glob(pat)) if "*" in pat else [ROOT / pat]
        for p in matched:
            if p.exists() and p not in seen:
                seen.add(p)
                files.append(p)
    return files


def _confirm_relock(
    passphrase_file: pathlib.Path = DEFAULT_PASSPHRASE_FILE,
    tty_path: str | pathlib.Path = "/dev/tty",
) -> tuple[bool, str]:
    # Requires typing the passphrase stored in `passphrase_file` (outside the
    # repo) into `/dev/tty`. `passphrase_file` may hold either the plaintext
    # passphrase or its 64-hex SHA-256 digest. Tracked
    # `config/scorer-lock.sha256` remains the git-visible backstop.
    try:
        expected = passphrase_file.read_text(encoding="utf-8").strip()
    except OSError:
        expected = ""
    if not expected:
        return False, (
            f"refusing to write scorer lock: missing or empty passphrase file "
            f"{passphrase_file} (create it outside the repo first)"
        )
    # Open separate "r" and "w" handles: character devices such as /dev/tty
    # are non-seekable, so open("/dev/tty", "r+") raises io.UnsupportedOperation
    # (a subclass of OSError).
    try:
        with open(tty_path, "r", encoding="utf-8") as tty_in:
            if not tty_in.isatty():
                return False, (
                    "refusing to write scorer lock without an interactive "
                    "controlling terminal (/dev/tty)"
                )
            with open(tty_path, "w", encoding="utf-8") as tty_out:
                if not tty_out.isatty():
                    return False, (
                        "refusing to write scorer lock without an interactive "
                        "controlling terminal (/dev/tty)"
                    )
                tty_out.write("Confirm writing scorer lock (enter passphrase): ")
                tty_out.flush()
                ans = tty_in.readline().strip()
    except OSError:
        return False, (
            "refusing to write scorer lock without a controlling terminal "
            "(/dev/tty; run interactively and enter your passphrase)"
        )
    if not ans:
        return False, "refusing to write scorer lock: empty passphrase entered"
    ans_sha256 = hashlib.sha256(ans.encode("utf-8")).hexdigest()
    if hmac.compare_digest(ans, expected) or (
        bool(re.fullmatch(r"[0-9a-fA-F]{64}", expected))
        and hmac.compare_digest(ans_sha256, expected.lower())
    ):
        return True, ""
    return False, "refusing to write scorer lock: passphrase mismatch"


def _resolve_approved_digest_file(
    lock_file: pathlib.Path, approved_digest_file: pathlib.Path | None
) -> pathlib.Path | None:
    if approved_digest_file is not None:
        return approved_digest_file
    if lock_file.resolve() == DEFAULT_LOCK_FILE.resolve():
        return REPO_APPROVED_LOCK_DIGEST
    return None


def _read_approved_digest(digest_file: pathlib.Path) -> str | None:
    for line in digest_file.read_text().splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        token = s.split()[0].lower()
        return token if re.fullmatch(r"[0-9a-f]{64}", token) else None
    return None


def _write_lock_manifest(
    lock_file: pathlib.Path,
    force: bool = False,
    confirmed: bool = False,
    approved_digest_file: pathlib.Path | None = None,
) -> tuple[bool, str]:
    if lock_file.exists() and not force:
        return False, (
            f"lock file already exists: {lock_file} "
            "(pass --force-relock explicitly to overwrite)"
        )
    if not confirmed:
        return False, f"refusing to write lock manifest {lock_file} without confirmation"
    digest = soak_scorer.scorer_sha256()
    lock_file.parent.mkdir(parents=True, exist_ok=True)
    if lock_file.exists():
        os.chmod(lock_file, stat.S_IRUSR | stat.S_IWUSR)
    lines = [f"{digest}  tools/bench/soak_scorer.py"]
    for p in _locked_files():
        rel = p.relative_to(ROOT).as_posix()
        lines.append(f"{_sha256_file(p)}  {rel}")
    lock_file.write_text("\n".join(lines) + "\n")
    os.chmod(lock_file, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)  # 0444 read-only
    digest_target = _resolve_approved_digest_file(lock_file, approved_digest_file)
    if digest_target is not None:
        digest_target.parent.mkdir(parents=True, exist_ok=True)
        digest_target.write_text(
            "# User-approved SHA-256 digest of ~/.local/share/wow-llm-bots/scorer.sha256.\n"
            "# Updated by `hill_climb.py lock-scorer` so any relock is visible in git diff.\n"
            f"{_sha256_file(lock_file)}  scorer.sha256\n"
        )
    return True, f"locked {len(lines) - 1} measurement files (0444) -> {lock_file}"


def _verify_all_locks(
    lock_file: pathlib.Path = DEFAULT_LOCK_FILE,
    approved_digest_file: pathlib.Path | None = None,
) -> tuple[bool, str]:
    if not lock_file.exists():
        return False, f"lock file missing: {lock_file} (run lock-scorer)"
    ok, msg = soak_scorer.verify_scorer_hash(lock_file)
    if not ok:
        return False, msg
    lines = [l.strip() for l in lock_file.read_text().splitlines() if l.strip()]
    recorded: dict[str, str] = {}
    for line in lines[1:]:
        parts = line.split(None, 1)
        if len(parts) != 2:
            return False, f"malformed lock entry: {line}"
        expected, rel = parts
        recorded[rel] = expected

    expected_rels = {p.relative_to(ROOT).as_posix() for p in _locked_files()}
    missing = sorted(expected_rels - set(recorded.keys()))
    if missing:
        return False, f"lock manifest missing required files (line deleted or new soak config?): {missing}"

    for rel, expected in sorted(recorded.items()):
        p = ROOT / rel
        if not p.exists():
            return False, f"locked file missing: {rel}"
        actual = _sha256_file(p)
        if actual != expected:
            return False, (
                f"locked measurement file modified: {rel} "
                f"(expected {expected[:12]}, got {actual[:12]})"
            )
    # Always verify against tracked `config/scorer-lock.sha256` unless a test
    # explicitly supplies `approved_digest_file`, so overriding `--lock-file`
    # can never bypass tracked digest verification.
    digest_target = (
        approved_digest_file
        if approved_digest_file is not None
        else REPO_APPROVED_LOCK_DIGEST
    )
    if not digest_target.exists():
        return False, (
            f"repo approved scorer digest missing: {digest_target} "
            "(re-lock only with user OK)"
        )
    approved = _read_approved_digest(digest_target)
    actual_manifest = _sha256_file(lock_file)
    if approved != actual_manifest:
        return False, (
            f"lock manifest digest mismatch vs {digest_target}: "
            f"approved={approved[:12] if approved else 'invalid'}, "
            f"actual={actual_manifest[:12]}"
        )
    return True, f"all {len(recorded)} measurement locks verified"


def _has_human_traces(human_dir: pathlib.Path, min_hz: float = 3.5) -> bool:
    hdir = human_dir / "human"
    if not hdir.is_dir():
        return False
    traces = trace_parity.load_traces(human_dir)
    human_traces = soak_scorer._filter_role_rows(traces["human"], "human")
    valid_samples = 0
    total_span_ms = 0
    has_clips = False
    has_5hz = False
    for rows in human_traces.values():
        ts = [
            int(r["t_ms"])
            for r in rows
            if r.get("kind") == "sample"
            and all(k in r for k in ("t_ms", "x", "y", "z"))
        ]
        valid_samples += len(ts)
        if len(ts) >= 2:
            total_span_ms += max(ts) - min(ts)
        hz = trace_parity.estimate_source_hz(rows)
        if hz is not None and hz >= min_hz:
            has_5hz = True
        if trace_parity.extract_clips(rows, clip_s=30.0):
            has_clips = True
    return (
        valid_samples >= soak_scorer.MIN_HUMAN_SAMPLES
        and total_span_ms >= 60_000
        and has_clips
        and has_5hz
    )


def _changed_files_since(base_ref: str = "HEAD") -> list[str]:
    """Lists all modified/staged/committed-since-base AND untracked files."""
    tracked = [f for f in _git("diff", "--name-only", base_ref).splitlines() if f.strip()]
    untracked = [
        f for f in _git("ls-files", "--others", "--exclude-standard").splitlines() if f.strip()
    ]
    return sorted(set(tracked) | set(untracked))


def _check_edit_scope(
    changed: list[str] | None = None, base_ref: str = "HEAD"
) -> tuple[bool, list[str]]:
    if changed is None:
        changed = _changed_files_since(base_ref)
    disallowed = [
        f for f in changed
        if not any(fnmatch.fnmatch(f, pat) for pat in ALLOWED_EDIT_GLOBS)
    ]
    return len(disallowed) == 0, disallowed


def _extract_progress(res: dict) -> tuple[float, int]:
    prog = res.get("progress") or {}
    xp_h = float(prog.get("xp_per_hour") or 0.0)
    deaths = int(prog.get("deaths") or 0)
    return xp_h, deaths


def _check_fixed_guardrails(
    res: dict, min_xp_per_hour: float, max_deaths: int
) -> tuple[bool, list[str]]:
    """Enforces fixed XP/h floor and deaths ceiling on top of Layer A gates."""
    failures: list[str] = []
    if not res.get("guardrails_ok"):
        failures.append("Layer A gates failed")
    xp_h, deaths = _extract_progress(res)
    if xp_h < min_xp_per_hour:
        failures.append(f"xp/h={xp_h:.0f} < floor={min_xp_per_hour:.0f}")
    if deaths > max_deaths:
        failures.append(f"deaths={deaths} > max={max_deaths}")
    return len(failures) == 0, failures


def _revert_to(base_ref: str) -> None:
    subprocess.run(["git", "reset", "--hard", base_ref], cwd=ROOT, check=False)
    subprocess.run(["git", "clean", "-fd"], cwd=ROOT, check=False)


def _load_ledger() -> list[dict]:
    if not LEDGER.exists():
        return []
    return [json.loads(line) for line in LEDGER.read_text().splitlines() if line.strip()]


def _append_ledger(entry: dict) -> None:
    LEDGER.parent.mkdir(parents=True, exist_ok=True)
    with LEDGER.open("a") as f:
        f.write(json.dumps(entry) + "\n")


def _best_kept(entries: list[dict], suite: str | None = None) -> dict | None:
    kept = [
        e for e in entries
        if e.get("kept") and e.get("guardrails_ok")
        and (suite is None or e.get("suite", "quest") == suite)
    ]
    if not kept:
        return None
    return kept[-1]


def _save_baseline_traces(suite: str) -> pathlib.Path | None:
    src = VAR / "soak_traces"
    if not src.exists():
        return None
    dst = BASELINE_TRACES_ROOT / suite
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)
    return dst


def cmd_lock_scorer(args) -> int:
    lock_file = args.lock_file or DEFAULT_LOCK_FILE
    if lock_file.exists() and not args.force_relock:
        print(
            f"lock file already exists: {lock_file} "
            "(pass --force-relock explicitly to overwrite)",
            file=sys.stderr,
        )
        return 2
    confirmed, reason = _confirm_relock()
    if not confirmed:
        print(f"ABORT: {reason}", file=sys.stderr)
        return 2
    ok, msg = _write_lock_manifest(lock_file, force=args.force_relock, confirmed=True)
    print(msg, file=sys.stdout if ok else sys.stderr)
    return 0 if ok else 2


def cmd_calibrate(args) -> int:
    h_tr = trace_parity.load_traces(args.human_dir)["human"] if args.human_dir.exists() else None
    b_dir = BASELINE_TRACES_ROOT / args.suite
    b_tr = trace_parity.load_traces(b_dir)["bot"] if b_dir.exists() else None
    p_tr = (
        trace_parity.load_traces(args.plain_playerbots_dir)["bot"]
        if args.plain_playerbots_dir and args.plain_playerbots_dir.exists()
        else None
    )
    res = scorer_calibration.run_calibration(
        human_traces=h_tr,
        current_bot_traces=b_tr,
        plain_playerbots_traces=p_tr,
        llm_url=args.llm_url if args.judge else None,
        blind_ratings_path=args.blind_ratings if args.blind_ratings.exists() else None,
        allow_synthetic=args.allow_synthetic,
    )
    print(f"== Scorer Calibration (Layer D): {'PASS' if res['passed'] else 'FAIL'} ==")
    if res.get("error"):
        print(f"  ERROR: {res['error']}")
    for name, info in sorted(res["ratings_run1"].items(), key=lambda kv: -kv[1]["rating"]):
        print(f"  {name:20s}: {info['rating']:6.1f} [{info['ci_low']:6.1f} .. {info['ci_high']:6.1f}]")
    print(
        f"  ranking_ok={res['ranking_ok']}  repeatable={res['repeatable']}  "
        f"bad_caught={res['bad_variants_caught']}"
    )
    return 0 if res["passed"] else 1


def cmd_init(args) -> int:
    if not _has_human_traces(args.human_dir):
        if not args.allow_fallback:
            print(
                f"ABORT: fewer than {soak_scorer.MIN_HUMAN_SAMPLES} verified human samples under "
                f"{args.human_dir / 'human'}. Record human play first (or pass --allow-fallback).",
                file=sys.stderr,
            )
            return 2
        print("WARNING: running without verified human baseline (--allow-fallback)", file=sys.stderr)

    lock_file = args.lock_file or DEFAULT_LOCK_FILE
    ok, msg = _verify_all_locks(lock_file)
    if not ok:
        print(f"ABORT: {msg}", file=sys.stderr)
        return 2

    res = soak_case.run_soak(
        minutes=args.minutes,
        runs=args.runs,
        score=True,
        human_dir=args.human_dir,
        blind_ratings_path=args.blind_ratings,
        use_judge=args.judge,
        verify_hash=lock_file,
        suite=args.suite,
        check_guardrails=False,
    )
    xp_h, deaths = _extract_progress(res)
    _save_baseline_traces(args.suite)

    entries = _load_ledger()
    round_num = (entries[-1]["round"] + 1) if entries else 0
    entry = {
        "round": round_num,
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
        "commit": _git("rev-parse", "--short", "HEAD"),
        "category": "baseline",
        "suite": args.suite,
        "scenarios": res.get("scenarios", [res.get("scenario", args.suite)]),
        "hypothesis": f"baseline ({args.suite}, {args.minutes}m x {args.runs})",
        "diff_stat": "",
        "rating": res.get("rating"),
        "ci_low": res.get("ci_low"),
        "ci_high": res.get("ci_high"),
        "ranking_mode": res.get("ranking", {}).get("mode"),
        "xp_per_hour": round(xp_h, 1),
        "deaths": deaths,
        "mode": res.get("baseline_mode"),
        "human_provenance": res.get("human_provenance"),
        "guardrails_ok": res["guardrails_ok"],
        "kept": True,
        "scorer_sha256": res["scorer_sha256"],
        "tells": res.get("tells", []),
    }
    _append_ledger(entry)
    print(
        f"recorded baseline round {round_num} ({args.suite}): "
        f"rating={res.get('rating')} [{res.get('ci_low')}..{res.get('ci_high')}], "
        f"xp/h={xp_h:.0f}, gates={'PASS' if res['guardrails_ok'] else 'FAIL'}"
    )
    return 0


def cmd_eval_candidate(args) -> int:
    lock_file = args.lock_file or DEFAULT_LOCK_FILE
    ok, msg = _verify_all_locks(lock_file)
    if not ok:
        print(f"ABORT: {msg}", file=sys.stderr)
        return 2

    if not _has_human_traces(args.human_dir):
        if not args.allow_fallback:
            print(
                f"ABORT: fewer than {soak_scorer.MIN_HUMAN_SAMPLES} verified human samples under "
                f"{args.human_dir / 'human'}. Refusing without human baseline (pass --allow-fallback).",
                file=sys.stderr,
            )
            return 2
        print("WARNING: running without verified human baseline (--allow-fallback)", file=sys.stderr)

    entries = _load_ledger()
    best = _best_kept(entries, suite=args.suite)
    base_ref = args.base_ref or (best["commit"] if best and best.get("commit") else "HEAD")

    if not args.allow_any_files:
        scope_ok, disallowed = _check_edit_scope(base_ref=base_ref)
        if not scope_ok:
            print(
                "ABORT: scored round edits files outside allowed humanization scope "
                f"({ALLOWED_EDIT_GLOBS}): {disallowed}\n"
                "Submit bug fixes and C++ game-judgment changes separately, "
                "or pass --allow-any-files to override.",
                file=sys.stderr,
            )
            return 2

    if not args.skip_check:
        chk = subprocess.run([str(ROOT / "tools" / "wlb"), "check"], cwd=ROOT)
        if chk.returncode != 0:
            print("REJECT: `tools/wlb check` failed", file=sys.stderr)
            if args.auto_revert:
                _revert_to(base_ref)
            return 1

    xp_floor = float(args.min_xp_per_hour)
    max_deaths = int(args.max_deaths_per_run) * max(1, int(args.runs))
    round_num = (entries[-1]["round"] + 1) if entries else 1
    diff_stat = _git("diff", "--stat", base_ref)
    baseline_dir = BASELINE_TRACES_ROOT / args.suite

    res = soak_case.run_soak(
        minutes=args.minutes,
        runs=args.runs,
        score=True,
        human_dir=args.human_dir,
        baseline_trace_dir=baseline_dir if baseline_dir.exists() else None,
        blind_ratings_path=args.blind_ratings,
        use_judge=args.judge,
        verify_hash=lock_file,
        suite=args.suite,
        check_guardrails=False,
    )
    xp_h, deaths = _extract_progress(res)
    guardrails_ok, g_failures = _check_fixed_guardrails(res, xp_floor, max_deaths)
    if not args.allow_fallback and res.get("baseline_mode") != "human_baseline":
        guardrails_ok = False
        g_failures.append(f"scorer ran in {res.get('baseline_mode')} instead of human_baseline")
    for f in g_failures:
        print(f"GATE FAIL: {f}", file=sys.stderr)

    # Layer B keep/revert rule: beat current outside bootstrapped 95% CI.
    beats_ci = bool(res.get("ranking", {}).get("beats_current_outside_ci"))
    improved = guardrails_ok and beats_ci

    confirm_beats = None
    if improved and not args.no_confirm:
        print("\n== Candidate beat current outside 95% CI; running confirmation soak ==")
        confirm = soak_case.run_soak(
            minutes=args.minutes,
            runs=args.runs,
            score=True,
            human_dir=args.human_dir,
            baseline_trace_dir=baseline_dir if baseline_dir.exists() else None,
            blind_ratings_path=args.blind_ratings,
            use_judge=args.judge,
            verify_hash=lock_file,
            suite=args.suite,
            check_guardrails=False,
        )
        confirm_ok, _ = _check_fixed_guardrails(confirm, xp_floor, max_deaths)
        confirm_beats = bool(confirm.get("ranking", {}).get("beats_current_outside_ci"))
        if not (confirm_ok and confirm_beats):
            print("CONFIRMATION FAILED: reverting candidate", file=sys.stderr)
            improved = False

    heldout_ok = None
    if improved and not args.no_heldout and args.suite != "heldout":
        print("\n== Running held-out generalization gate (--suite heldout) ==")
        ho = soak_case.run_soak(
            minutes=args.minutes,
            runs=args.runs,
            score=True,
            human_dir=args.human_dir,
            use_judge=args.judge,
            verify_hash=lock_file,
            suite="heldout",
            check_guardrails=False,
        )
        heldout_ok, ho_failures = _check_fixed_guardrails(ho, xp_floor, max_deaths)
        if not heldout_ok:
            print(f"HELD-OUT GATE FAILED ({ho_failures}): reverting candidate", file=sys.stderr)
            improved = False

    if improved:
        _save_baseline_traces(args.suite)
        if _changed_files_since("HEAD"):
            subprocess.run(["git", "add", "-A"], cwd=ROOT, check=False)
            subprocess.run(
                ["git", "commit", "-m", f"hill-climb round {round_num}: {args.hypothesis}"],
                cwd=ROOT,
                check=False,
            )

    entry = {
        "round": round_num,
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
        "commit": _git("rev-parse", "--short", "HEAD"),
        "category": args.category,
        "suite": args.suite,
        "scenarios": res.get("scenarios", [res.get("scenario", args.suite)]),
        "target_tell": args.target,
        "hypothesis": args.hypothesis,
        "diff_stat": diff_stat,
        "rating": res.get("rating"),
        "ci_low": res.get("ci_low"),
        "ci_high": res.get("ci_high"),
        "delta_vs_current": res.get("ranking", {}).get("delta_vs_current"),
        "beats_outside_ci": beats_ci,
        "confirm_beats": confirm_beats,
        "ranking_mode": res.get("ranking", {}).get("mode"),
        "xp_per_hour": round(xp_h, 1),
        "deaths": deaths,
        "xp_floor": round(xp_floor, 1),
        "max_deaths": max_deaths,
        "mode": res.get("baseline_mode"),
        "human_provenance": res.get("human_provenance"),
        "guardrails_ok": guardrails_ok,
        "heldout_guardrails_ok": heldout_ok,
        "kept": improved,
        "scorer_sha256": res["scorer_sha256"],
        "tells": res.get("tells", []),
    }
    _append_ledger(entry)

    verdict = "KEEP" if improved else "REVERT"
    print(
        f"\n== ROUND {round_num}: {verdict} "
        f"(rating={res.get('rating')} [{res.get('ci_low')}..{res.get('ci_high')}], "
        f"beats_ci={beats_ci}, gates={'PASS' if guardrails_ok else 'FAIL'}) =="
    )
    if not improved and args.auto_revert:
        _revert_to(base_ref)
        print(f"auto-reverted working tree (`git reset --hard {base_ref} && git clean -fd`)")
    return 0 if improved else 1


def cmd_status(_args) -> int:
    entries = _load_ledger()
    if not entries:
        print("No experiments logged yet. Run `tools/bench/hill_climb.py init` first.")
        return 0
    best = _best_kept(entries)
    b_rat = best.get("rating") if best else None
    print(f"== Experiment ledger ({len(entries)} rounds, latest kept rating={b_rat}) ==")
    for e in entries[-15:]:
        verdict = "KEEP" if e.get("kept") else "REVERT"
        r_str = f"{e['rating']:6.1f}" if e.get("rating") is not None else "unrated"
        print(
            f"  #{e['round']:2d} [{verdict:6s}] rating={r_str} "
            f"({e.get('category', '-'):8s}) | {e.get('hypothesis', '')}"
        )
    if best:
        print(f"\nLatest Layer C tells: {best.get('tells')}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Hill-climbing self-improvement controller")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_lock = sub.add_parser("lock-scorer", help="Write read-only SHA-256 lock manifest")
    p_lock.add_argument("--lock-file", type=pathlib.Path, default=DEFAULT_LOCK_FILE)
    p_lock.add_argument("--force-relock", action="store_true")

    p_cal = sub.add_parser("calibrate", help="Run Layer D scorer calibration meta-checks")
    p_cal.add_argument("--suite", default="varied")
    p_cal.add_argument("--human-dir", type=pathlib.Path, default=DEFAULT_HUMAN_DIR)
    p_cal.add_argument("--plain-playerbots-dir", type=pathlib.Path, default=None)
    p_cal.add_argument("--blind-ratings", type=pathlib.Path, default=DEFAULT_BLIND_RATINGS)
    p_cal.add_argument("--judge", action="store_true", default=False)
    p_cal.add_argument("--llm-url", default="http://127.0.0.1:8090")
    p_cal.add_argument("--allow-synthetic", action="store_true")

    p_init = sub.add_parser("init", help="Verify/create lock manifest and record baseline")
    p_init.add_argument("--minutes", type=float, default=5.0)
    p_init.add_argument("--runs", type=int, default=2)
    p_init.add_argument("--suite", default="varied")
    p_init.add_argument("--human-dir", type=pathlib.Path, default=DEFAULT_HUMAN_DIR)
    p_init.add_argument("--blind-ratings", type=pathlib.Path, default=DEFAULT_BLIND_RATINGS)
    p_init.add_argument("--lock-file", type=pathlib.Path, default=DEFAULT_LOCK_FILE)
    p_init.add_argument("--judge", action="store_true", default=False)
    p_init.add_argument("--no-judge", action="store_false", dest="judge")
    p_init.add_argument("--allow-fallback", action="store_true")

    p_eval = sub.add_parser("eval-candidate", help="Evaluate current working-tree change")
    p_eval.add_argument("--hypothesis", required=True)
    p_eval.add_argument("--target", default="")
    p_eval.add_argument("--category", choices=["numbers", "prompt", "code"], default="code")
    p_eval.add_argument("--minutes", type=float, default=5.0)
    p_eval.add_argument("--runs", type=int, default=2)
    p_eval.add_argument("--suite", default="varied")
    p_eval.add_argument("--base-ref", default="")
    p_eval.add_argument("--min-xp-per-hour", type=float, default=DEFAULT_MIN_XP_PER_HOUR)
    p_eval.add_argument("--max-deaths-per-run", type=int, default=DEFAULT_MAX_DEATHS_PER_RUN)
    p_eval.add_argument("--human-dir", type=pathlib.Path, default=DEFAULT_HUMAN_DIR)
    p_eval.add_argument("--blind-ratings", type=pathlib.Path, default=DEFAULT_BLIND_RATINGS)
    p_eval.add_argument("--lock-file", type=pathlib.Path, default=DEFAULT_LOCK_FILE)
    p_eval.add_argument("--judge", action="store_true", default=False)
    p_eval.add_argument("--no-judge", action="store_false", dest="judge")
    p_eval.add_argument("--allow-fallback", action="store_true")
    p_eval.add_argument("--allow-any-files", action="store_true")
    p_eval.add_argument("--no-confirm", action="store_true")
    p_eval.add_argument("--no-heldout", action="store_true")
    p_eval.add_argument("--skip-check", action="store_true")
    p_eval.add_argument("--auto-revert", action="store_true")

    sub.add_parser("status", help="Show experiment history and current Layer C tells")

    args = ap.parse_args()
    return {
        "lock-scorer": cmd_lock_scorer,
        "calibrate": cmd_calibrate,
        "init": cmd_init,
        "eval-candidate": cmd_eval_candidate,
        "status": cmd_status,
    }[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
