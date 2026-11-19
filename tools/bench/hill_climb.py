#!/usr/bin/env python3
"""Self-improving hill-climbing loop controller (docs/plans/self-improving-loop.md).

Commands:
  lock-scorer      Write read-only (0444) SHA-256 lock manifest covering scorer,
                   controller, trace writer, trace call sites, snapshot config,
                   soak runner, and all soak suite configs
  init             Verify (or create if absent) lock manifest, run baseline 5x
                   to measure mean/sigma, record round 0 in ledger.jsonl
  eval-candidate   Verify exact lock manifest, enforce edit scope (staged,
                   unstaged, committed since --base-ref, and untracked), require
                   verified human traces (refuses fallback unless
                   --allow-fallback), run `wlb check` + soak, enforce fixed
                   XP/h floor & max-deaths from `res["progress"]`, require
                   > max(min_delta, 2*sigma) + confirmation re-run, and run
                   --suite heldout with full floors on every KEEP
  status           Print experiment ledger summary + current worst metric/tell
"""

import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import pathlib
import stat
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
VAR = ROOT / "var"
LEDGER = VAR / "experiments" / "ledger.jsonl"
EXTERNAL_STATE_DIR = pathlib.Path.home() / ".local" / "share" / "wow-llm-bots"
DEFAULT_LOCK_FILE = EXTERNAL_STATE_DIR / "scorer.sha256"
DEFAULT_HUMAN_DIR = EXTERNAL_STATE_DIR / "human_baseline"

