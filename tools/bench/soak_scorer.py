#!/usr/bin/env python3
"""Plain soak checker: stalls, deaths, XP/h, wake loops, latency, and errors.

The parked human-likeness scorer (Layers B/C, pairwise ranking, and scorer lock)
lives in `experimental/human_likeness/layered_scorer.py`.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
for p in (ROOT / "tools", ROOT / "tools" / "bench"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import chatter_check  # noqa: E402
import stall_check  # noqa: E402
from wlblib import trace as trace_lib  # noqa: E402

MAX_REPEATED_REFUSALS = 5
DEFAULT_MAX_STALL_S = 300.0
DEFAULT_STALL_RADIUS_YD = 15.0
DEFAULT_MAX_CHATTER_PER_HOUR = chatter_check.DEFAULT_MAX_CHATTER_PER_HOUR


def peak_per_minute(times_ms: list[Any]) -> int:
    """The most timestamps (ms) that fall in any 60 s window."""
    ts = sorted(int(t) for t in times_ms if isinstance(t, (int, float)))
    best = 0
    lo = 0
    for hi, t in enumerate(ts):
        while t - ts[lo] >= 60_000:
            lo += 1
        best = max(best, hi - lo + 1)
    return best


def _bot_xp_gained(rows: list[dict[str, Any]], samples: list[dict[str, Any]]) -> int:
    """Computes XP earned without dropping gains across level-ups.

    Uses exact `xp_gain` trace events when present; otherwise computes sample
    deltas and counts rollover XP whenever `level` increases across a drop in
    the current-level XP bar.
    """
    event_xp = sum(
        max(0, int(r.get("value", 0)))
        for r in rows
        if r.get("kind") == "event" and r.get("event") == "xp_gain"
    )
    if event_xp > 0:
        return event_xp

    sample_xp = 0
    for i in range(1, len(samples)):
        prev_s, cur_s = samples[i - 1], samples[i]
        prev_xp = int(prev_s.get("xp", 0))
        cur_xp = int(cur_s.get("xp", 0))
        prev_lvl = int(prev_s.get("level", 0))
        cur_lvl = int(cur_s.get("level", 0))
        if cur_lvl > prev_lvl:
            # XP bar reset on level-up: keep at least the new-level XP (plus
            # remaining bar of previous level when `xp_max` is recorded).
            prev_max = int(prev_s.get("xp_max", 0))
            tail = max(0, prev_max - prev_xp) if prev_max > prev_xp else 0
            sample_xp += tail + max(0, cur_xp)
        elif cur_xp > prev_xp:
            sample_xp += cur_xp - prev_xp
    return sample_xp


def _progress_metrics(
    bot_traces: dict[str, list[dict[str, Any]]],
    *,
    max_stall_s: float = DEFAULT_MAX_STALL_S,
    stall_radius_yd: float = DEFAULT_STALL_RADIUS_YD,
) -> dict[str, Any]:
    """Computes XP/h, quests turned in, deaths, real stalls, and moved bots."""
    total_xp = 0
    total_hours = 0.0
    deaths = 0
    quests_completed = 0
    moved_bots = 0
    stuck_bots = 0
    stalled_bots = 0
    longest_stall_s = 0.0
    stall_spans: dict[str, list[dict[str, Any]]] = {}

    for name, rows in bot_traces.items():
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
            if not samples[i - 1].get("dead") and samples[i].get("dead"):
                deaths += 1
        total_xp += _bot_xp_gained(rows, samples)

        x0, y0 = samples[0]["x"], samples[0]["y"]
        max_d = max(math.hypot(s["x"] - x0, s["y"] - y0) for s in samples)
        spans = stall_check.check_bot_records(
            rows, max_stall_s=max_stall_s, radius_yd=stall_radius_yd
        )
        if spans:
            stalled_bots += 1
            stall_spans[name] = spans
            longest_stall_s = max(
                longest_stall_s, max(float(sp["duration_s"]) for sp in spans)
            )
        if max_d >= 3.0:
            moved_bots += 1
        if max_d < 3.0 or spans:
            stuck_bots += 1

    xp_per_hour = (total_xp / total_hours) if total_hours > 0 else 0.0
    quests_per_hour = (quests_completed / total_hours) if total_hours > 0 else 0.0
    return {
        "total_xp": total_xp,
        "total_hours": round(total_hours, 4),
        "xp_per_hour": round(xp_per_hour, 1),
        "quests_completed": quests_completed,
        "quests_per_hour": round(quests_per_hour, 2),
        "deaths": deaths,
        "moved_bots": moved_bots,
        "stalled_bots": stalled_bots,
        "stuck_bots": stuck_bots,
        "max_stall_s": round(longest_stall_s, 1),
        "stalls": stall_spans,
        "traced_bots": len(bot_traces),
    }


def _resolve_decisions_dirs(
    decisions_dirs: list[pathlib.Path] | None,
    run_stats: dict[str, Any],
) -> list[pathlib.Path]:
    raw: list[pathlib.Path | str] = []
    if decisions_dirs is not None:
        raw.extend(decisions_dirs)
    else:
        if run_stats.get("decisions_dirs"):
            raw.extend(run_stats["decisions_dirs"])
        elif run_stats.get("decisions_dir"):
            raw.append(run_stats["decisions_dir"])
    resolved: list[pathlib.Path] = []
    for entry in raw:
        p = pathlib.Path(entry)
        if not p.is_absolute():
            p = ROOT / p
        if p.exists():
            resolved.append(p)
    return resolved


def score_soak(
    trace_dir: pathlib.Path,
    run_stats: dict[str, Any],
    *,
    min_eval_passed: int | None = None,
    eval_passed: int | None = None,
    max_stall_s: float = DEFAULT_MAX_STALL_S,
    stall_radius_yd: float = DEFAULT_STALL_RADIUS_YD,
    decisions_dirs: list[pathlib.Path] | None = None,
    max_chatter_per_hour: float = DEFAULT_MAX_CHATTER_PER_HOUR,
    chatter_judge: chatter_check.JudgeFn | None = None,
) -> dict[str, Any]:
    """Evaluates plain soak health gates (stalls, deaths, XP/h, wake loops, latency)."""
    traces = trace_lib.load_traces(trace_dir)
    bot_traces = trace_lib.filter_role_rows(traces["bot"], "bot")
    progress = _progress_metrics(
        bot_traces, max_stall_s=max_stall_s, stall_radius_yd=stall_radius_yd
    )

    guardrails: list[dict[str, Any]] = []

    def check(name: str, ok: bool, detail: str) -> None:
        guardrails.append({"name": name, "ok": bool(ok), "detail": detail})

    n_bots = max(1, int(run_stats.get("bots", progress["traced_bots"] or 1)))
    min_traced = max(1, n_bots // 2)
    check(
        "has_traced_bots",
        progress["traced_bots"] >= min_traced,
        f"traced_bots={progress['traced_bots']} (>= {min_traced})",
    )
    check(
        "has_decisions",
        int(run_stats.get("decisions", 0)) >= 1,
        f"decisions={run_stats.get('decisions', 0)} (>= 1)",
    )
    check(
        "no_llm_errors",
        run_stats.get("llm_errors", 0) == 0,
        f"llm_errors={run_stats.get('llm_errors', 0)}",
    )
    check(
        "no_invalid_plans",
        run_stats.get("invalid_plans", 0) == 0,
        f"invalid_plans={run_stats.get('invalid_plans', 0)}",
    )
    # The busiest minute (soak.py writes it), not the run's average.
    wake_rate = float(run_stats.get("peak_wakes_per_min", 0.0))
    check(
        "no_wake_loop",
        wake_rate <= 20.0,
        f"wakes/min/bot={wake_rate:.1f} (<= 20.0)",
    )
    check(
        "latency_p95",
        run_stats.get("do_p95_ms", 0.0) <= 2000.0,
        f"do_p95={run_stats.get('do_p95_ms', 0.0):.0f}ms (<= 2000ms)",
    )
    max_stuck = max(1, int(math.floor(n_bots * 0.3)))
    check(
        "not_stuck",
        progress["stuck_bots"] <= max_stuck,
        f"stuck={progress['stuck_bots']}/{progress['traced_bots']} "
        f"(stalled={progress['stalled_bots']}, max_stall={progress['max_stall_s']}s, <= {max_stuck})",
    )
    check(
        "deaths_bounded",
        progress["deaths"] <= n_bots,
        f"deaths={progress['deaths']} (<= {n_bots})",
    )
    max_refusals = int(run_stats.get("max_repeated_refusals", 0))
    check(
        "no_refusal_loop",
        max_refusals <= MAX_REPEATED_REFUSALS,
        f"max_repeated_refusals={max_refusals} (<= {MAX_REPEATED_REFUSALS})",
    )
    chatter: dict[str, Any] | None = None
    resolved_dec_dirs = _resolve_decisions_dirs(decisions_dirs, run_stats)
    if resolved_dec_dirs:
        chatter_hours: float | None = None
        if progress["total_hours"] > 0.0:
            chatter_hours = float(progress["total_hours"])
        elif float(run_stats.get("elapsed_min", 0.0)) > 0.0:
            chatter_hours = (float(run_stats["elapsed_min"]) / 60.0) * n_bots
        chatter = chatter_check.check_decision_dirs(
            resolved_dec_dirs,
            duration_hours=chatter_hours,
            max_per_hour=max_chatter_per_hour,
            judge=chatter_judge,
        )
        c = chatter["counts"]
        check(
            "chatter_bounded",
            chatter["ok"],
            f"flagged/h={chatter['flagged_per_hour']:.2f} (<= {chatter['max_per_hour']:.2f}; "
            f"rep={c.get('repeated', 0)}, nar={c.get('narrating', 0)}, "
            f"syc={c.get('sycophantic', 0)}, total={chatter['total_lines']})",
        )
    if min_eval_passed is not None and eval_passed is not None:
        check(
            "offline_eval",
            eval_passed >= min_eval_passed,
            f"eval={eval_passed} (>= {min_eval_passed})",
        )

    out: dict[str, Any] = {
        "guardrails_ok": all(g["ok"] for g in guardrails),
        "guardrails": guardrails,
        "progress": progress,
        "run_stats": run_stats,
    }
    if chatter is not None:
        out["chatter"] = chatter
    return out


def format_score_report(res: dict[str, Any]) -> str:
    p = res["progress"]
    lines = [
        f"SOAK GATES: {'PASS' if res['guardrails_ok'] else 'FAIL'}",
        f"  progress: xp/h={p['xp_per_hour']:.0f} (total_xp={p['total_xp']}), "
        f"quests={p['quests_completed']}, deaths={p['deaths']}, "
        f"moved={p['moved_bots']}/{p['traced_bots']}, "
        f"stalled={p['stalled_bots']} (max={p['max_stall_s']}s)",
    ]
    if res.get("chatter") is not None:
        ch = res["chatter"]
        c = ch["counts"]
        lines.append(
            f"  chatter: flagged/h={ch['flagged_per_hour']:.2f} (<= {ch['max_per_hour']:.2f}), "
            f"flagged={ch['flagged_lines']}/{ch['total_lines']} "
            f"(rep={c.get('repeated', 0)}, nar={c.get('narrating', 0)}, "
            f"syc={c.get('sycophantic', 0)})"
        )
    lines.append("  Gates:")
    for g in res["guardrails"]:
        lines.append(f"    {'PASS' if g['ok'] else 'FAIL'} {g['name']:22s}: {g['detail']}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Score a soak trace directory (plain soak checks)")
    ap.add_argument("trace_dir", type=pathlib.Path)
    ap.add_argument("--decisions-dir", action="append", type=pathlib.Path, default=None)
    ap.add_argument("--max-stall-s", type=float, default=DEFAULT_MAX_STALL_S)
    ap.add_argument("--max-chatter-per-hour", type=float, default=DEFAULT_MAX_CHATTER_PER_HOUR)
    ap.add_argument("--chatter-llm-judge", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    judge = chatter_check.make_llm_judge() if args.chatter_llm_judge else None
    res = score_soak(
        args.trace_dir,
        run_stats={},
        max_stall_s=args.max_stall_s,
        decisions_dirs=args.decisions_dir,
        max_chatter_per_hour=args.max_chatter_per_hour,
        chatter_judge=judge,
    )
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print(format_score_report(res))
    return 0 if res["guardrails_ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
