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
import statistics
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import trace_parity  # noqa: E402

DODGE_ACT_VERBS = ("OUT_OF", "AWAY", "AVOID", "TO", "KITE", "STOP")


def _is_dodge_act(detail: str) -> bool:
    verb = detail.strip().split(" ", 1)[0].upper() if detail else ""
    return verb in DODGE_ACT_VERBS


def _sample_dist(r: dict[str, Any], cx: float | None, cy: float | None) -> float | None:
    if cx is not None and cy is not None and "x" in r and "y" in r:
        return math.hypot(float(r["x"]) - cx, float(r["y"]) - cy)
    td = float(r.get("target_dist", -1))
    return td if td >= 0 else None


def _is_moving_away_at(
    t_ms: int,
    dist_timeline: list[tuple[int, float, bool]],
) -> bool:
    """Returns True if the bot is still increasing distance from the cast origin at `t_ms`."""
    prev_idx: int | None = None
    next_idx: int | None = None
    for i, (ts, _d, _m) in enumerate(dist_timeline):
        if ts <= t_ms:
            prev_idx = i
        elif next_idx is None:
            next_idx = i
            break
    if prev_idx is not None and next_idx is not None:
        return dist_timeline[next_idx][1] > dist_timeline[prev_idx][1] + 0.5
    if prev_idx is not None and prev_idx > 0:
        _, d_prev, moving_prev = dist_timeline[prev_idx]
        _, d_prev2, _ = dist_timeline[prev_idx - 1]
        return moving_prev and d_prev > d_prev2 + 0.5
    return False


