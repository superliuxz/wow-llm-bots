#!/usr/bin/env python3
"""Layered human-likeness scorer (A gates + B pairwise ranking + C diagnostics).

Implements `docs/plans/self-improving-loop.md` sections 3A-3C & 4.6.
The retired single 0-100 movement-average composite score has been removed.

Layers:
  A. Gates (must pass, never traded off):
     - zero crashes / LLM errors / invalid plans
     - bounded wake rate (<= 20/min/bot) & latency (`do:` p95 <= 2000 ms)
     - bounded stuck bots, deaths, and repeated refusal loops
     - human timing limits (GT Sophy: no < 0.8 s whisper replies or < 150 ms
       aggro reactions)
     - optional offline plan eval non-regression
  B. Ranking (what drives tuning):
     - pairwise blind comparisons (candidate vs current + human anchors) fed
       into Bradley-Terry with bootstrapped 95% confidence intervals
     - ground truth from `blind_rate.py`, or order-debiased LLM judge while
       calibrated against human blind ratings
  C. Diagnostics (never a reward):
     - per-situation `scikit-learn` classifier + MMO adversary suite
       (`adversary_suite.py`) + population & long-horizon metrics
       (`population_metrics.py`)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import random
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import adversary_suite  # noqa: E402
import pairwise_ranker  # noqa: E402
import population_metrics  # noqa: E402
import trace_parity  # noqa: E402

MIN_HUMAN_SAMPLES = 300
MIN_HUMAN_WHISPER_DELAY_S = 0.8
MIN_HUMAN_REACTION_MS = 150.0
MAX_REPEATED_REFUSALS = 5

SCORER_FILES = [
    "tools/bench/soak_scorer.py",
    "tools/bench/trace_parity.py",
    "tools/bench/pairwise_ranker.py",
    "tools/bench/adversary_suite.py",
    "tools/bench/population_metrics.py",
    "tools/bench/known_bad_bots.py",
    "tools/bench/scorer_calibration.py",
]


def scorer_sha256() -> str:
    """SHA-256 over all layered scorer modules for tamper checks."""
    h = hashlib.sha256()
    for rel in SCORER_FILES:
        path = (ROOT / rel).resolve()
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        h.update(path.read_bytes())
        h.update(b"\0")
    return h.hexdigest()


def verify_scorer_hash(lock_file: pathlib.Path) -> tuple[bool, str]:
    cur = scorer_sha256()
    if not lock_file.exists():
        return False, f"lock file missing: {lock_file} (run `hill_climb.py lock-scorer`)"
    expected = lock_file.read_text().strip().split()[0]
    if cur != expected:
        return False, f"scorer hash mismatch! expected {expected[:12]}..., got {cur[:12]}..."
    return True, cur


def _filter_role_rows(traces: dict[str, list[dict]], expected_role: str) -> dict[str, list[dict]]:
    """Keeps only records whose embedded `role` matches `expected_role`."""
    out: dict[str, list[dict]] = {}
    for name, rows in traces.items():
        filtered = [r for r in rows if r.get("role") == expected_role]
        if filtered:
            out[name] = filtered
    return out


def _progress_metrics(bot_traces: dict[str, list[dict]]) -> dict[str, Any]:
    """Computes XP/h, quests turned in, deaths, stuck bots, and moved bots."""
    total_xp = 0
    total_hours = 0.0
    deaths = 0
    quests_completed = 0
    moved_bots = 0
    stuck_bots = 0
    for _name, rows in bot_traces.items():
        samples = [r for r in rows if r.get("kind") == "sample"]
        quests_completed += sum(
            1 for r in rows if r.get("kind") == "event" and r.get("event") == "quest_turnin"
        )
        if len(samples) < 2:
            stuck_bots += 1
            continue
        dur_s = max(0.0, (samples[-1]["t_ms"] - samples[0]["t_ms"]) / 1000.0)
        total_hours += dur_s / 3600.0
        for i in range(1, len(samples)):
            prev_s, cur_s = samples[i - 1], samples[i]
            if not prev_s.get("dead") and cur_s.get("dead"):
                deaths += 1
            dxp = int(cur_s.get("xp", 0)) - int(prev_s.get("xp", 0))
            if dxp > 0:
                total_xp += dxp
        x0, y0 = samples[0]["x"], samples[0]["y"]
        max_d = max(math.hypot(s["x"] - x0, s["y"] - y0) for s in samples)
        if max_d >= 3.0:
            moved_bots += 1
        else:
            stuck_bots += 1
    xp_per_hour = (total_xp / total_hours) if total_hours > 0 else 0.0
    quests_per_hour = (quests_completed / total_hours) if total_hours > 0 else 0.0
    return {
        "total_xp": total_xp,
        "xp_per_hour": round(xp_per_hour, 1),
        "quests_completed": quests_completed,
        "quests_per_hour": round(quests_per_hour, 2),
        "deaths": deaths,
        "moved_bots": moved_bots,
        "stuck_bots": stuck_bots,
        "traced_bots": len(bot_traces),
    }


def _clips_by_situation(traces: dict[str, list[dict]], clip_s: float = 45.0) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {}
    for rows in traces.values():
        for c in trace_parity.extract_clips(rows, clip_s=clip_s):
            out.setdefault(c["situation"], []).append(c)
    return out


def _run_layer_b_ranking(
    bot_traces: dict[str, list[dict]],
    human_traces: dict[str, list[dict]],
    candidate_id: str = "",
    current_id: str = "",
    baseline_traces: dict[str, list[dict]] | None = None,
    blind_ratings_path: pathlib.Path | None = None,
    judge_gate_path: pathlib.Path = pairwise_ranker.DEFAULT_JUDGE_GATE_FILE,
    use_judge: bool = False,
    llm_url: str = "http://127.0.0.1:8090",
    pairs: int = 24,
    seed: int = 0,
    parity_ok: bool = True,
) -> dict[str, Any]:
    """Runs Layer B pairwise Bradley-Terry ranking anchored to human clips."""
    comparisons: list[dict[str, Any]] = []
    mode = "unrated_awaiting_blind_batch"
    judge_gate_error = ""

    # 1. Include ONLY blind ratings tagged with this exact candidate_id (and
    #    current_id when referencing current) so old candidates never leak in.
    if blind_ratings_path and blind_ratings_path.exists():
        for line in blind_ratings_path.read_text().splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            a, b, w = r.get("a"), r.get("b"), r.get("winner")
            if not a or not b or w not in ("a", "b", "tie"):
                continue
            a_id = str(r.get("a_id", ""))
            b_id = str(r.get("b_id", ""))
            if a == "candidate" and (not candidate_id or a_id != candidate_id):
                continue
            if b == "candidate" and (not candidate_id or b_id != candidate_id):
                continue
            if a == "current" and (not current_id or a_id != current_id):
                continue
            if b == "current" and (not current_id or b_id != current_id):
                continue
            comparisons.append({"a": a, "b": b, "winner": w})
        if comparisons:
            mode = "blind_human"

    # 2. Enforce the LLM judge gate (agreement with human blind ratings AND
    #    Layer D known-bad bot detection) before allowing --judge comparisons.
    if use_judge:
        if not parity_ok:
            judge_gate_error = "source-rate parity failed (refusing LLM judge on mismatched rates)"
            mode = "llm_judge_blocked_parity"
        else:
            gate = pairwise_ranker.load_judge_gate(judge_gate_path)
            gate_ok, gate_msg = pairwise_ranker.is_judge_gate_valid(gate, llm_url)
            if not gate_ok:
                judge_gate_error = gate_msg
                if mode == "unrated_awaiting_blind_batch":
                    mode = "llm_judge_blocked_uncalibrated"
            elif bot_traces and human_traces:
                rng = random.Random(seed)
                sources: dict[str, dict[str, list[dict]]] = {
                    "human": _clips_by_situation(human_traces),
                    "candidate": _clips_by_situation(bot_traces),
                }
                if baseline_traces:
                    sources["current"] = _clips_by_situation(baseline_traces)

                names = sorted(sources.keys())
                matchups: list[tuple[str, str, str]] = []
                for i in range(len(names)):
                    for j in range(i + 1, len(names)):
                        ni, nj = names[i], names[j]
                        shared = sorted(set(sources[ni].keys()) & set(sources[nj].keys()))
                        for sit in shared:
                            matchups.append((sit, ni, nj))

                if matchups:
                    for _ in range(pairs):
                        sit, left, right = rng.choice(matchups)
                        ca = rng.choice(sources[left][sit])
                        cb = rng.choice(sources[right][sit])
                        layer = "social" if sit == "chatting" else "body"
                        ans = pairwise_ranker.judge_pair_llm(ca, cb, llm_url=llm_url, layer=layer)
                        if ans in ("a", "b", "tie"):
                            comparisons.append({"a": left, "b": right, "winner": ans})
                    mode = "blind_human+llm" if mode == "blind_human" else "llm_pairwise"

    if not comparisons:
        return {
            "mode": mode,
            "judge_gate_error": judge_gate_error,
            "candidate_id": candidate_id,
            "current_id": current_id,
            "comparisons": 0,
            "rating": None,
            "ci_low": None,
            "ci_high": None,
            "human_anchor_rating": pairwise_ranker.HUMAN_ANCHOR_RATING,
            "beats_current_outside_ci": False,
            "delta_vs_current": None,
            "ratings": {},
        }

    bt = pairwise_ranker.fit_bradley_terry(comparisons, anchor_label="human", seed=seed)
    cand_info = bt["ratings"].get("candidate")
    beats, delta_info = (
        pairwise_ranker.beats_outside_ci(bt, "candidate", "current")
        if ("candidate" in bt["ratings"] and "current" in bt["ratings"])
        else (False, {"delta": 0.0, "ci_low": 0.0, "ci_high": 0.0})
    )
    return {
        "mode": mode,
        "judge_gate_error": judge_gate_error,
        "candidate_id": candidate_id,
        "current_id": current_id,
        "comparisons": bt["comparisons"],
        "rating": cand_info["rating"] if cand_info else None,
        "ci_low": cand_info["ci_low"] if cand_info else None,
        "ci_high": cand_info["ci_high"] if cand_info else None,
        "human_anchor_rating": pairwise_ranker.HUMAN_ANCHOR_RATING,
        "beats_current_outside_ci": beats,
        "delta_vs_current": delta_info if "current" in bt["ratings"] else None,
        "ratings": bt["ratings"],
    }


def score_soak(
    trace_dir: pathlib.Path,
    run_stats: dict[str, Any],
    human_dir: pathlib.Path | None = None,
    baseline_trace_dir: pathlib.Path | None = None,
    blind_ratings_path: pathlib.Path | None = None,
    judge_gate_path: pathlib.Path = pairwise_ranker.DEFAULT_JUDGE_GATE_FILE,
    use_judge: bool = False,
    llm_url: str = "http://127.0.0.1:8090",
    min_eval_passed: int | None = None,
    eval_passed: int | None = None,
) -> dict[str, Any]:
    traces = trace_parity.load_traces(trace_dir)
    bot_traces = _filter_role_rows(traces["bot"], "bot")
    human_traces = _filter_role_rows(traces["human"], "human")
    if human_dir and human_dir.exists():
        extra = trace_parity.load_traces(human_dir)
        human_traces = {**human_traces, **_filter_role_rows(extra["human"], "human")}

    baseline_traces = None
    if baseline_trace_dir and baseline_trace_dir.exists():
        b_extra = trace_parity.load_traces(baseline_trace_dir)
        baseline_traces = _filter_role_rows(b_extra["bot"], "bot") or None

    candidate_id = trace_parity.trace_set_hash(trace_dir, role="bot")
    current_id = (
        trace_parity.trace_set_hash(baseline_trace_dir, role="bot")
        if baseline_trace_dir and baseline_trace_dir.exists()
        else ""
    )

    human_sample_count = sum(
        sum(1 for r in rows if r.get("kind") == "sample")
        for rows in human_traces.values()
    )
    has_human = len(human_traces) > 0 and human_sample_count >= MIN_HUMAN_SAMPLES
    parity = (
        trace_parity.check_source_rate_parity(human_traces, bot_traces)
        if has_human and bot_traces
        else {"ok": True, "human_hz": None, "bot_hz": None, "reason": "no_human_comparison"}
    )
    baseline_mode = (
        "human_baseline"
        if (has_human and parity["ok"])
        else ("parity_mismatch" if has_human else "no_human_baseline")
    )

    progress = _progress_metrics(bot_traces)

    # Layer C: Diagnostics (never a reward). Skip human comparison if source rates mismatch.
    adversary = adversary_suite.run_adversary_suite(
        bot_traces, human_traces if parity["ok"] else {}
    )
    population = population_metrics.analyze_population(
        bot_traces, human_traces if parity["ok"] else {}
    )
    tells = list(dict.fromkeys(adversary.get("tells", []) + population.get("tells", [])))

    # Layer B: Pairwise ranking anchored to human clips.
    ranking = _run_layer_b_ranking(
        bot_traces=bot_traces,
        human_traces=human_traces,
        candidate_id=candidate_id,
        current_id=current_id,
        baseline_traces=baseline_traces,
        blind_ratings_path=blind_ratings_path,
        judge_gate_path=judge_gate_path,
        use_judge=use_judge,
        llm_url=llm_url,
        parity_ok=parity["ok"],
    )

    # Layer A: Hard gates (must pass, never traded off).
    guardrails: list[dict[str, Any]] = []

    def check(name: str, ok: bool, detail: str) -> None:
        guardrails.append({"name": name, "ok": bool(ok), "detail": detail})

    n_bots = max(1, int(run_stats.get("bots", progress["traced_bots"] or 1)))
    min_traced = max(1, n_bots // 2)
    check("has_traced_bots", progress["traced_bots"] >= min_traced,
          f"traced_bots={progress['traced_bots']} (>= {min_traced})")
    check("has_decisions", int(run_stats.get("decisions", 0)) >= 1,
          f"decisions={run_stats.get('decisions', 0)} (>= 1)")
    check("no_llm_errors", run_stats.get("llm_errors", 0) == 0,
          f"llm_errors={run_stats.get('llm_errors', 0)}")
    check("no_invalid_plans", run_stats.get("invalid_plans", 0) == 0,
          f"invalid_plans={run_stats.get('invalid_plans', 0)}")
    check("no_wake_loop", run_stats.get("max_wakes_per_min", 0.0) <= 20.0,
          f"max_wakes/min/bot={run_stats.get('max_wakes_per_min', 0.0):.1f} (<= 20.0)")
    check("latency_p95", run_stats.get("do_p95_ms", 0.0) <= 2000.0,
          f"do_p95={run_stats.get('do_p95_ms', 0.0):.0f}ms (<= 2000ms)")
    max_stuck = max(1, int(math.floor(n_bots * 0.3)))
    check("not_stuck", progress["stuck_bots"] <= max_stuck,
          f"stuck={progress['stuck_bots']}/{progress['traced_bots']} (<= {max_stuck})")
    check("deaths_bounded", progress["deaths"] <= n_bots,
          f"deaths={progress['deaths']} (<= {n_bots})")

    max_refusals = int(run_stats.get("max_repeated_refusals", 0))
    check("no_refusal_loop", max_refusals <= MAX_REPEATED_REFUSALS,
          f"max_repeated_refusals={max_refusals} (<= {MAX_REPEATED_REFUSALS})")

    # Human timing limits (GT Sophy): no sub-human instant replies or reactions.
    min_whisper_s = adversary["mmo_detectors"]["chat"]["bot"].get("min_reply_delay_s")
    whisper_ok = min_whisper_s is None or min_whisper_s >= MIN_HUMAN_WHISPER_DELAY_S
    check("human_whisper_timing", whisper_ok,
          f"min_whisper_delay={min_whisper_s}s (>= {MIN_HUMAN_WHISPER_DELAY_S}s)")
    react_pcts = adversary["mmo_detectors"]["reaction_ms_p10_p50_p90"].get("bot")
    react_ok = react_pcts is None or react_pcts[0] >= MIN_HUMAN_REACTION_MS
    check("human_react_timing", react_ok,
          f"react_p10={react_pcts[0] if react_pcts else None}ms (>= {MIN_HUMAN_REACTION_MS:.0f}ms)")

    if has_human:
        check("recording_parity", parity["ok"],
              f"human={parity['human_hz']}Hz bot={parity['bot_hz']}Hz ({parity['reason']})")

    if min_eval_passed is not None and eval_passed is not None:
        check("offline_eval", eval_passed >= min_eval_passed,
              f"eval={eval_passed} (>= {min_eval_passed})")

    guardrails_ok = all(g["ok"] for g in guardrails)

    return {
        "rating": ranking["rating"],
        "ci_low": ranking["ci_low"],
        "ci_high": ranking["ci_high"],
        "ranking": ranking,
        "parity": parity,
        "baseline_mode": baseline_mode,
        "human_provenance": {
            "dir": str(human_dir) if human_dir else "",
            "characters": len(human_traces),
            "samples": human_sample_count,
        },
        "guardrails_ok": guardrails_ok,
        "guardrails": guardrails,
        "progress": progress,
        "adversary": adversary,
        "population": population,
        "tells": tells,
        "run_stats": run_stats,
        "scorer_sha256": scorer_sha256(),
    }


def format_score_report(res: dict[str, Any]) -> str:
    r = res["ranking"]
    if res["rating"] is not None:
        rat_str = (
            f"RATING: {res['rating']:.1f} [95% CI {res['ci_low']:.1f}..{res['ci_high']:.1f}] "
            f"(human anchor={r['human_anchor_rating']:.0f}, mode={r['mode']}, n={r['comparisons']})"
        )
    else:
        rat_str = f"RATING: unrated ({r['mode']}; run blind_rate.py or pass --judge)"
    lines = [
        f"{rat_str}  |  gates={'PASS' if res['guardrails_ok'] else 'FAIL'}  "
        f"|  scorer={res['scorer_sha256'][:12]}",
    ]
    if r.get("delta_vs_current"):
        d = r["delta_vs_current"]
        lines.append(
            f"  vs current: delta={d['delta']:+.1f} [95% CI {d['ci_low']:+.1f}..{d['ci_high']:+.1f}] "
            f"beats_outside_ci={r['beats_current_outside_ci']}"
        )
    p = res["progress"]
    lines.append(
        f"  progress: xp/h={p['xp_per_hour']:.0f} (total_xp={p['total_xp']}), "
        f"quests={p['quests_completed']}, deaths={p['deaths']}, "
        f"moved={p['moved_bots']}/{p['traced_bots']}"
    )
    lines.append("  Layer A gates:")
    for g in res["guardrails"]:
        lines.append(f"    {'PASS' if g['ok'] else 'FAIL'} {g['name']:22s}: {g['detail']}")
    lines.append("  Layer C diagnostic tells (never a reward):")
    for t in res["tells"] or ["(no strong tells flagged)"]:
        lines.append(f"    - {t}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Score a soak trace directory (layered A/B/C scorer)")
    ap.add_argument("trace_dir", type=pathlib.Path)
    ap.add_argument("--human-dir", type=pathlib.Path, default=None)
    ap.add_argument("--baseline-dir", type=pathlib.Path, default=None)
    ap.add_argument("--blind-ratings", type=pathlib.Path, default=None)
    ap.add_argument("--judge", action="store_true")
    ap.add_argument("--llm-url", default="http://127.0.0.1:8090")
    ap.add_argument("--verify-hash", type=pathlib.Path, default=None)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if args.verify_hash:
        ok, msg = verify_scorer_hash(args.verify_hash)
        if not ok:
            print(f"ERROR: {msg}", file=sys.stderr)
            return 2

    res = score_soak(
        args.trace_dir,
        run_stats={},
        human_dir=args.human_dir,
        baseline_trace_dir=args.baseline_dir,
        blind_ratings_path=args.blind_ratings,
        use_judge=args.judge,
        llm_url=args.llm_url,
    )
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print(format_score_report(res))
    return 0 if res["guardrails_ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
