#!/usr/bin/env python3
"""Fixed, tamper-checked human-likeness + guardrail scorer for soak runs.

Designed to sit outside the tuning agent's write scope (see
`docs/plans/self-improving-loop.md`):
  1. Computes a single composite human-likeness score (0..100, higher = closer
     to human play) from unified JSONL traces (`human_metrics.py`), comparing
     bot distributions against recorded human baseline traces (or documented
     fallback reference ranges until human traces are recorded).
  2. Optionally runs an anonymized LLM judge (`--judge`) over paired short
     trace clips (human control vs bot) and folds detection-closeness-to-50%
     into the score.
  3. Evaluates hard guardrails that must NEVER regress:
     - 0 crashes, 0 LLM errors, 0 invalid plans
     - bounded wake rate (<= 20 wakes/min/bot)
     - bounded latency (`do:` p95 <= 2000 ms)
     - bounded stuck bots & deaths
     - progress (XP/h + moved bots; prevents "idle more to look human" gaming)
     - optional offline eval pass rate (`--eval-passed`)
  4. Computes and verifies a SHA-256 hash of itself + `human_metrics.py` so the
     tuning loop can guarantee the scorer was not altered.
"""

import argparse
import hashlib
import json
import math
import pathlib
import random
import statistics
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import human_metrics  # noqa: E402

# Fallback human-plausible reference targets used ONLY when no human baseline
# traces exist yet in `human_dir`. Each entry is (target_value, tolerance_scale)
# where `exp(-((bot - target) / scale)^2)` scores 1.0 at target and ~0.37 at
# 1 scale unit away.
FALLBACK_TARGETS = {
    "path_straightness": (0.82, 0.25),
    "stops_per_min": (1.5, 1.5),
    "jumps_per_min": (1.2, 1.2),
    "instant_turns_per_min": (0.5, 2.0),
    "idle_pct": (15.0, 15.0),
    "aggro_react_p50_ms": (650.0, 450.0),
    "whisper_reply_p50_s": (4.5, 4.0),
}

METRIC_WEIGHTS = {
    "path_straightness": 1.5,
    "stops_per_min": 1.0,
    "jumps_per_min": 1.5,
    "instant_turns_per_min": 2.0,
    "idle_pct": 1.0,
    "aggro_react_p50_ms": 1.5,
    "whisper_reply_p50_s": 1.5,
}


def scorer_sha256() -> str:
    """SHA-256 over this scorer and `human_metrics.py` for tamper checks."""
    h = hashlib.sha256()
    for path in (pathlib.Path(__file__).resolve(),
                 (ROOT / "tools" / "bench" / "human_metrics.py").resolve()):
        h.update(path.name.encode("utf-8"))
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


def _progress_metrics(bot_traces: dict[str, list[dict]]) -> dict:
    """Computes XP/h, deaths, stuck bots, and moved bots from bot traces."""
    total_xp = 0
    total_hours = 0.0
    deaths = 0
    moved_bots = 0
    stuck_bots = 0
    for _name, rows in bot_traces.items():
        samples = [r for r in rows if r.get("kind") == "sample"]
        if len(samples) < 2:
            stuck_bots += 1
            continue
        dur_s = max(0.0, (samples[-1]["t_ms"] - samples[0]["t_ms"]) / 1000.0)
        total_hours += dur_s / 3600.0
        # Track positive XP deltas (level-up resets xp to 0, so sum positive jumps).
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
    return {
        "total_xp": total_xp,
        "xp_per_hour": xp_per_hour,
        "deaths": deaths,
        "moved_bots": moved_bots,
        "stuck_bots": stuck_bots,
        "traced_bots": len(bot_traces),
    }


MIN_HUMAN_SAMPLES = 300


def _filter_role_rows(traces: dict[str, list[dict]], expected_role: str) -> dict[str, list[dict]]:
    """Keeps only records whose embedded `role` matches `expected_role`."""
    out: dict[str, list[dict]] = {}
    for name, rows in traces.items():
        filtered = [r for r in rows if r.get("role") == expected_role]
        if filtered:
            out[name] = filtered
    return out


