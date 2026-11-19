#!/usr/bin/env python3
"""Self-improving hill-climbing loop controller (docs/plans/self-improving-loop.md).

Commands:
  lock-scorer      Write SHA-256 lock manifest covering scorer, trace writer,
                   soak runner, and soak suite configs to var/scorer.sha256
  init             Lock measurement files, run baseline 5 times to measure
                   mean/sigma and XP/h floor, record round 0 in ledger.jsonl
  eval-candidate   Verify all locked hashes, enforce edit scope, require real
                   human traces (refuses fallback mode unless --allow-fallback),
                   run `wlb check` + soak, require > max(min_delta, 2*sigma)
                   and XP/h floor, re-run candidate once to confirm, and run
                   --suite heldout on every KEEP (reverting on any guardrail
                   failure)
  status           Print experiment ledger summary + current worst metric/tell
"""

import argparse
import datetime
import fnmatch
import hashlib
import json
import pathlib
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
VAR = ROOT / "var"
LEDGER = VAR / "experiments" / "ledger.jsonl"
LOCK_FILE = VAR / "scorer.sha256"

# Every file that produces or configures the soak score is tamper-locked,
# including behavior_trace.* and its call sites in llm_brain_world_script.cc.
LOCKED_PATTERNS = [
    "tools/bench/soak_scorer.py",
    "tools/bench/human_metrics.py",
    "tools/wlblib/cases/soak.py",
    "server_module/src/snapshot/behavior_trace.h",
    "server_module/src/snapshot/behavior_trace.cc",
    "server_module/src/llm_brain_world_script.cc",
    "config/tests/soak_*.llm-brain.overrides",
]

DEFAULT_MIN_XP_PER_HOUR = 3000.0
DEFAULT_MAX_DEATHS_PER_RUN = 2

# Allowed file globs for scored humanization rounds (bug fixes & C++ game
# judgment belong in separate proposals, not mixed into scored rounds).
ALLOWED_EDIT_GLOBS = [
    "server_module/src/options/human_pacing*",
    "content/RULES.md",
    "content/identities/*",
]

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
    files: list[pathlib.Path] = []
    for pat in LOCKED_PATTERNS:
        if "*" in pat:
            files.extend(sorted(ROOT.glob(pat)))
        else:
            p = ROOT / pat
            if p.exists():
                files.append(p)
    return files


def _verify_all_locks() -> tuple[bool, str]:
    if not LOCK_FILE.exists():
        return False, f"lock file missing: {LOCK_FILE.relative_to(ROOT)} (run lock-scorer)"
    ok, msg = soak_scorer.verify_scorer_hash(LOCK_FILE)
    if not ok:
        return False, msg
    lines = [l.strip() for l in LOCK_FILE.read_text().splitlines() if l.strip()]
    # Skip line 0 (combined soak_scorer.scorer_sha256() digest checked above).
    for line in lines[1:]:
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        expected, rel = parts
        p = ROOT / rel
        if not p.exists():
            return False, f"locked file missing: {rel}"
        actual = _sha256_file(p)
        if actual != expected:
            return False, (
                f"locked measurement file modified: {rel} "
                f"(expected {expected[:12]}, got {actual[:12]})"
            )
    return True, "all measurement locks verified"


def _has_human_traces(human_dir: pathlib.Path) -> bool:
    hdir = human_dir / "human"
    return hdir.is_dir() and any(hdir.rglob("*.jsonl"))


def _check_edit_scope(changed: list[str] | None = None) -> tuple[bool, list[str]]:
    if changed is None:
        changed = [f for f in _git("diff", "--name-only").splitlines() if f.strip()]
    disallowed = [
        f for f in changed
        if not any(fnmatch.fnmatch(f, pat) for pat in ALLOWED_EDIT_GLOBS)
    ]
    return len(disallowed) == 0, disallowed


def _check_fixed_guardrails(
    res: dict, min_xp_per_hour: float, max_deaths: int
) -> tuple[bool, list[str]]:
    """Enforces fixed XP/h floor and deaths ceiling on top of scorer guardrails."""
    failures: list[str] = []
    if not res.get("guardrails_ok"):
        failures.append("scorer guardrails failed")
    xp_h = float(res.get("xp_per_hour") or 0.0)
    if xp_h < min_xp_per_hour:
        failures.append(f"xp/h={xp_h:.0f} < floor={min_xp_per_hour:.0f}")
    deaths = int(res.get("deaths") or 0)
    if deaths > max_deaths:
        failures.append(f"deaths={deaths} > max={max_deaths}")
    return len(failures) == 0, failures


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


def cmd_lock_scorer(_args) -> int:
    digest = soak_scorer.scorer_sha256()
    LOCK_FILE.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"{digest}  tools/bench/soak_scorer.py"]
    for p in _locked_files():
        rel = p.relative_to(ROOT).as_posix()
        lines.append(f"{_sha256_file(p)}  {rel}")
    LOCK_FILE.write_text("\n".join(lines) + "\n")
    print(f"locked {len(lines) - 1} measurement files -> {LOCK_FILE.relative_to(ROOT)}")
    return 0


