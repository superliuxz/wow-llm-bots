#!/usr/bin/env python3
"""Phase 4 B2 soak stall detector for live/soak bot trace `.jsonl` files.

Flags any living, out-of-combat bot that stays within `radius_yd` (default
15 yd) with zero meaningful progress events (using exact `trace-events.md`
event names) for `>= max_stall_s` (default 300 s = 5 min).
"""

from __future__ import annotations

import argparse
import math
import pathlib
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
from wlblib import trace as trace_lib  # noqa: E402

# Exact event names emitted by `server_module` (`docs/specs/trace-events.md`).
PROGRESS_EVENTS = frozenset({
    "combat_start",
    "combat_end",
    "spell_cast",
    "damage_dealt",
    "heal_done",
    "mob_kill",
    "xp_gain",
    "level_up",
    "quest_accept",
    "quest_turnin",
    "quest_reward",
    "loot",
    "item_equip",
    "item_use",
    "item_craft",
    "vendor_repair",
    "spell_learn",
    "talent_learn",
    "skill_up",
    "go_interact",
    "zone_change",
    "area_change",
    "map_change",
})

LOGOUT_GAP_MS = 60_000


def check_bot_records(
    records: list[dict[str, Any]],
    *,
    max_stall_s: float = 300.0,
    radius_yd: float = 15.0,
) -> list[dict[str, Any]]:
    """Scans one bot's sorted trace records and returns full stall spans."""
    records = sorted(
        (r for r in records if isinstance(r, dict) and "t_ms" in r),
        key=lambda r: int(r.get("t_ms", 0)),
    )

    stalls: list[dict[str, Any]] = []
    anchor_t_ms: int | None = None
    anchor_pos: tuple[float, float, float] | None = None
    last_alive_t_ms: int | None = None

    def close_span(end_t_ms: int | None) -> None:
        nonlocal anchor_t_ms, anchor_pos
        if anchor_t_ms is not None and end_t_ms is not None and end_t_ms >= anchor_t_ms:
            dur_s = (end_t_ms - anchor_t_ms) / 1000.0
            if dur_s >= max_stall_s:
                stalls.append({
                    "start_t_ms": anchor_t_ms,
                    "end_t_ms": end_t_ms,
                    "duration_s": round(dur_s, 1),
                    "pos": anchor_pos,
                })
        anchor_t_ms = None
        anchor_pos = None

    for rec in records:
        t_ms = int(rec.get("t_ms", 0))
        kind = rec.get("kind")

        if kind == "event":
            ev = str(rec.get("event", ""))
            if ev in PROGRESS_EVENTS:
                # If a logout gap preceded this event, close at the last live
                # sample before the gap so offline time is never counted.
                if last_alive_t_ms is not None and (t_ms - last_alive_t_ms) > LOGOUT_GAP_MS:
                    close_span(last_alive_t_ms)
                    last_alive_t_ms = None
                else:
                    close_span(t_ms)
            continue

        if kind != "sample":
            continue

        if bool(rec.get("dead")) or bool(rec.get("in_combat")):
            close_span(last_alive_t_ms)
            last_alive_t_ms = None
            continue

        # Large gap (> 60 s) means logout/restart, not a continuous stall.
        if last_alive_t_ms is not None and (t_ms - last_alive_t_ms) > LOGOUT_GAP_MS:
            close_span(last_alive_t_ms)

        x = float(rec.get("x", 0.0))
        y = float(rec.get("y", 0.0))
        z = float(rec.get("z", 0.0))
        if anchor_t_ms is None or anchor_pos is None:
            anchor_t_ms = t_ms
            anchor_pos = (x, y, z)
        else:
            dist = math.sqrt(
                (x - anchor_pos[0]) ** 2
                + (y - anchor_pos[1]) ** 2
                + (z - anchor_pos[2]) ** 2
            )
            if dist > radius_yd:
                close_span(last_alive_t_ms)
                anchor_t_ms = t_ms
                anchor_pos = (x, y, z)
        last_alive_t_ms = t_ms

    close_span(last_alive_t_ms)
    return stalls


def check_bot_trace(
    trace_paths: list[pathlib.Path],
    *,
    max_stall_s: float = 300.0,
    radius_yd: float = 15.0,
) -> list[dict[str, Any]]:
    """Scans one bot's sorted `.jsonl` trace shards and returns full stall spans."""
    return check_bot_records(
        trace_lib.load_jsonl_files(trace_paths),
        max_stall_s=max_stall_s,
        radius_yd=radius_yd,
    )


def check_trace_dir(
    trace_dir: pathlib.Path,
    *,
    max_stall_s: float = 300.0,
    radius_yd: float = 15.0,
) -> dict[str, list[dict[str, Any]]]:
    """Scans `<trace_dir>/bot/<Name>/*.jsonl` and returns stalls per bot."""
    bot_root = trace_dir / "bot" if (trace_dir / "bot").is_dir() else trace_dir
    by_bot: dict[str, list[pathlib.Path]] = {}
    for fp in sorted(bot_root.glob("*/*.jsonl")):
        by_bot.setdefault(fp.parent.name, []).append(fp)
    results: dict[str, list[dict[str, Any]]] = {}
    for bot_name, paths in sorted(by_bot.items()):
        stalls = check_bot_trace(paths, max_stall_s=max_stall_s, radius_yd=radius_yd)
        if stalls:
            results[bot_name] = stalls
    return results


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("trace_dir", type=pathlib.Path)
    ap.add_argument("--max-stall-s", type=float, default=300.0)
    ap.add_argument("--radius-yd", type=float, default=15.0)
    args = ap.parse_args(argv)
    stalls = check_trace_dir(
        args.trace_dir, max_stall_s=args.max_stall_s, radius_yd=args.radius_yd
    )
    if not stalls:
        print(f"OK: no stalls >= {args.max_stall_s:.0f}s found in {args.trace_dir}")
        return 0
    for bot, spans in stalls.items():
        for sp in spans:
            print(
                f"STALL: {bot} stationary {sp['duration_s']}s "
                f"(t={sp['start_t_ms']}..{sp['end_t_ms']})"
            )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