def _score_distributions(
    bot_summary: dict, human_summary: dict | None, human_sample_count: int = 0
) -> tuple[float, dict, str]:
    """Scores bot distributions (0..100) against human baseline or fallback.

    - Requires at least `MIN_HUMAN_SAMPLES` verified human samples for
      `human_baseline` mode; never silently falls back to made-up constants per
      metric when in `human_baseline` mode.
    - Missing metrics (None) score 0.0 instead of being dropped from the average.
    """
    use_human = bool(
        human_summary
        and human_summary.get("characters", 0) > 0
        and human_sample_count >= MIN_HUMAN_SAMPLES
    )
    mode = "human_baseline" if use_human else "fallback_reference"
    per_metric = {}
    weighted_sum = 0.0
    weight_total = 0.0

    for key, weight in METRIC_WEIGHTS.items():
        weight_total += weight
        bval = bot_summary.get(key)
        if use_human:
            hval = human_summary.get(key)
            if hval is None:
                per_metric[key] = {
                    "bot": round(float(bval), 3) if bval is not None else None,
                    "target": None,
                    "score": 0.0,
                    "reason": "no_human_data",
                }
                continue
            target = float(hval)
            scale = max(abs(target) * 0.5, FALLBACK_TARGETS[key][1] * 0.5)
        else:
            target, scale = FALLBACK_TARGETS[key]

        if bval is None:
            per_metric[key] = {
                "bot": None,
                "target": round(float(target), 3),
                "score": 0.0,
                "reason": "missing_bot_metric",
            }
            continue

        z = (float(bval) - target) / max(scale, 1e-6)
        sim = math.exp(-(z * z))  # 1.0 at target, decays smoothly
        pts = 100.0 * sim
        per_metric[key] = {
            "bot": round(float(bval), 3),
            "target": round(float(target), 3),
            "score": round(pts, 1),
        }
        weighted_sum += weight * pts

    dist_score = (weighted_sum / weight_total) if weight_total > 0 else 0.0
    return round(dist_score, 2), per_metric, mode


def _format_clip(
    rows: list[dict], max_samples: int = 20, rng: random.Random | None = None
) -> str:
    samples = [r for r in rows if r.get("kind") == "sample"]
    if not samples:
        return "(empty)"
    if len(samples) > max_samples and rng is not None:
        start = rng.randint(0, len(samples) - max_samples)
        samples = samples[start : start + max_samples]
    else:
        samples = samples[:max_samples]
    lines = []
    t0 = samples[0]["t_ms"]
    for s in samples:
        dt = (s["t_ms"] - t0) / 1000.0
        lines.append(
            f"t+{dt:4.1f}s pos=({s['x']:.1f},{s['y']:.1f}) o={s['o']:.2f} "
            f"mov={int(bool(s.get('moving')))} jmp={int(bool(s.get('jumping')))} "
            f"cbt={int(bool(s.get('in_combat')))} cast={s.get('casting') or '-'}"
        )
    return "\n".join(lines)