def cmd_init(args) -> int:
    if not _has_human_traces(args.human_dir):
        if not args.allow_fallback:
            print(
                f"ABORT: no human traces found under {args.human_dir / 'human'}. "
                "Record human play first (or pass --allow-fallback for smoke tests).",
                file=sys.stderr,
            )
            return 2
        print("WARNING: running in fallback_reference mode (--allow-fallback)", file=sys.stderr)

    cmd_lock_scorer(args)
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
            verify_hash=LOCK_FILE,
            suite=args.suite,
            check_guardrails=False,
        )
        scores.append(float(res["score"]))
        xps.append(float(res.get("xp_per_hour") or 0.0))
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
        "mode": last_res.get("mode"),
        "guardrails_ok": last_res["guardrails_ok"],
        "kept": True,
        "scorer_sha256": last_res["scorer_sha256"],
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
    ok, msg = _verify_all_locks()
    if not ok:
        print(f"ABORT: {msg}", file=sys.stderr)
        return 2

    if not _has_human_traces(args.human_dir):
        if not args.allow_fallback:
            print(
                f"ABORT: no human traces found under {args.human_dir / 'human'}. "
                "Refusing to score in fallback mode (pass --allow-fallback to override).",
                file=sys.stderr,
            )
            return 2
        print("WARNING: running in fallback_reference mode (--allow-fallback)", file=sys.stderr)

    if not args.allow_any_files:
        scope_ok, disallowed = _check_edit_scope()
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
                subprocess.run(["git", "checkout", "--", "."], cwd=ROOT)
            return 1

    entries = _load_ledger()
    best = _best_kept(entries, suite=args.suite)
    best_score = float(best["score"]) if best else 0.0
    baseline_std, _baseline_xp = _baseline_stats(entries, args.suite)
    required_delta = max(args.min_delta, 2.0 * baseline_std)
    xp_floor = float(args.min_xp_per_hour)
    max_deaths = int(args.max_deaths_per_run) * max(1, int(args.runs))
    round_num = (entries[-1]["round"] + 1) if entries else 1
    diff_stat = _git("diff", "--stat")

    res = soak_case.run_soak(
        minutes=args.minutes,
        runs=args.runs,
        score=True,
        human_dir=args.human_dir,
        use_judge=args.judge,
        verify_hash=LOCK_FILE,
        suite=args.suite,
        check_guardrails=False,
    )
    guardrails_ok, g_failures = _check_fixed_guardrails(res, xp_floor, max_deaths)
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
            verify_hash=LOCK_FILE,
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
            verify_hash=LOCK_FILE,
            suite="heldout",
            check_guardrails=False,
        )
        heldout_score = float(ho["score"])
        heldout_ok = bool(ho["guardrails_ok"])
        if not heldout_ok:
            print("HELD-OUT GATE FAILED: reverting candidate", file=sys.stderr)
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
        "xp_per_hour": res.get("xp_per_hour"),
        "xp_floor": round(xp_floor, 1),
        "mode": res.get("mode"),
        "guardrails_ok": guardrails_ok,
        "heldout_score": heldout_score,
        "heldout_guardrails_ok": heldout_ok,
        "kept": improved,
        "scorer_sha256": res["scorer_sha256"],
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
        subprocess.run(["git", "checkout", "--", "."], cwd=ROOT)
        print("auto-reverted working tree changes (`git checkout -- .`)")
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

    sub.add_parser("lock-scorer", help="Write SHA-256 lock manifest for all measurement files")

    p_init = sub.add_parser("init", help="Lock measurement files and record 5x baseline")
    p_init.add_argument("--minutes", type=float, default=5.0)
    p_init.add_argument("--runs", type=int, default=2)
    p_init.add_argument("--baseline-runs", type=int, default=5)
    p_init.add_argument("--suite", default="varied")
    p_init.add_argument("--human-dir", type=pathlib.Path, default=VAR / "human_baseline")
    p_init.add_argument("--judge", action="store_true", default=True)
    p_init.add_argument("--no-judge", action="store_false", dest="judge")
    p_init.add_argument("--allow-fallback", action="store_true")

    p_eval = sub.add_parser("eval-candidate", help="Evaluate current working-tree change")
    p_eval.add_argument("--hypothesis", required=True)
    p_eval.add_argument("--target", default="")
    p_eval.add_argument("--category", choices=["numbers", "prompt", "code"], default="code")
    p_eval.add_argument("--minutes", type=float, default=5.0)
    p_eval.add_argument("--runs", type=int, default=2)
    p_eval.add_argument("--suite", default="varied")
    p_eval.add_argument("--min-delta", type=float, default=0.5)
    p_eval.add_argument("--min-xp-per-hour", type=float, default=DEFAULT_MIN_XP_PER_HOUR)
    p_eval.add_argument("--max-deaths-per-run", type=int, default=DEFAULT_MAX_DEATHS_PER_RUN)
    p_eval.add_argument("--human-dir", type=pathlib.Path, default=VAR / "human_baseline")
    p_eval.add_argument("--judge", action="store_true", default=True)
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