# Every file that produces, configures, or gates the soak score is locked.
LOCKED_PATTERNS = [
    "tools/bench/soak_scorer.py",
    "tools/bench/human_metrics.py",
    "tools/bench/hill_climb.py",
    "tools/wlblib/cases/soak.py",
    "server_module/src/snapshot/behavior_trace.h",
    "server_module/src/snapshot/behavior_trace.cc",
    "server_module/src/snapshot/snapshot_schedule.h",
    "server_module/src/snapshot/snapshot_schedule.cc",
    "server_module/src/llm_brain_world_script.cc",
    "server_module/conf/mod-llm-brain.conf.dist",
    "server_module/src/config/llm_brain_config.cc",
    "config/llm-brain.overrides",
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
import soak_scorer  # noqa: E402
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


def _write_lock_manifest(lock_file: pathlib.Path, force: bool = False) -> tuple[bool, str]:
    if lock_file.exists() and not force:
        return False, (
            f"lock file already exists: {lock_file} "
            "(pass --force-relock explicitly to overwrite)"
        )
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
    return True, f"locked {len(lines) - 1} measurement files (0444) -> {lock_file}"


def _verify_all_locks(lock_file: pathlib.Path = DEFAULT_LOCK_FILE) -> tuple[bool, str]:
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
    return True, f"all {len(recorded)} measurement locks verified"


def _has_human_traces(human_dir: pathlib.Path) -> bool:
    hdir = human_dir / "human"
    if not hdir.is_dir():
        return False
    traces = soak_scorer.human_metrics.load_traces(human_dir)
    human_traces = soak_scorer._filter_role_rows(traces["human"], "human")
    valid_samples = 0
    total_span_ms = 0
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
    if valid_samples < soak_scorer.MIN_HUMAN_SAMPLES or total_span_ms < 60_000:
        return False
    per_char = {
        n: soak_scorer.human_metrics.analyze_character(r)
        for n, r in human_traces.items()
    }
    summary = soak_scorer.human_metrics.summarize_role(per_char)
    return summary.get("path_straightness") is not None


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
    """Reads xp_per_hour and deaths from `res['progress']` (real run_soak shape)."""
    prog = res.get("progress") or {}
    xp_h = float(prog.get("xp_per_hour") or 0.0)
    deaths = int(prog.get("deaths") or 0)
    return xp_h, deaths


def _check_fixed_guardrails(
    res: dict, min_xp_per_hour: float, max_deaths: int
) -> tuple[bool, list[str]]:
    """Enforces fixed XP/h floor and deaths ceiling on top of scorer guardrails."""
    failures: list[str] = []
    if not res.get("guardrails_ok"):
        failures.append("scorer guardrails failed")
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
    return max(kept, key=lambda e: float(e.get("score", -1.0)))


def _baseline_stats(entries: list[dict], suite: str) -> tuple[float, float]:
    """Returns (baseline_std, baseline_xp_per_hour) from the latest baseline."""
    baselines = [
        e for e in entries
        if e.get("category") == "baseline" and e.get("suite", "quest") == suite
    ]
    if not baselines:
        return 0.0, 0.0
    b = baselines[-1]
    return float(b.get("baseline_std") or 0.0), float(b.get("xp_per_hour") or 0.0)


def cmd_lock_scorer(args) -> int:
    ok, msg = _write_lock_manifest(args.lock_file, force=args.force_relock)
    print(msg, file=sys.stdout if ok else sys.stderr)
    return 0 if ok else 2


def cmd_init(args) -> int:
    if not _has_human_traces(args.human_dir):
        if not args.allow_fallback:
            print(
                f"ABORT: fewer than {soak_scorer.MIN_HUMAN_SAMPLES} verified human samples under "
                f"{args.human_dir / 'human'}. Record human play first (or pass --allow-fallback).",
                file=sys.stderr,
            )
            return 2
        print("WARNING: running in fallback_reference mode (--allow-fallback)", file=sys.stderr)

    if args.lock_file.exists():
        ok, msg = _verify_all_locks(args.lock_file)
        if not ok:
            print(f"ABORT: {msg}", file=sys.stderr)
            return 2
    else:
        ok, msg = _write_lock_manifest(args.lock_file, force=False)
        if not ok:
            print(f"ABORT: {msg}", file=sys.stderr)
            return 2
        print(msg)

    scores: list[float] = []
    xps: list[float] = []
    last_res: dict = {}
    for i in range(args.baseline_runs):
        print(f"\n== baseline repetition {i + 1}/{args.baseline_runs} ==")
        res = soak_case.run_soak(
            minutes=args.minutes,
            runs=args.runs,
            score=True,
            human_dir=args.human_dir,
            use_judge=args.judge,
            verify_hash=args.lock_file,
            suite=args.suite,
            check_guardrails=False,
        )
        xp_h, _deaths = _extract_progress(res)
        scores.append(float(res["score"]))
        xps.append(xp_h)
        last_res = res

    mean_score = statistics.mean(scores)
    std_score = statistics.stdev(scores) if len(scores) >= 2 else 0.0
    mean_xp = statistics.mean(xps) if xps else 0.0

    entries = _load_ledger()
    round_num = (entries[-1]["round"] + 1) if entries else 0
    entry = {
        "round": round_num,
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
        "commit": _git("rev-parse", "--short", "HEAD"),
        "category": "baseline",
        "suite": args.suite,
        "scenarios": last_res.get("scenarios", [last_res.get("scenario", args.suite)]),
        "target_metric": last_res.get("worst_metric"),
        "hypothesis": (
            f"baseline ({args.suite}, {args.minutes}m x {args.runs}, "
            f"{args.baseline_runs} reps)"
        ),
        "diff_stat": "",
        "score_before": None,
        "score": round(mean_score, 2),
        "baseline_scores": [round(s, 2) for s in scores],
        "baseline_std": round(std_score, 2),
        "xp_per_hour": round(mean_xp, 1),
        "mode": last_res.get("baseline_mode"),
        "human_provenance": last_res.get("human_provenance"),
        "guardrails_ok": last_res["guardrails_ok"],
        "kept": True,
        "scorer_sha256": last_res["scorer_sha256"],
        "safeguards": {
            "allow_fallback": args.allow_fallback,
            "judge": args.judge,
            "lock_file": str(args.lock_file),
            "baseline_runs": args.baseline_runs,
        },
        "per_metric": last_res["per_metric"],
        "tells": last_res["tells"],
    }
    _append_ledger(entry)
    print(
        f"recorded baseline round {round_num} ({args.suite}): "
        f"mean={mean_score:.2f} +/- {std_score:.2f} (2*sigma={2*std_score:.2f}), "
        f"xp/h={mean_xp:.0f}, worst={last_res.get('worst_metric')}"
    )
    return 0


def cmd_eval_candidate(args) -> int:
    ok, msg = _verify_all_locks(args.lock_file)
    if not ok:
        print(f"ABORT: {msg}", file=sys.stderr)
        return 2

    if not _has_human_traces(args.human_dir):
        if not args.allow_fallback:
            print(
                f"ABORT: fewer than {soak_scorer.MIN_HUMAN_SAMPLES} verified human samples under "
                f"{args.human_dir / 'human'}. Refusing fallback mode (pass --allow-fallback).",
                file=sys.stderr,
            )
            return 2
        print("WARNING: running in fallback_reference mode (--allow-fallback)", file=sys.stderr)

    entries = _load_ledger()
    best = _best_kept(entries, suite=args.suite)
    # Default base_ref to the latest kept round's commit so committed edits
    # made during the round cannot slip past the edit-scope check.
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

    best_score = float(best["score"]) if best else 0.0
    baseline_std, _baseline_xp = _baseline_stats(entries, args.suite)
    required_delta = max(args.min_delta, 2.0 * baseline_std)
    xp_floor = float(args.min_xp_per_hour)
    max_deaths = int(args.max_deaths_per_run) * max(1, int(args.runs))
    round_num = (entries[-1]["round"] + 1) if entries else 1
    diff_stat = _git("diff", "--stat", base_ref)

    res = soak_case.run_soak(
        minutes=args.minutes,
        runs=args.runs,
        score=True,
        human_dir=args.human_dir,
        use_judge=args.judge,
        verify_hash=args.lock_file,
        suite=args.suite,
        check_guardrails=False,
    )
    xp_h, deaths = _extract_progress(res)
    guardrails_ok, g_failures = _check_fixed_guardrails(res, xp_floor, max_deaths)
    if not args.allow_fallback and res.get("baseline_mode") != "human_baseline":
        guardrails_ok = False
        g_failures.append(f"scorer ran in {res.get('baseline_mode')} instead of human_baseline")
    for f in g_failures:
        print(f"GUARDRAIL FAIL: {f}", file=sys.stderr)

    improved = guardrails_ok and (best is None or res["score"] > best_score + required_delta)

    confirm_score = None
    if improved and not args.no_confirm:
        print(
            f"\n== Candidate beat {best_score:.2f} by >{required_delta:.2f} "
            f"({res['score']:.2f}); running confirmation soak =="
        )
        confirm = soak_case.run_soak(
            minutes=args.minutes,
            runs=args.runs,
            score=True,
            human_dir=args.human_dir,
            use_judge=args.judge,
            verify_hash=args.lock_file,
            suite=args.suite,
            check_guardrails=False,
        )
        confirm_score = float(confirm["score"])
        confirm_ok, _ = _check_fixed_guardrails(confirm, xp_floor, max_deaths)
        mean_two = 0.5 * (float(res["score"]) + confirm_score)
        if not (confirm_ok and mean_two > best_score + required_delta):
            print(
                f"CONFIRMATION FAILED: run1={res['score']:.2f}, run2={confirm_score:.2f}, "
                f"mean={mean_two:.2f} (need > {best_score + required_delta:.2f})",
                file=sys.stderr,
            )
            improved = False
        else:
            res["score"] = round(mean_two, 2)

    heldout_score = None
    heldout_ok = None
    if improved and not args.no_heldout and args.suite != "heldout":
        print("\n== Running held-out generalization gate (--suite heldout) ==")
        ho = soak_case.run_soak(
            minutes=args.minutes,
            runs=args.runs,
            score=True,
            human_dir=args.human_dir,
            use_judge=args.judge,
            verify_hash=args.lock_file,
            suite="heldout",
            check_guardrails=False,
        )
        heldout_score = float(ho["score"])
        heldout_ok, ho_failures = _check_fixed_guardrails(ho, xp_floor, max_deaths)
        if not heldout_ok:
            print(f"HELD-OUT GATE FAILED ({ho_failures}): reverting candidate", file=sys.stderr)
            improved = False

    entry = {
        "round": round_num,
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
        "commit": _git("rev-parse", "--short", "HEAD"),
        "category": args.category,
        "suite": args.suite,
        "scenarios": res.get("scenarios", [res.get("scenario", args.suite)]),
        "target_metric": args.target or (best.get("worst_metric") if best else None),
        "hypothesis": args.hypothesis,
        "diff_stat": diff_stat,
        "score_before": best_score if best else None,
        "score": res["score"],
        "confirm_score": confirm_score,
        "required_delta": round(required_delta, 2),
        "xp_per_hour": round(xp_h, 1),
        "deaths": deaths,
        "xp_floor": round(xp_floor, 1),
        "max_deaths": max_deaths,
        "mode": res.get("baseline_mode"),
        "human_provenance": res.get("human_provenance"),
        "guardrails_ok": guardrails_ok,
        "heldout_score": heldout_score,
        "heldout_guardrails_ok": heldout_ok,
        "kept": improved,
        "scorer_sha256": res["scorer_sha256"],
        "safeguards": {
            "allow_fallback": args.allow_fallback,
            "allow_any_files": args.allow_any_files,
            "judge": args.judge,
            "no_confirm": args.no_confirm,
            "no_heldout": args.no_heldout,
            "skip_check": args.skip_check,
            "base_ref": base_ref,
            "lock_file": str(args.lock_file),
        },
        "per_metric": res["per_metric"],
        "tells": res["tells"],
    }
    _append_ledger(entry)

    verdict = "KEEP" if improved else "REVERT"
    print(
        f"\n== ROUND {round_num}: {verdict} "
        f"(score {best_score:.2f} -> {res['score']:.2f}, "
        f"req_delta={required_delta:.2f}, "
        f"guardrails={'PASS' if guardrails_ok else 'FAIL'}) =="
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
    print(f"== Experiment ledger ({len(entries)} rounds, best={best['score']:.2f} @ round {best['round']}) ==")
    for e in entries[-15:]:
        verdict = "KEEP" if e.get("kept") else "REVERT"
        sb = f"{e['score_before']:.2f}" if e.get("score_before") is not None else "  -  "
        print(
            f"  #{e['round']:2d} [{verdict:6s}] {sb} -> {e['score']:5.2f} "
            f"({e.get('category', '-'):8s}) target={str(e.get('target_metric')):22s} | "
            f"{e.get('hypothesis', '')}"
        )
    if best:
        print(f"\nCurrent worst metric: {best.get('target_metric')} | tells: {best.get('tells')}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Hill-climbing self-improvement controller")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_lock = sub.add_parser("lock-scorer", help="Write read-only SHA-256 lock manifest")
    p_lock.add_argument("--lock-file", type=pathlib.Path, default=DEFAULT_LOCK_FILE)
    p_lock.add_argument("--force-relock", action="store_true")

    p_init = sub.add_parser("init", help="Verify/create lock manifest and record 5x baseline")
    p_init.add_argument("--minutes", type=float, default=5.0)
    p_init.add_argument("--runs", type=int, default=2)
    p_init.add_argument("--baseline-runs", type=int, default=5)
    p_init.add_argument("--suite", default="varied")
    p_init.add_argument("--human-dir", type=pathlib.Path, default=DEFAULT_HUMAN_DIR)
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
    p_eval.add_argument("--min-delta", type=float, default=0.5)
    p_eval.add_argument("--min-xp-per-hour", type=float, default=DEFAULT_MIN_XP_PER_HOUR)
    p_eval.add_argument("--max-deaths-per-run", type=int, default=DEFAULT_MAX_DEATHS_PER_RUN)
    p_eval.add_argument("--human-dir", type=pathlib.Path, default=DEFAULT_HUMAN_DIR)
    p_eval.add_argument("--lock-file", type=pathlib.Path, default=DEFAULT_LOCK_FILE)
    p_eval.add_argument("--judge", action="store_true", default=False)
    p_eval.add_argument("--no-judge", action="store_false", dest="judge")
    p_eval.add_argument("--allow-fallback", action="store_true")
    p_eval.add_argument("--allow-any-files", action="store_true")
    p_eval.add_argument("--no-confirm", action="store_true")
    p_eval.add_argument("--no-heldout", action="store_true")
    p_eval.add_argument("--skip-check", action="store_true")
    p_eval.add_argument("--auto-revert", action="store_true")

    sub.add_parser("status", help="Show experiment history and current worst metric")

    args = ap.parse_args()
    return {
        "lock-scorer": cmd_lock_scorer,
        "init": cmd_init,
        "eval-candidate": cmd_eval_candidate,
        "status": cmd_status,
    }[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
