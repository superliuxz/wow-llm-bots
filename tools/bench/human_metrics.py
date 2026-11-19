#!/usr/bin/env python3
"""Unified human-likeness behavior trace analyzer (docs/design.md, "Evals").

Reads unified JSONL traces written by `BehaviorTraceWriter` under:
  <trace_dir>/<role>/<name>/<YYYY-MM-DD>.jsonl   (role = "human" or "bot")

Computes per-role distributions for:
  - Reaction time to aggro (ms, p50/p95)
  - Whisper reply delay (s, p50), reply length (chars), lowercase/casual rate
  - Movement: path straightness (0..1), stops/min, jumps/min, turning speed
    (rad/s), instant-turn spikes (> 90 deg in one tick, per min)
  - Pacing: idle %, town %, session length (min)

When both `human` and `bot` traces exist, computes normalized distance from the
human baseline and flags concrete bot tells.
"""

import argparse
import json
import math
import pathlib
import statistics
import sys


def _angle_diff(a: float, b: float) -> float:
    d = abs(b - a) % (2.0 * math.pi)
    return min(d, 2.0 * math.pi - d)


def _pct(vals, q: float) -> float | None:
    if not vals:
        return None
    s = sorted(vals)
    idx = min(len(s) - 1, int(q * (len(s) - 1)))
    return float(s[idx])


def load_traces(trace_dir: pathlib.Path):
    """Returns {role: {name: [record, ...]}} sorted by monotonic_us / t_ms."""
    out = {"human": {}, "bot": {}}
    if not trace_dir.exists():
        return out
    for role in ("human", "bot"):
        rdir = trace_dir / role
        if not rdir.is_dir():
            continue
        for cdir in sorted(rdir.iterdir()):
            if not cdir.is_dir():
                continue
            rows = []
            for f in sorted(cdir.glob("*.jsonl")):
                for line in f.read_text().splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rows.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue
            if rows:
                rows.sort(key=lambda r: (r.get("t_ms", 0), r.get("mono_us", 0)))
                out[role][cdir.name] = rows
    return out


def analyze_character(rows: list[dict]) -> dict:
    samples = [r for r in rows if r.get("kind") == "sample"]
    chats = [r for r in rows if r.get("kind") == "chat"]

    if len(samples) < 2:
        return {"samples": len(samples), "duration_s": 0.0}

    t0 = samples[0]["t_ms"]
    t1 = samples[-1]["t_ms"]
    duration_s = max(0.0, (t1 - t0) / 1000.0)
    duration_min = max(duration_s / 60.0, 1e-6)

    # Movement: path straightness over moving segments, stops, jumps, turns.
    straightness_vals = []
    seg_start = None
    seg_len = 0.0
    stops = 0
    jumps = 0
    turn_rates = []
    instant_turns = 0
    idle_s = 0.0
    town_s = 0.0
    total_dt_s = 0.0

    # Aggro reaction: in_combat false -> true until casting or target change or move change.
    aggro_react_ms = []
    pending_aggro_t = None
    pending_aggro_target = None

    for i in range(1, len(samples)):
        prev = samples[i - 1]
        cur = samples[i]
        dt_s = (cur["t_ms"] - prev["t_ms"]) / 1000.0
        if dt_s <= 0.0 or dt_s > 10.0:
            # Session gap: close any open segment.
            if seg_start is not None and seg_len >= 5.0:
                net = math.hypot(prev["x"] - seg_start[0], prev["y"] - seg_start[1])
                straightness_vals.append(min(1.0, net / seg_len))
            seg_start = None
            seg_len = 0.0
            pending_aggro_t = None
            continue

        total_dt_s += dt_s
        step_d = math.hypot(cur["x"] - prev["x"], cur["y"] - prev["y"])
        is_moving = bool(cur.get("moving")) or step_d >= 0.4
        was_moving = bool(prev.get("moving"))

        if is_moving:
            if seg_start is None:
                seg_start = (prev["x"], prev["y"])
                seg_len = 0.0
            seg_len += step_d
        else:
            if was_moving:
                stops += 1
            if seg_start is not None and seg_len >= 5.0:
                net = math.hypot(prev["x"] - seg_start[0], prev["y"] - seg_start[1])
                straightness_vals.append(min(1.0, net / seg_len))
            seg_start = None
            seg_len = 0.0

        if cur.get("jumping") and not prev.get("jumping"):
            jumps += 1

        da = _angle_diff(float(prev.get("o", 0.0)), float(cur.get("o", 0.0)))
        if da > 0.05:
            turn_rates.append(da / dt_s)
        if da > (math.pi / 2.0):
            instant_turns += 1

        if not is_moving and not cur.get("in_combat") and not cur.get("casting"):
            idle_s += dt_s
        if cur.get("in_town"):
            town_s += dt_s

        # Aggro reaction timing.
        if cur.get("in_combat") and not prev.get("in_combat"):
            pending_aggro_t = cur["t_ms"]
            pending_aggro_target = cur.get("target", "")
        elif pending_aggro_t is not None:
            if not cur.get("in_combat"):
                pending_aggro_t = None
            elif (
                cur.get("casting")
                or (cur.get("target") and cur.get("target") != pending_aggro_target)
                or step_d >= 1.0
            ):
                aggro_react_ms.append(float(cur["t_ms"] - pending_aggro_t))
                pending_aggro_t = None

    if seg_start is not None and seg_len >= 5.0:
        last = samples[-1]
        net = math.hypot(last["x"] - seg_start[0], last["y"] - seg_start[1])
        straightness_vals.append(min(1.0, net / seg_len))

    # Chat metrics: whisper reply delay, length, casual/lowercase rate.
    reply_delays_s = []
    reply_lengths = []
    lowercase_replies = 0
    pending_whisper_in = {}
    for c in chats:
        if c.get("channel") != "whisper":
            continue
        peer = c.get("peer", "")
        if c.get("dir") == "in":
            pending_whisper_in[peer] = c["t_ms"]
        elif c.get("dir") == "out":
            text = c.get("text", "")
            reply_lengths.append(len(text))
            if text and text[0].islower():
                lowercase_replies += 1
            if peer in pending_whisper_in:
                delay = (c["t_ms"] - pending_whisper_in.pop(peer)) / 1000.0
                if 0.0 <= delay <= 120.0:
                    reply_delays_s.append(delay)

    return {
        "samples": len(samples),
        "duration_s": duration_s,
        "path_straightness": statistics.mean(straightness_vals) if straightness_vals else None,
        "stops_per_min": stops / duration_min,
        "jumps_per_min": jumps / duration_min,
        "turn_rad_per_s": statistics.median(turn_rates) if turn_rates else 0.0,
        "instant_turns_per_min": instant_turns / duration_min,
        "idle_pct": (100.0 * idle_s / total_dt_s) if total_dt_s > 0 else 0.0,
        "town_pct": (100.0 * town_s / total_dt_s) if total_dt_s > 0 else 0.0,
        "aggro_react_p50_ms": _pct(aggro_react_ms, 0.5),
        "aggro_react_p95_ms": _pct(aggro_react_ms, 0.95),
        "whisper_reply_p50_s": _pct(reply_delays_s, 0.5),
        "whisper_len_mean": statistics.mean(reply_lengths) if reply_lengths else None,
        "whisper_lowercase_pct": (
            100.0 * lowercase_replies / len(reply_lengths) if reply_lengths else None
        ),
    }


