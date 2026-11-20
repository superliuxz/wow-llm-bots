#!/usr/bin/env python3
"""Layer D scorer calibration & meta-checks (must pass before driving tuning).

Implements `docs/plans/self-improving-loop.md` section 3D:
  1. Correct ranking: held-out humans > current bots > known-bad variants
     (`plain_playerbots`, `random_jumper`, `instant_reply`, `same_loop_walker`).
  2. Repeatability: scoring the same bot traces twice (different random seeds)
     yields consistent Bradley-Terry ratings within uncertainty.
  3. Human agreement & sensitivity: agrees with `blind_rate.py` ratings when
     provided, and catches every known-bad variant via gates / adversary suite.
"""

from __future__ import annotations

import argparse
import datetime
import json
import math
import pathlib
import random
import statistics
import sys
from typing import Any, Callable

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import adversary_suite  # noqa: E402
import known_bad_bots  # noqa: E402
import pairwise_ranker  # noqa: E402
import trace_parity  # noqa: E402


def _default_clip_judge(
    human_ref_feats: list[dict[str, float]],
) -> Callable[[dict[str, Any], dict[str, Any]], str]:
    """Deterministic distance-to-human-manifold pairwise judge for offline calibration.

    Used ONLY as a fast deterministic stand-in during offline unit tests; live
    tuning uses human `blind_rate.py` ratings + a human-validated LLM judge.
    """
    keys = (
        "straightness",
        "turn_rate_med",
        "snap_turn_rate",
        "stops_per_min",
        "jump_interval_cv",
    )
    means = {k: statistics.mean(f[k] for f in human_ref_feats) for k in keys}
    stds = {
        k: max(statistics.stdev(f[k] for f in human_ref_feats), 0.15)
        if len(human_ref_feats) >= 2
        else 1.0
        for k in keys
    }

    def dist(clip: dict[str, Any]) -> float:
        f = adversary_suite.extract_clip_features(clip)
        d = sum(((f[k] - means[k]) / stds[k]) ** 2 for k in keys)
        # Penalize sub-human instant chat replies inside the clip.
        chats = [e for e in clip.get("events", []) if str(e.get("event", "")).startswith("chat")]
        last_in = None
        for e in chats:
            if e["event"] == "chat_in":
                last_in = e["t_ms"]
            elif e["event"] == "chat_out" and last_in is not None:
                if (e["t_ms"] - last_in) < 800:
                    d += 25.0
                last_in = None
        return math.sqrt(d)

    def judge(clip_a: dict[str, Any], clip_b: dict[str, Any]) -> str:
        da, db = dist(clip_a), dist(clip_b)
        if abs(da - db) < 0.05:
            return "tie"
        return "a" if da < db else "b"

    return judge


def _extract_all_clips(traces: dict[str, list[dict[str, Any]]], clip_s: float = 35.0) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for rows in traces.values():
        out.extend(trace_parity.extract_clips(rows, clip_s=clip_s, step_s=10.0))
    return out


def _run_tournament(
    entrants: dict[str, list[dict[str, Any]]],
    judge_fn: Callable[[dict[str, Any], dict[str, Any]], str | None],
    pairs_per_matchup: int = 12,
    seed: int = 0,
) -> dict[str, Any]:
    rng = random.Random(seed)
    names = sorted(entrants.keys())
    comparisons: list[dict[str, Any]] = []
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            ni, nj = names[i], names[j]
            ci, cj = entrants[ni], entrants[nj]
            if not ci or not cj:
                continue
            for _ in range(pairs_per_matchup):
                ca = rng.choice(ci)
                cb = rng.choice(cj)
                res = judge_fn(ca, cb)
                if res in ("a", "b", "tie"):
                    comparisons.append({"a": ni, "b": nj, "winner": res})
    return pairwise_ranker.fit_bradley_terry(comparisons, anchor_label="heldout_human", seed=seed)


