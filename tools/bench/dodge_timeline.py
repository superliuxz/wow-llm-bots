#!/usr/bin/env python3
"""Boss cast / dodge timeline analyzer from unified behavior traces.

Reads `<trace_dir>/bot/<Name>/*.jsonl` and, for every boss spell cast observed
in `target_casting`, reports per-bot reaction chains using DBC-sourced spell
radius (`target_cast_radius`) and cast origin `(target_x, target_y)`:
  cast start -> dodge/movement act_sent delay -> stationary->moving transition
  -> Euclidean distance to cast origin at impact vs DBC radius -> impact dmg.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import trace_parity  # noqa: E402

DODGE_ACT_VERBS = ("AWAY", "AVOID", "TO", "KITE", "STOP")


def _is_dodge_act(detail: str) -> bool:
    verb = detail.strip().split(" ", 1)[0].upper() if detail else ""
    return verb in DODGE_ACT_VERBS


def extract_dodge_timelines(
    traces: dict[str, list[dict[str, Any]]],
    spell_filter: str = "",
    radius_override: float | None = None,
) -> dict[str, Any]:
    """Finds boss casts seen via `target_casting` and measures each bot's reaction."""
    # Each raw window: (t_start, t_end, spell_name, radius_yd, cast_x, cast_y)
    raw_windows: list[list[Any]] = []
    act_counts: dict[str, int] = {}
    cbt_t0: int | None = None
    cbt_t1: int | None = None

    for name, rows in sorted(traces.items()):
        cur: list[Any] | None = None
        acts = 0
        for r in rows:
            k = r.get("kind")
            t = int(r.get("t_ms", 0))
            if k == "event" and r.get("event") == "act_sent":
                acts += 1
            if k != "sample":
                continue
            if r.get("in_combat"):
                cbt_t0 = t if cbt_t0 is None else min(cbt_t0, t)
                cbt_t1 = t if cbt_t1 is None else max(cbt_t1, t)
            sp = str(r.get("target_casting", ""))
            if sp and (not spell_filter or spell_filter.lower() in sp.lower()):
                rad = float(r.get("target_cast_radius", -1.0))
                tx = float(r["target_x"]) if "target_x" in r else None
                ty = float(r["target_y"]) if "target_y" in r else None
                if cur is None or cur[2] != sp or t - cur[1] > 1500:
                    if cur is not None:
                        raw_windows.append(cur)
                    cur = [t, t, sp, rad if rad > 0 else None, tx, ty]
                else:
                    cur[1] = t
                    if rad > 0:
                        cur[3] = rad
                    if tx is not None and ty is not None:
                        cur[4], cur[5] = tx, ty
            elif cur is not None and t - cur[1] > 600:
                raw_windows.append(cur)
                cur = None
        if cur is not None:
            raw_windows.append(cur)
        act_counts[name] = acts

    fight_min = max(1.0 / 60.0, ((cbt_t1 - cbt_t0) / 60000.0)) if (cbt_t0 and cbt_t1 and cbt_t1 > cbt_t0) else 1.0
    acts_per_min = {name: round(cnt / fight_min, 1) for name, cnt in act_counts.items()}

    raw_windows.sort(key=lambda w: (w[0], w[1]))
    merged: list[list[Any]] = []
    for w in raw_windows:
        if merged and merged[-1][2] == w[2] and w[0] <= merged[-1][1] + 1000:
            merged[-1][1] = max(merged[-1][1], w[1])
            if w[3] is not None:
                merged[-1][3] = w[3]
            if w[4] is not None and w[5] is not None:
                merged[-1][4], merged[-1][5] = w[4], w[5]
        else:
            merged.append(list(w))

    windows: list[dict[str, Any]] = []
    for s0, s1, sp, dbc_radius, cx, cy in merged:
        radius = radius_override if radius_override is not None else dbc_radius
        per_bot: dict[str, dict[str, Any]] = {}
        for name, rows in sorted(traces.items()):
            samples = [
                r for r in rows
                if r.get("kind") == "sample" and s0 - 600 <= int(r["t_ms"]) <= s1 + 1500
            ]
            events_during = [
                r for r in rows
                if r.get("kind") == "event" and s0 - 200 <= int(r["t_ms"]) <= s1 + 200
            ]
            events_impact = [
                r for r in rows
                if r.get("kind") == "event" and s1 - 500 <= int(r["t_ms"]) <= s1 + 1500
            ]
            during = [r for r in samples if s0 <= int(r["t_ms"]) <= s1 + 200]

            # Measure Euclidean distance to the cast origin `(cx, cy)` when available
            # (works even if a bot wasn't targeting the boss!); fall back to target_dist.
            dists: list[float] = []
            for r in during:
                if cx is not None and cy is not None and "x" in r and "y" in r:
                    dists.append(math.hypot(float(r["x"]) - cx, float(r["y"]) - cy))
                elif float(r.get("target_dist", -1)) >= 0:
                    dists.append(float(r["target_dist"]))
            start_dist = dists[0] if dists else None
            end_dist = dists[-1] if dists else None
            max_dist = max(dists) if dists else None

            pre = [r for r in samples if s0 - 400 <= int(r["t_ms"]) < s0]
            was_moving = bool(pre[-1].get("moving")) if pre else False
            move_delay_ms: int | str | None = None
            if was_moving and during and bool(during[0].get("moving")):
                move_delay_ms = "already"
            else:
                for r in during:
                    if r.get("moving"):
                        move_delay_ms = int(r["t_ms"]) - s0
                        break

            # Tie act_delay_ms to dodge/movement acts, not unrelated TARGET/USE acts.
            dodge_act = next(
                (e for e in events_during if e.get("event") == "act_sent" and _is_dodge_act(str(e.get("detail", "")))),
                None,
            )
            act_delay_ms = (int(dodge_act["t_ms"]) - s0) if dodge_act else None
            act_detail = str(dodge_act.get("detail", "")) if dodge_act else ""

            impact_dmg = sum(
                int(e.get("value", 0)) for e in events_impact if e.get("event") == "damage_taken"
            )
            died = any(e.get("event") == "died" for e in events_impact)
            outside = (end_dist >= radius) if (radius is not None and end_dist is not None) else None

            per_bot[name] = {
                "start_yd": round(start_dist, 1) if start_dist is not None else None,
                "end_yd": round(end_dist, 1) if end_dist is not None else None,
                "max_yd": round(max_dist, 1) if max_dist is not None else None,
                "outside_at_impact": outside,
                "act_delay_ms": act_delay_ms,
                "act_detail": act_detail,
                "move_delay_ms": move_delay_ms,
                "impact_dmg": impact_dmg,
                "died": died,
            }
        windows.append({
            "spell": sp,
            "radius_yd": round(radius, 1) if radius is not None else None,
            "origin": [round(cx, 1), round(cy, 1)] if (cx is not None and cy is not None) else None,
            "start_ms": s0,
            "end_ms": s1,
            "dur_s": round((s1 - s0) / 1000.0, 2),
            "bots": per_bot,
        })
    return {"acts_per_min": acts_per_min, "windows": windows}