def extract_dodge_timelines(
    traces: dict[str, list[dict[str, Any]]],
    spell_filter: str = "",
    radius_override: float | None = None,
    bots: set[str] | None = None,
) -> dict[str, Any]:
    """Finds boss casts seen via `target_casting` and measures each bot's reaction.

    Windows are isolated per `inst` (dungeon instance ID) so parallel parties
    sharing a trace directory are never scored against another party's cast.
    """
    if bots:
        traces = {n: r for n, r in traces.items() if n in bots}
    # Each raw window: (t_start, t_end, spell_name, radius_yd, cast_x, cast_y, inst_id)
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
            inst_id = int(r.get("inst", 0))
            if sp and (not spell_filter or spell_filter.lower() in sp.lower()):
                rad = float(r.get("target_cast_radius", -1.0))
                tx = float(r["target_x"]) if "target_x" in r else None
                ty = float(r["target_y"]) if "target_y" in r else None
                if cur is None or cur[2] != sp or cur[6] != inst_id or t - cur[1] > 1500:
                    if cur is not None:
                        raw_windows.append(cur)
                    cur = [t, t, sp, rad if rad > 0 else None, tx, ty, inst_id]
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

    raw_windows.sort(key=lambda w: (w[6], w[0], w[1]))
    merged: list[list[Any]] = []
    for w in raw_windows:
        if merged and merged[-1][6] == w[6] and merged[-1][2] == w[2] and w[0] <= merged[-1][1] + 1000:
            merged[-1][1] = max(merged[-1][1], w[1])
            if w[3] is not None:
                merged[-1][3] = w[3]
            if w[4] is not None and w[5] is not None:
                merged[-1][4], merged[-1][5] = w[4], w[5]
        else:
            merged.append(list(w))
    merged.sort(key=lambda w: (w[0], w[1], w[6]))

    windows: list[dict[str, Any]] = []
    for s0, s1, sp, dbc_radius, cx, cy, inst_id in merged:
        radius = radius_override if radius_override is not None else dbc_radius
        per_bot: dict[str, dict[str, Any]] = {}
        for name, rows in sorted(traces.items()):
            samples = [
                r for r in rows
                if r.get("kind") == "sample"
                and s0 - 600 <= int(r["t_ms"]) <= s1 + 1500
                and (inst_id == 0 or int(r.get("inst", 0)) == inst_id)
            ]
            if inst_id != 0 and not samples:
                continue  # Bot was in a different dungeon instance.
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
            during_dist_timeline: list[tuple[int, float, bool]] = []
            for r in during:
                d = _sample_dist(r, cx, cy)
                if d is not None:
                    dists.append(d)
                    during_dist_timeline.append((int(r["t_ms"]), d, bool(r.get("moving"))))
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

            # Pre-cast DPS in [s0 - 10000, s0), clamped to [1.0, 10.0]s via cbt_t0.
            dps_before_raw: float | None = None
            if cbt_t0 is not None and cbt_t0 < s0:
                pre_dur_s = max(1.0, min(10.0, (s0 - max(s0 - 10000, cbt_t0)) / 1000.0))
                dmg_before = sum(
                    int(e.get("value", 0))
                    for e in rows
                    if e.get("kind") == "event"
                    and e.get("event") == "damage_dealt"
                    and s0 - 10000 <= int(e["t_ms"]) < s0
                )
                dps_before_raw = dmg_before / pre_dur_s

            # Time back in range / resuming DPS after impact, only if the bot actually dodged.
            dodged = (
                act_delay_ms is not None
                or (max_dist is not None and start_dist is not None and max_dist > start_dist + 2.0)
            )
            time_back_in_range_ms: int | None = None
            if dodged:
                died_before_impact = any(
                    r.get("kind") == "event"
                    and r.get("event") == "died"
                    and s0 <= int(r["t_ms"]) < s1
                    for r in rows
                )
                if not died_before_impact:
                    post_rows = sorted(
                        (
                            r for r in rows
                            if s1 <= int(r["t_ms"]) <= s1 + 20000
                            and (
                                r.get("kind") == "event"
                                or (
                                    r.get("kind") == "sample"
                                    and (inst_id == 0 or int(r.get("inst", 0)) == inst_id)
                                )
                            )
                        ),
                        key=lambda r: (
                            int(r["t_ms"]),
                            0 if (r.get("kind") == "event" and r.get("event") == "died") else 1,
                        ),
                    )
                    dist_timeline: list[tuple[int, float, bool]] = [
                        pt for pt in during_dist_timeline if pt[0] < s1
                    ]
                    for r in post_rows:
                        if r.get("kind") == "sample":
                            d = _sample_dist(r, cx, cy)
                            if d is not None:
                                dist_timeline.append((int(r["t_ms"]), d, bool(r.get("moving"))))

                    # "Back" means within engagement range of the attack target
                    # AND actively casting/attacking again (not a DoT tick from
                    # 40 yd away, nor walking in without resuming combat).
                    if start_dist is not None:
                        attack_range_yd = (
                            max(start_dist + 4.0, 8.0)
                            if start_dist <= 10.0
                            else min(36.0, max(start_dist + 4.0, 30.0))
                        )
                    else:
                        attack_range_yd = 30.0

                    def _target_dist_of(sample_row: dict[str, Any]) -> float | None:
                        td = float(sample_row.get("target_dist", -1.0))
                        if td >= 0.0:
                            return td
                        return _sample_dist(sample_row, cx, cy)

                    latest_target_dist: float | None = (
                        _target_dist_of(during[-1]) if during else end_dist
                    )

                    for r in post_rows:
                        t = int(r["t_ms"])
                        kind = r.get("kind")
                        if kind == "sample":
                            td = _target_dist_of(r)
                            if td is not None:
                                latest_target_dist = td
                            in_range = (
                                latest_target_dist is not None
                                and latest_target_dist <= attack_range_yd
                            )
                            if (
                                in_range
                                and bool(r.get("casting", ""))
                                and not _is_moving_away_at(t, dist_timeline)
                            ):
                                time_back_in_range_ms = t - s1
                                break
                        elif kind == "event":
                            ev = r.get("event")
                            if ev == "died":
                                break
                            in_range = (
                                latest_target_dist is not None
                                and latest_target_dist <= attack_range_yd
                            )
                            if (
                                ev in ("spell_cast", "cast_start")
                                and in_range
                                and not _is_moving_away_at(t, dist_timeline)
                            ):
                                time_back_in_range_ms = t - s1
                                break
                            if (
                                ev == "damage_dealt"
                                and latest_target_dist is not None
                                and latest_target_dist <= 8.0
                                and not _is_moving_away_at(t, dist_timeline)
                            ):
                                time_back_in_range_ms = t - s1
                                break

            downtime_s_raw: float | None = None
            dps_during_raw: float | None = None
            dps_lost_raw: float | None = None
            if time_back_in_range_ms is not None:
                return_t = s1 + time_back_in_range_ms
                downtime_s_raw = (return_t - s0) / 1000.0
                dmg_during = sum(
                    int(e.get("value", 0))
                    for e in rows
                    if e.get("kind") == "event"
                    and e.get("event") == "damage_dealt"
                    and s0 <= int(e["t_ms"]) <= return_t
                )
                if downtime_s_raw > 0:
                    dps_during_raw = dmg_during / downtime_s_raw
                if dps_before_raw is not None:
                    dps_lost_raw = max(0.0, dps_before_raw * downtime_s_raw - dmg_during)

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
                "dps_before": round(dps_before_raw, 1) if dps_before_raw is not None else None,
                "time_back_in_range_ms": time_back_in_range_ms,
                "downtime_s": round(downtime_s_raw, 2) if downtime_s_raw is not None else None,
                "dps_during": round(dps_during_raw, 1) if dps_during_raw is not None else None,
                "dps_lost": round(dps_lost_raw, 1) if dps_lost_raw is not None else None,
            }
        windows.append({
            "spell": sp,
            "instance_id": inst_id,
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
        inst_s = f", inst={w['instance_id']}" if w.get("instance_id") else ""
        lines.append(f"\n== +{rel:5.1f}s | {w['spell']} (dur={w['dur_s']:.2f}s{rad}{inst_s}) ==")
        lines.append(
            f"  {'Bot':8s} {'StartYd':>7s} {'EndYd':>7s} {'Safe?':>5s} "
            f"{'ActDelay':>9s} {'MoveDelay':>10s} {'ImpactDmg':>9s} {'Died':>4s} "
            f"{'BackMs':>8s} {'DmgLost':>8s}  Act"
        )
        for name, b in w["bots"].items():
            sy = f"{b['start_yd']:.1f}" if b["start_yd"] is not None else "-"
            ey = f"{b['end_yd']:.1f}" if b["end_yd"] is not None else "-"
            sf = "-" if b["outside_at_impact"] is None else ("YES" if b["outside_at_impact"] else "NO")
            ad = f"{b['act_delay_ms']}ms" if b["act_delay_ms"] is not None else "NONE"
            md = "NEVER" if b["move_delay_ms"] is None else (
                "already" if b["move_delay_ms"] == "already" else f"{b['move_delay_ms']}ms"
            )
            bm = f"{b['time_back_in_range_ms']}ms" if b.get("time_back_in_range_ms") is not None else "-"
            dl = f"{b['dps_lost']:.0f}" if b.get("dps_lost") is not None else "-"
            lines.append(
                f"  {name:8s} {sy:>7s} {ey:>7s} {sf:>5s} {ad:>9s} {md:>10s} "
                f"{b['impact_dmg']:>9d} {'YES' if b['died'] else 'no':>4s} "
                f"{bm:>8s} {dl:>8s}  {b['act_detail']}"
            )
    back_vals = [
        b["time_back_in_range_ms"]
        for w in windows
        for b in w["bots"].values()
        if b.get("time_back_in_range_ms") is not None
    ]
    lost_vals = [
        b["dps_lost"]
        for w in windows
        for b in w["bots"].values()
        if b.get("dps_lost") is not None
    ]
    if back_vals or lost_vals:
        med_back = f"{statistics.median(back_vals):.0f}ms" if back_vals else "-"
        med_lost = f"{statistics.median(lost_vals):.0f}" if lost_vals else "-"
        lines.append(
            f"\nDodge recovery summary (n={len(back_vals)}): "
            f"median_back_ms={med_back}, median_dps_lost={med_lost}"
        )
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze boss cast dodge timelines from traces.")
    ap.add_argument("trace_dir", type=pathlib.Path)
    ap.add_argument("--spell", default="", help="Optional case-insensitive spell substring filter")
    ap.add_argument("--radius", type=float, default=None, help="Override spell impact radius (yd)")
    ap.add_argument("--bots", default="", help="Optional comma-separated bot names to include")
    args = ap.parse_args()
    bot_set = {b.strip() for b in args.bots.split(",") if b.strip()} or None
    traces = trace_parity.load_traces(args.trace_dir)["bot"]
    res = extract_dodge_timelines(traces, spell_filter=args.spell, radius_override=args.radius, bots=bot_set)
    print(format_timelines(res))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