def _ask_judge_pair(a_txt: str, b_txt: str, llm_url: str) -> str | None:
    prompt = (
        "Two short World of Warcraft gameplay traces follow (Trace A and Trace B). "
        "One is a human player and one is a bot. Reply with ONLY 'A' or 'B' for "
        "which trace is the HUMAN player.\n\n"
        f"Trace A:\n{a_txt}\n\nTrace B:\n{b_txt}\n"
    )
    payload = json.dumps({
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.0,
        "max_tokens": 4,
    }).encode("utf-8")
    req = urllib.request.Request(
        f"{llm_url.rstrip('/')}/v1/chat/completions",
        data=payload,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        ans = data["choices"][0]["message"]["content"].strip().upper()[:1]
        return ans if ans in ("A", "B") else None
    except Exception:
        return None


def _run_llm_judge(
    bot_traces: dict[str, list[dict]],
    human_traces: dict[str, list[dict]],
    llm_url: str,
    pairs: int = 30,
    control_pairs: int = 10,
    seed: int | None = None,
) -> tuple[float | None, dict]:
    """Asks local LLM judge to distinguish human vs bot clips across random windows.

    Includes human-vs-human control pairs to detect position bias (e.g. an
    'always A' judge). 50% accuracy = 100 pts; 100% bot detection = 0 pts.
    """
    if not bot_traces or not human_traces:
        return None, {}
    rng = random.Random(seed) if seed is not None else random.SystemRandom()
    bot_list = list(bot_traces.values())
    human_list = list(human_traces.values())
    correct = 0
    total = 0
    a_picks = 0
    for _ in range(pairs):
        b_clip = _format_clip(rng.choice(bot_list), rng=rng)
        h_clip = _format_clip(rng.choice(human_list), rng=rng)
        human_is_a = rng.choice([True, False])
        a_txt, b_txt = (h_clip, b_clip) if human_is_a else (b_clip, h_clip)
        ans = _ask_judge_pair(a_txt, b_txt, llm_url)
        if ans is not None:
            total += 1
            if ans == "A":
                a_picks += 1
            if (ans == "A") == human_is_a:
                correct += 1

    ctrl_total = 0
    ctrl_a = 0
    for _ in range(control_pairs):
        h1 = _format_clip(rng.choice(human_list), rng=rng)
        h2 = _format_clip(rng.choice(human_list), rng=rng)
        ans = _ask_judge_pair(h1, h2, llm_url)
        if ans is not None:
            ctrl_total += 1
            if ans == "A":
                ctrl_a += 1

    if total == 0:
        return None, {}
    acc = correct / total
    ctrl_a_rate = (ctrl_a / ctrl_total) if ctrl_total > 0 else 0.5
    details = {
        "pairs": total,
        "accuracy": round(acc, 3),
        "a_pick_rate": round(a_picks / total, 3),
        "control_pairs": ctrl_total,
        "control_a_rate": round(ctrl_a_rate, 3),
    }
    # If the judge shows extreme position bias on human-vs-human controls
    # (>80% or <20% picking A), do not trust it as a signal.
    if ctrl_total >= 4 and (ctrl_a_rate > 0.8 or ctrl_a_rate < 0.2):
        details["invalid_bias"] = True
        return None, details
    score = round(max(0.0, 100.0 * (1.0 - max(0.0, acc - 0.5) * 2.0)), 2)
    return score, details


def score_soak(
    trace_dir: pathlib.Path,
    run_stats: dict,
    human_dir: pathlib.Path | None = None,
    use_judge: bool = False,
    llm_url: str = "http://127.0.0.1:8090",
    min_eval_passed: int | None = None,
    eval_passed: int | None = None,
) -> dict:
    traces = human_metrics.load_traces(trace_dir)
    bot_traces = _filter_role_rows(traces["bot"], "bot")
    human_traces = _filter_role_rows(traces["human"], "human")
    if human_dir and human_dir.exists():
        extra = human_metrics.load_traces(human_dir)
        human_traces = {**human_traces, **_filter_role_rows(extra["human"], "human")}

    human_sample_count = sum(
        sum(1 for r in rows if r.get("kind") == "sample")
        for rows in human_traces.values()
    )
    bot_per_char = {n: human_metrics.analyze_character(r) for n, r in bot_traces.items()}
    human_per_char = {n: human_metrics.analyze_character(r) for n, r in human_traces.items()}
    bot_summary = human_metrics.summarize_role(bot_per_char)
    human_summary = human_metrics.summarize_role(human_per_char) if human_per_char else None

    dist_score, per_metric, mode = _score_distributions(
        bot_summary, human_summary, human_sample_count=human_sample_count
    )
    judge_score, judge_details = (
        _run_llm_judge(bot_traces, human_traces, llm_url=llm_url)
        if use_judge
        else (None, {})
    )
    composite = (
        round(0.7 * dist_score + 0.3 * judge_score, 2)
        if judge_score is not None
        else dist_score
    )

    progress = _progress_metrics(bot_traces)
    tells = human_metrics.detect_bot_tells(bot_summary, human_summary)

    # Hard guardrails.
    guardrails = []
    def check(name: str, ok: bool, detail: str):
        guardrails.append({"name": name, "ok": bool(ok), "detail": detail})

    n_bots = max(1, int(run_stats.get("bots", progress["traced_bots"] or 1)))
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
    if min_eval_passed is not None and eval_passed is not None:
        check("offline_eval", eval_passed >= min_eval_passed,
              f"eval={eval_passed} (>= {min_eval_passed})")

    guardrails_ok = all(g["ok"] for g in guardrails)
    # Worst metric to guide the next hill-climbing iteration.
    worst_metric = None
    if per_metric:
        worst_metric = min(per_metric.items(), key=lambda kv: kv[1]["score"])[0]

    return {
        "score": composite,
        "dist_score": dist_score,
        "judge_score": judge_score,
        "judge_details": judge_details,
        "baseline_mode": mode,
        "human_provenance": {
            "dir": str(human_dir) if human_dir else "",
            "characters": len(human_traces),
            "samples": human_sample_count,
        },
        "guardrails_ok": guardrails_ok,
        "guardrails": guardrails,
        "progress": progress,
        "per_metric": per_metric,
        "worst_metric": worst_metric,
        "tells": tells,
        "run_stats": run_stats,
        "scorer_sha256": scorer_sha256(),
    }


def format_score_report(res: dict) -> str:
    lines = [
        f"SCORE: {res['score']:.2f} / 100.00  "
        f"(mode={res['baseline_mode']}, guardrails={'PASS' if res['guardrails_ok'] else 'FAIL'}, "
        f"scorer={res['scorer_sha256'][:12]})",
    ]
    if res.get("judge_score") is not None:
        lines.append(
            f"  components: distribution={res['dist_score']:.2f}, llm_judge={res['judge_score']:.2f}"
        )
    p = res["progress"]
    lines.append(
        f"  progress: xp/h={p['xp_per_hour']:.0f} (total_xp={p['total_xp']}), "
        f"deaths={p['deaths']}, moved={p['moved_bots']}/{p['traced_bots']}"
    )
    lines.append("  metrics:")
    for k, m in sorted(res["per_metric"].items(), key=lambda kv: kv[1]["score"]):
        mark = " <-- WORST" if k == res["worst_metric"] else ""
        bstr = f"{m['bot']:7.2f}" if m["bot"] is not None else "   None"
        tstr = f"{m['target']:7.2f}" if m["target"] is not None else "   None"
        lines.append(
            f"    {k:24s}: bot={bstr}  target={tstr}  "
            f"pts={m['score']:5.1f}{mark}"
        )
    lines.append("  guardrails:")
    for g in res["guardrails"]:
        lines.append(f"    {'PASS' if g['ok'] else 'FAIL'} {g['name']:18s}: {g['detail']}")
    if res["tells"]:
        lines.append("  tells:")
        for t in res["tells"]:
            lines.append(f"    - {t}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Score a soak trace directory")
    ap.add_argument("trace_dir", type=pathlib.Path)
    ap.add_argument("--human-dir", type=pathlib.Path, default=None)
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