def run_calibration(
    human_traces: dict[str, list[dict[str, Any]]] | None = None,
    current_bot_traces: dict[str, list[dict[str, Any]]] | None = None,
    plain_playerbots_traces: dict[str, list[dict[str, Any]]] | None = None,
    llm_url: str | None = None,
    blind_ratings_path: pathlib.Path | None = None,
    judge_gate_path: pathlib.Path = pairwise_ranker.DEFAULT_JUDGE_GATE_FILE,
    pairs_per_matchup: int = 12,
    seed: int = 42,
    allow_synthetic: bool = False,
) -> dict[str, Any]:
    """Runs all Layer D scorer meta-checks and returns a pass/fail report.

    Real calibration requires real `human_traces`, real `current_bot_traces`,
    matching source sample rates (>= 3.5 Hz, ratio <= 1.5), and a real judge
    (`--llm-url`). Pass `allow_synthetic=True` ONLY for offline unit tests.
    """
    if (not human_traces or not current_bot_traces or not plain_playerbots_traces) and not allow_synthetic:
        return {
            "passed": False,
            "error": (
                "real human_traces, current_bot_traces, and plain_playerbots_traces "
                "(--plain-playerbots-dir, a test-server soak with the brain off) are "
                "required for calibration (pass --allow-synthetic for offline unit tests only)"
            ),
            "ratings_run1": {},
            "ratings_run2": {},
            "ranking_ok": False,
            "ranking_failures": ["missing real human, bot, or plain-playerbots traces"],
            "repeatable": False,
            "bad_variants_caught": {},
            "blind_agreement": None,
        }
    if not llm_url and not allow_synthetic:
        return {
            "passed": False,
            "error": (
                "a real judge (--llm-url) is required for Layer D calibration; "
                "refusing feature-distance stand-in without --allow-synthetic"
            ),
            "ratings_run1": {},
            "ratings_run2": {},
            "ranking_ok": False,
            "ranking_failures": ["no real judge provided"],
            "repeatable": False,
            "bad_variants_caught": {},
            "blind_agreement": None,
        }

    using_real_bots = bool(current_bot_traces)
    if not human_traces:
        human_traces = {
            "RefHuman1": known_bad_bots.generate_synthetic_human("RefHuman1", seed=201),
            "RefHuman2": known_bad_bots.generate_synthetic_human("RefHuman2", seed=202),
        }
    if not current_bot_traces:
        current_bot_traces = {
            "CurrBot1": known_bad_bots.generate_synthetic_current_bot("CurrBot1", seed=301),
            "CurrBot2": known_bad_bots.generate_synthetic_current_bot("CurrBot2", seed=302),
        }

    # Source-rate parity check (rejects old 1 Hz human traces vs 5 Hz bot traces).
    parity = trace_parity.check_source_rate_parity(human_traces, current_bot_traces)
    if not parity["ok"] and not allow_synthetic:
        return {
            "passed": False,
            "error": f"recording parity check failed: {parity['reason']}",
            "parity": parity,
            "ratings_run1": {},
            "ratings_run2": {},
            "ranking_ok": False,
            "ranking_failures": [parity["reason"]],
            "repeatable": False,
            "bad_variants_caught": {},
            "blind_agreement": None,
        }

    bad_suite = known_bad_bots.build_known_bad_suite(
        real_bot_traces=current_bot_traces if using_real_bots else None,
        plain_playerbots_traces=plain_playerbots_traces,
    )

    entrants_clips: dict[str, list[dict[str, Any]]] = {
        "heldout_human": _extract_all_clips(human_traces),
        "current_bots": _extract_all_clips(current_bot_traces),
    }
    for bad_name, b_tr in bad_suite.items():
        entrants_clips[bad_name] = _extract_all_clips(b_tr)

    if llm_url:
        judge_fn = lambda ca, cb: pairwise_ranker.judge_pair_llm(ca, cb, llm_url=llm_url)
    else:
        h_feats = [adversary_suite.extract_clip_features(c) for c in entrants_clips["heldout_human"]]
        judge_fn = _default_clip_judge(h_feats)

    bt1 = _run_tournament(entrants_clips, judge_fn, pairs_per_matchup=pairs_per_matchup, seed=seed)
    bt2 = _run_tournament(entrants_clips, judge_fn, pairs_per_matchup=pairs_per_matchup, seed=seed + 1000)

    r1 = bt1["ratings"]
    r2 = bt2["ratings"]

    # Check 1: Correct ranking (humans > current > every known-bad variant).
    h_rat = r1["heldout_human"]["rating"]
    c_rat = r1["current_bots"]["rating"]
    ranking_failures: list[str] = []
    if not (h_rat > c_rat):
        ranking_failures.append(f"heldout_human ({h_rat}) <= current_bots ({c_rat})")
    for bad_name in sorted(bad_suite.keys()):
        b_rat = r1[bad_name]["rating"]
        if not (c_rat > b_rat):
            ranking_failures.append(f"current_bots ({c_rat}) <= {bad_name} ({b_rat})")

    # Check 2: Repeatability (same bot scored twice has overlapping 95% CIs).
    c1, c2 = r1["current_bots"], r2["current_bots"]
    repeatable = not (c1["ci_high"] < c2["ci_low"] or c2["ci_high"] < c1["ci_low"])

    # Check 3: Adversary & gate detection of known-bad variants.
    bad_caught: dict[str, bool] = {}
    # random_jumper caught by classifier / straightness
    adv_jump = adversary_suite.run_adversary_suite(bad_suite["random_jumper"], human_traces, clip_s=35.0)
    bad_caught["random_jumper"] = bool(adv_jump["tells"]) or (r1["random_jumper"]["rating"] < c_rat)
    # instant_reply caught by sub-human reply delay (< 0.8 s)
    adv_inst = adversary_suite.run_adversary_suite(bad_suite["instant_reply"], human_traces, clip_s=35.0)
    inst_min = adv_inst["mmo_detectors"]["chat"]["bot"]["min_reply_delay_s"]
    bad_caught["instant_reply"] = inst_min is not None and inst_min < 0.8
    # same_loop_walker caught by Mitterhofer waypoint repetition
    adv_loop = adversary_suite.run_adversary_suite(bad_suite["same_loop_walker"], human_traces, clip_s=35.0)
    loop_wp = adv_loop["mmo_detectors"]["waypoint_repeat_ratio"]["bot_mean"]
    bad_caught["same_loop_walker"] = loop_wp >= 0.45
    # plain_playerbots caught by ranking below current & adversary tells
    adv_plain = adversary_suite.run_adversary_suite(bad_suite["plain_playerbots"], human_traces, clip_s=35.0)
    bad_caught["plain_playerbots"] = bool(adv_plain["tells"]) or (r1["plain_playerbots"]["rating"] < c_rat)

    # Optional Check 4: agreement with recorded blind_rate.py ratings.
    blind_agree = None
    if blind_ratings_path and blind_ratings_path.exists():
        rows = [json.loads(l) for l in blind_ratings_path.read_text().splitlines() if l.strip()]
        if rows and llm_url:
            h_lbls, l_lbls = [], []
            for row in rows[-30:]:
                if row.get("view_a") and row.get("view_b"):
                    ans = pairwise_ranker._ask_llm_once(
                        row["view_a"], row["view_b"], llm_url, row.get("layer", "body")
                    )
                    if ans:
                        h_lbls.append(row["winner"])
                        l_lbls.append(ans)
            blind_agree = pairwise_ranker.check_judge_agreement(h_lbls, l_lbls)

    passed = (
        len(ranking_failures) == 0
        and repeatable
        and all(bad_caught.values())
        and (blind_agree is None or blind_agree["valid"])
    )

    # Persist Layer D gate verdict when calibrating a real LLM judge.
    if llm_url:
        pairwise_ranker.update_judge_gate(
            {
                "llm_url": llm_url,
                "bad_bots_caught": bool(passed and len(ranking_failures) == 0 and all(bad_caught.values())),
                "calibration_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            },
            path=judge_gate_path,
        )

    return {
        "passed": passed,
        "parity": parity,
        "ratings_run1": r1,
        "ratings_run2": r2,
        "ranking_ok": len(ranking_failures) == 0,
        "ranking_failures": ranking_failures,
        "repeatable": repeatable,
        "bad_variants_caught": bad_caught,
        "blind_agreement": blind_agree,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Run Layer D scorer calibration meta-checks")
    ap.add_argument("--human-dir", type=pathlib.Path, default=None)
    ap.add_argument("--bot-dir", type=pathlib.Path, default=None)
    ap.add_argument("--plain-playerbots-dir", type=pathlib.Path, default=None)
    ap.add_argument("--llm-url", default=None)
    ap.add_argument("--blind-ratings", type=pathlib.Path, default=None)
    ap.add_argument("--judge-gate", type=pathlib.Path, default=pairwise_ranker.DEFAULT_JUDGE_GATE_FILE)
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--allow-synthetic", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    h_tr = trace_parity.load_traces(args.human_dir)["human"] if args.human_dir else None
    b_tr = trace_parity.load_traces(args.bot_dir)["bot"] if args.bot_dir else None
    p_tr = (
        trace_parity.load_traces(args.plain_playerbots_dir)["bot"]
        if args.plain_playerbots_dir
        else None
    )
    res = run_calibration(
        human_traces=h_tr,
        current_bot_traces=b_tr,
        plain_playerbots_traces=p_tr,
        llm_url=args.llm_url,
        blind_ratings_path=args.blind_ratings,
        judge_gate_path=args.judge_gate,
        pairs_per_matchup=args.pairs,
        allow_synthetic=args.allow_synthetic,
    )
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print(f"== Scorer Calibration (Layer D): {'PASS' if res['passed'] else 'FAIL'} ==")
        if res.get("error"):
            print(f"  ERROR: {res['error']}")
        for name, info in sorted(res["ratings_run1"].items(), key=lambda kv: -kv[1]["rating"]):
            print(f"  {name:20s}: {info['rating']:6.1f} [{info['ci_low']:6.1f} .. {info['ci_high']:6.1f}]")
        print(f"  ranking_ok={res['ranking_ok']}  repeatable={res['repeatable']}  bad_caught={res['bad_variants_caught']}")
        if res["ranking_failures"]:
            for f in res["ranking_failures"]:
                print(f"  FAIL: {f}")
    return 0 if res["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