def format_timelines(res: dict[str, Any]) -> str:
    windows = res["windows"]
    lines: list[str] = []
    if res["acts_per_min"]:
        apm_str = ", ".join(f"{k}={v:.1f}/m" for k, v in sorted(res["acts_per_min"].items()))
        lines.append(f"Standing-order acts/min: {apm_str}")
    if not windows:
        lines.append("No boss cast windows observed.")
        return "\n".join(lines)
    t0 = windows[0]["start_ms"]
    for w in windows:
        rel = (w["start_ms"] - t0) / 1000.0
        rad = f", radius={w['radius_yd']:.1f}yd" if w["radius_yd"] is not None else ""
        lines.append(f"\n== +{rel:5.1f}s | {w['spell']} (dur={w['dur_s']:.2f}s{rad}) ==")
        lines.append(
            f"  {'Bot':8s} {'StartYd':>7s} {'EndYd':>7s} {'Safe?':>5s} "
            f"{'ActDelay':>9s} {'MoveDelay':>10s} {'ImpactDmg':>9s} {'Died':>4s}  Act"
        )
        for name, b in w["bots"].items():
            sy = f"{b['start_yd']:.1f}" if b["start_yd"] is not None else "-"
            ey = f"{b['end_yd']:.1f}" if b["end_yd"] is not None else "-"
            sf = "-" if b["outside_at_impact"] is None else ("YES" if b["outside_at_impact"] else "NO")
            ad = f"{b['act_delay_ms']}ms" if b["act_delay_ms"] is not None else "NONE"
            md = "NEVER" if b["move_delay_ms"] is None else (
                "already" if b["move_delay_ms"] == "already" else f"{b['move_delay_ms']}ms"
            )
            lines.append(
                f"  {name:8s} {sy:>7s} {ey:>7s} {sf:>5s} {ad:>9s} {md:>10s} "
                f"{b['impact_dmg']:>9d} {'YES' if b['died'] else 'no':>4s}  {b['act_detail']}"
            )
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze boss cast dodge timelines from traces.")
    ap.add_argument("trace_dir", type=pathlib.Path)
    ap.add_argument("--spell", default="", help="Optional case-insensitive spell substring filter")
    ap.add_argument("--radius", type=float, default=None, help="Override spell impact radius (yd)")
    args = ap.parse_args()
    traces = trace_parity.load_traces(args.trace_dir)["bot"]
    res = extract_dodge_timelines(traces, spell_filter=args.spell, radius_override=args.radius)
    print(format_timelines(res))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
