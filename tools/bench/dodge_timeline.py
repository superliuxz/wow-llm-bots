#!/usr/bin/env python3
"""Boss cast / dodge timeline analyzer from unified behavior traces.

Reads `<trace_dir>/bot/<Name>/*.jsonl` and, for every boss spell cast observed
in `target_casting`, prints a per-bot timeline:
  cast start -> first movement after cast -> max distance -> distance at impact
  -> damage taken around cast end -> death (if any).
"""

from __future__ import annotations

import argparse
import pathlib
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import trace_parity  # noqa: E402


def extract_dodge_timelines(
    traces: dict[str, list[dict[str, Any]]],
    spell_filter: str = "",
) -> list[dict[str, Any]]:
    """Finds boss casts seen via `target_casting` and measures each bot's reaction."""
    # Collect cast windows across all bots (merge overlapping observations of same spell).
    raw_windows: list[tuple[int, int, str]] = []
    for rows in traces.values():
        cur_spell = ""
        t_start = 0
        t_last = 0
        for r in rows:
            if r.get("kind") != "sample":
                continue
            sp = str(r.get("target_casting", ""))
            t = int(r["t_ms"])
            if sp and (not spell_filter or spell_filter.lower() in sp.lower()):
                if sp != cur_spell or t - t_last > 1500:
                    if cur_spell:
                        raw_windows.append((t_start, t_last, cur_spell))
                    cur_spell = sp
                    t_start = t
                t_last = t
            elif cur_spell and t - t_last > 600:
                raw_windows.append((t_start, t_last, cur_spell))
                cur_spell = ""
        if cur_spell:
            raw_windows.append((t_start, t_last, cur_spell))

    raw_windows.sort()
    merged: list[list[Any]] = []
    for s0, s1, sp in raw_windows:
        if merged and merged[-1][2] == sp and s0 <= merged[-1][1] + 1000:
            merged[-1][1] = max(merged[-1][1], s1)
        else:
            merged.append([s0, s1, sp])

    out: list[dict[str, Any]] = []
    for s0, s1, sp in merged:
        per_bot: dict[str, dict[str, Any]] = {}
        for name, rows in sorted(traces.items()):
            samples = [
                r for r in rows
                if r.get("kind") == "sample" and s0 - 400 <= int(r["t_ms"]) <= s1 + 1500
            ]
            events = [
                r for r in rows
                if r.get("kind") == "event" and s1 - 500 <= int(r["t_ms"]) <= s1 + 1500
            ]
            during = [r for r in samples if s0 <= int(r["t_ms"]) <= s1 + 200]
            dists = [float(r["target_dist"]) for r in during if float(r.get("target_dist", -1)) >= 0]
            start_dist = dists[0] if dists else None
            end_dist = dists[-1] if dists else None
            max_dist = max(dists) if dists else None
            move_ms = None
            for r in during:
                if r.get("moving"):
                    move_ms = int(r["t_ms"]) - s0
                    break
            impact_dmg = sum(
                int(e.get("value", 0)) for e in events if e.get("event") == "damage_taken"
            )
            died = any(e.get("event") == "died" for e in events)
            per_bot[name] = {
                "start_yd": round(start_dist, 1) if start_dist is not None else None,
                "end_yd": round(end_dist, 1) if end_dist is not None else None,
                "max_yd": round(max_dist, 1) if max_dist is not None else None,
                "move_delay_ms": move_ms,
                "impact_dmg": impact_dmg,
                "died": died,
            }
        out.append({
            "spell": sp,
            "start_ms": s0,
            "end_ms": s1,
            "dur_s": round((s1 - s0) / 1000.0, 2),
            "bots": per_bot,
        })
    return out


def format_timelines(windows: list[dict[str, Any]]) -> str:
    if not windows:
        return "No boss cast windows observed."
    t0 = windows[0]["start_ms"]
    lines: list[str] = []
    for w in windows:
        rel = (w["start_ms"] - t0) / 1000.0
        lines.append(f"== +{rel:5.1f}s | {w['spell']} (dur={w['dur_s']:.2f}s) ==")
        lines.append(f"  {'Bot':10s} {'StartYd':>8s} {'EndYd':>8s} {'MaxYd':>8s} {'MoveDelay':>10s} {'ImpactDmg':>10s} {'Died':>5s}")
        for name, b in w["bots"].items():
            sy = f"{b['start_yd']:.1f}" if b["start_yd"] is not None else "-"
            ey = f"{b['end_yd']:.1f}" if b["end_yd"] is not None else "-"
            my = f"{b['max_yd']:.1f}" if b["max_yd"] is not None else "-"
            md = f"{b['move_delay_ms']}ms" if b["move_delay_ms"] is not None else "NEVER"
            lines.append(
                f"  {name:10s} {sy:>8s} {ey:>8s} {my:>8s} {md:>10s} {b['impact_dmg']:>10d} {'YES' if b['died'] else 'no':>5s}"
            )
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze boss cast dodge timelines from traces.")
    ap.add_argument("trace_dir", type=pathlib.Path)
    ap.add_argument("--spell", default="", help="Optional case-insensitive spell substring filter")
    args = ap.parse_args()
    traces = trace_parity.load_traces(args.trace_dir)["bot"]
    windows = extract_dodge_timelines(traces, spell_filter=args.spell)
    print(format_timelines(windows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