def summarize_role(per_char: dict[str, dict]) -> dict:
    keys = [
        "path_straightness",
        "stops_per_min",
        "jumps_per_min",
        "turn_rad_per_s",
        "instant_turns_per_min",
        "idle_pct",
        "town_pct",
        "aggro_react_p50_ms",
        "whisper_reply_p50_s",
        "whisper_len_mean",
    ]
    out = {"characters": len(per_char), "total_samples": sum(c["samples"] for c in per_char.values())}
    for k in keys:
        vals = [c[k] for c in per_char.values() if c.get(k) is not None]
        out[k] = statistics.mean(vals) if vals else None
    return out


def detect_bot_tells(bot_summary: dict, human_summary: dict | None = None) -> list[str]:
    tells = []
    ps = bot_summary.get("path_straightness")
    if ps is not None and ps >= 0.98:
        tells.append(f"robotic straight-line pathing (straightness={ps:.3f})")
    jpm = bot_summary.get("jumps_per_min")
    if jpm is not None and jpm == 0.0 and bot_summary.get("total_samples", 0) >= 60:
        tells.append("zero jumps across entire trace (0.0 jumps/min)")
    itpm = bot_summary.get("instant_turns_per_min")
    if itpm is not None and itpm >= 4.0:
        tells.append(f"frequent instant >90 deg turns ({itpm:.1f}/min)")
    if human_summary and human_summary.get("characters", 0) > 0:
        for k, label in (
            ("path_straightness", "path straightness"),
            ("jumps_per_min", "jumps/min"),
            ("idle_pct", "idle %"),
            ("aggro_react_p50_ms", "aggro reaction ms"),
            ("whisper_reply_p50_s", "whisper reply delay s"),
        ):
            bv = bot_summary.get(k)
            hv = human_summary.get(k)
            if bv is not None and hv is not None and hv > 0:
                ratio = abs(bv - hv) / hv
                if ratio > 0.5:
                    tells.append(f"{label} drifts from human baseline (bot={bv:.2f} vs human={hv:.2f})")
    return tells


def format_report(trace_dir: pathlib.Path) -> str:
    traces = load_traces(trace_dir)
    lines = [f"== Behavior trace report ({trace_dir}) =="]
    summaries = {}
    for role in ("human", "bot"):
        per_char = {n: analyze_character(rows) for n, rows in traces[role].items()}
        summary = summarize_role(per_char)
        summaries[role] = summary
        if summary["characters"] == 0:
            lines.append(f"  {role}: no traces yet")
            continue
        fmt = lambda v, u="": f"{v:.2f}{u}" if v is not None else "-"
        lines.append(
            f"  {role:5s} ({summary['characters']} chars, {summary['total_samples']} samples): "
            f"straightness={fmt(summary['path_straightness'])}, "
            f"stops={fmt(summary['stops_per_min'], '/m')}, "
            f"jumps={fmt(summary['jumps_per_min'], '/m')}, "
            f"instant_turns={fmt(summary['instant_turns_per_min'], '/m')}, "
            f"idle={fmt(summary['idle_pct'], '%')}, "
            f"town={fmt(summary['town_pct'], '%')}, "
            f"aggro_p50={fmt(summary['aggro_react_p50_ms'], 'ms')}, "
            f"whisper_p50={fmt(summary['whisper_reply_p50_s'], 's')}"
        )
    tells = detect_bot_tells(summaries["bot"], summaries["human"])
    if tells:
        lines.append("  Bot tells flagged:")
        for t in tells:
            lines.append(f"    - {t}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze unified human/bot behavior traces")
    ap.add_argument("trace_dir", type=pathlib.Path, help="Directory with human/ and bot/ JSONL traces")
    args = ap.parse_args()
    print(format_report(args.trace_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
