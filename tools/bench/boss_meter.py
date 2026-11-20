#!/usr/bin/env python3
"""Boss & dungeon combat meter analyzer from unified behavior traces.

Reads `<trace_dir>/bot/<Name>/*.jsonl` (or `human/`) recorded with
`behavior_trace_scripts.cc` (`damage_dealt`, `damage_taken`, `heal_done`,
`cast_start`, `combat_start`/`combat_end`, `died`, `target_dist`,
`target_casting`) and produces a WarcraftLogs-style summary table:
  - Fight duration (s)
  - Per-character DPS, total damage dealt, HPS, total healing done
  - Damage taken, deaths, casts/min (CPM), active %
  - Dodge / spacing stats during boss casts (when `target_casting` is seen)
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import trace_parity  # noqa: E402


def analyze_boss_fight(
    traces: dict[str, list[dict[str, Any]]],
    boss_name: str = "",
) -> dict[str, Any]:
    """Computes DPS, HPS, damage taken, CPM, deaths, and boss-cast spacing."""
    t_start: int | None = None
    t_end: int | None = None
    per_char: dict[str, dict[str, Any]] = {}

    for rows in traces.values():
        cbt_ts = [
            int(s["t_ms"])
            for s in rows
            if s.get("kind") == "sample" and s.get("in_combat")
        ]
        if cbt_ts:
            s0, s1 = min(cbt_ts), max(cbt_ts)
            t_start = s0 if t_start is None else min(t_start, s0)
            t_end = s1 if t_end is None else max(t_end, s1)

    win_lo = (t_start - 500) if t_start is not None else None
    win_hi = (t_end + 500) if t_end is not None else None

    for name, rows in sorted(traces.items()):
        samples = [
            r for r in rows
            if r.get("kind") == "sample"
            and (win_lo is None or win_lo <= int(r.get("t_ms", 0)) <= win_hi)
        ]
        events = [
            r for r in rows
            if r.get("kind") == "event"
            and (win_lo is None or win_lo <= int(r.get("t_ms", 0)) <= win_hi)
        ]

        dmg_dealt = 0
        dmg_taken = 0
        heal_done = 0
        casts = 0
        deaths = 0
        boss_killed = False

        for e in events:
            ev = e.get("event", "")
            val = int(e.get("value", 0))
            det = str(e.get("detail", ""))
            if ev == "damage_dealt":
                if not boss_name or det.lower() == boss_name.lower():
                    dmg_dealt += val
            elif ev == "damage_taken":
                dmg_taken += val
            elif ev == "heal_done":
                heal_done += val
            elif ev == "cast_start":
                casts += 1
            elif ev == "died":
                deaths += 1
            elif ev == "mob_kill":
                if boss_name and det.lower() == boss_name.lower():
                    boss_killed = True

        # Track target distance during boss spell casts (e.g. Lightning Nova).
        cast_dists: list[float] = []
        for s in samples:
            if s.get("target_casting") and float(s.get("target_dist", -1.0)) >= 0:
                cast_dists.append(float(s["target_dist"]))

        per_char[name] = {
            "damage_dealt": dmg_dealt,
            "damage_taken": dmg_taken,
            "heal_done": heal_done,
            "casts": casts,
            "deaths": deaths,
            "boss_killed": boss_killed,
            "max_dist_during_boss_cast_yd": round(max(cast_dists), 1) if cast_dists else None,
        }

    dur_s = max(1.0, (t_end - t_start) / 1000.0) if (t_start is not None and t_end is not None and t_end > t_start) else 1.0
    dur_min = dur_s / 60.0

    for info in per_char.values():
        info["dps"] = round(info["damage_dealt"] / dur_s, 1)
        info["hps"] = round(info["heal_done"] / dur_s, 1)
        info["cpm"] = round(info["casts"] / dur_min, 1)

    total_dps = round(sum(v["dps"] for v in per_char.values()), 1)
    total_hps = round(sum(v["hps"] for v in per_char.values()), 1)
    total_deaths = sum(v["deaths"] for v in per_char.values())
    killed = any(v["boss_killed"] for v in per_char.values())

    return {
        "boss": boss_name or "(all)",
        "duration_s": round(dur_s, 1),
        "killed": killed,
        "total_dps": total_dps,
        "total_hps": total_hps,
        "total_deaths": total_deaths,
        "characters": per_char,
    }


def format_boss_meter(res: dict[str, Any]) -> str:
    lines = [
        f"== Boss Meter: {res['boss']} | duration={res['duration_s']:.1f}s | "
        f"killed={'YES' if res['killed'] else 'NO'} | "
        f"party DPS={res['total_dps']:.1f} | party HPS={res['total_hps']:.1f} | "
        f"deaths={res['total_deaths']} ==",
        f"  {'Character':14s} {'DPS':>8s} {'Damage':>9s} {'HPS':>8s} {'Healing':>9s} "
        f"{'DmgTaken':>9s} {'CPM':>6s} {'Deaths':>6s} {'CastDodgeYd':>11s}",
    ]
    for name, c in sorted(res["characters"].items(), key=lambda kv: -kv[1]["dps"]):
        dodge = f"{c['max_dist_during_boss_cast_yd']:.1f}yd" if c["max_dist_during_boss_cast_yd"] is not None else "-"
        lines.append(
            f"  {name:14s} {c['dps']:8.1f} {c['damage_dealt']:9d} {c['hps']:8.1f} "
            f"{c['heal_done']:9d} {c['damage_taken']:9d} {c['cpm']:6.1f} "
            f"{c['deaths']:6d} {dodge:>11s}"
        )
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze boss DPS/HPS/mechanics from behavior traces")
    ap.add_argument("trace_dir", type=pathlib.Path)
    ap.add_argument("--role", choices=["bot", "human"], default="bot")
    ap.add_argument("--boss", default="")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    traces = trace_parity.load_traces(args.trace_dir)[args.role]
    res = analyze_boss_fight(traces, boss_name=args.boss)
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print(format_boss_meter(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
