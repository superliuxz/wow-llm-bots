#!/usr/bin/env python3
"""Self-improving hill-climbing loop controller (docs/plans/self-improving-loop.md).

Commands:
  lock-scorer      Write current scorer SHA-256 to var/scorer.sha256
  init             Lock scorer, run baseline soak (--score), record round 0
  eval-candidate   Verify scorer hash, run `wlb check` + soak (--score), compare
                   against best kept score, log to var/experiments/ledger.jsonl,
                   and emit KEEP or REVERT (with optional --auto-revert)
  status           Print experiment ledger summary + current worst metric/tell
"""

import argparse
import datetime
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
VAR = ROOT / "var"
LEDGER = VAR / "experiments" / "ledger.jsonl"
LOCK_FILE = VAR / "scorer.sha256"

sys.path.insert(0, str(ROOT / "tools" / "bench"))
sys.path.insert(0, str(ROOT / "tools"))
import soak_scorer  # noqa: E402
from wlblib.cases import soak as soak_case  # noqa: E402


def _git(*args: str) -> str:
    res = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    return res.stdout.strip()


def _load_ledger() -> list[dict]:
    if not LEDGER.exists():
        return []
    return [json.loads(line) for line in LEDGER.read_text().splitlines() if line.strip()]


def _append_ledger(entry: dict) -> None:
    LEDGER.parent.mkdir(parents=True, exist_ok=True)
    with LEDGER.open("a") as f:
        f.write(json.dumps(entry) + "\n")


def _best_kept(entries: list[dict]) -> dict | None:
    kept = [e for e in entries if e.get("kept") and e.get("guardrails_ok")]
    if not kept:
        return None
    return max(kept, key=lambda e: float(e.get("score", -1.0)))


def cmd_lock_scorer(_args) -> int:
    digest = soak_scorer.scorer_sha256()
    LOCK_FILE.parent.mkdir(parents=True, exist_ok=True)
    LOCK_FILE.write_text(f"{digest}  tools/bench/soak_scorer.py\n")
    print(f"locked scorer hash: {digest} -> {LOCK_FILE.relative_to(ROOT)}")
    return 0


def cmd_init(args) -> int:
    cmd_lock_scorer(args)
    res = soak_case.run_soak(
        minutes=args.minutes,
        runs=args.runs,
        score=True,
        human_dir=args.human_dir,
        use_judge=args.judge,
        verify_hash=LOCK_FILE,
    )
    entry = {
        "round": 0,
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
        "commit": _git("rev-parse", "--short", "HEAD"),
        "category": "baseline",
        "target_metric": res.get("worst_metric"),
        "hypothesis": "baseline measurement",
        "diff_stat": "",
        "score_before": None,
        "score": res["score"],
        "guardrails_ok": res["guardrails_ok"],
        "kept": True,
        "scorer_sha256": res["scorer_sha256"],
        "per_metric": res["per_metric"],
        "tells": res["tells"],
    }
    _append_ledger(entry)
    print(f"recorded baseline round 0: score={res['score']:.2f}, worst={res.get('worst_metric')}")
    return 0


def cmd_eval_candidate(args) -> int:
    ok, msg = soak_scorer.verify_scorer_hash(LOCK_FILE)
    if not ok:
        print(f"ABORT: {msg}", file=sys.stderr)
        return 2

    if not args.skip_check:
        chk = subprocess.run([str(ROOT / "tools" / "wlb"), "check"], cwd=ROOT)
        if chk.returncode != 0:
            print("REJECT: `tools/wlb check` failed", file=sys.stderr)
            if args.auto_revert:
                subprocess.run(["git", "checkout", "--", "."], cwd=ROOT)
            return 1

    entries = _load_ledger()
    best = _best_kept(entries)
    best_score = float(best["score"]) if best else 0.0
    round_num = (entries[-1]["round"] + 1) if entries else 1
    diff_stat = _git("diff", "--stat")

    res = soak_case.run_soak(
        minutes=args.minutes,
        runs=args.runs,
        score=True,
        human_dir=args.human_dir,
        use_judge=args.judge,
        verify_hash=LOCK_FILE,
    )
    improved = res["guardrails_ok"] and (best is None or res["score"] > best_score + args.min_delta)
    entry = {
        "round": round_num,
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
        "commit": _git("rev-parse", "--short", "HEAD"),
        "category": args.category,
        "target_metric": args.target or (best.get("worst_metric") if best else None),
        "hypothesis": args.hypothesis,
        "diff_stat": diff_stat,
        "score_before": best_score if best else None,
        "score": res["score"],
        "guardrails_ok": res["guardrails_ok"],
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
        f"guardrails={'PASS' if res['guardrails_ok'] else 'FAIL'}) =="
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

    sub.add_parser("lock-scorer", help="Write scorer SHA-256 lock file")

    p_init = sub.add_parser("init", help="Lock scorer and record round-0 baseline")
    p_init.add_argument("--minutes", type=float, default=2.0)
    p_init.add_argument("--runs", type=int, default=1)
    p_init.add_argument("--human-dir", type=pathlib.Path, default=VAR / "human_baseline")
    p_init.add_argument("--judge", action="store_true")

    p_eval = sub.add_parser("eval-candidate", help="Evaluate current working-tree change")
    p_eval.add_argument("--hypothesis", required=True)
    p_eval.add_argument("--target", default="")
    p_eval.add_argument("--category", choices=["numbers", "prompt", "code"], default="code")
    p_eval.add_argument("--minutes", type=float, default=2.0)
    p_eval.add_argument("--runs", type=int, default=2)
    p_eval.add_argument("--min-delta", type=float, default=0.5)
    p_eval.add_argument("--human-dir", type=pathlib.Path, default=VAR / "human_baseline")
    p_eval.add_argument("--judge", action="store_true")
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
