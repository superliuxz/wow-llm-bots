#!/usr/bin/env python3
"""Mine real failure moments from behavior JSONL traces into offline eval cases.

Reads `<trace_dir>/bot/<Name>/*.jsonl` (or `<trace_dir>/*.jsonl`), detects:
  (a) `missed_dodge`: heavy `damage_taken` during/just after a boss `target_casting`
      window with `target_cast_radius > 0` while inside radius and without an
      `act_sent` dodge.
  (b) `death`: `died` event with surrounding combat context.
  (c) `stall`: non-combat, non-dead, non-moving, non-casting idle stretch
      `>= threshold_s` (default 120s).

Converts each mined failure into:
  - `<case_name>.snapshot.json` matching `wlb::snapshot::Snapshot` (`snapshot/snapshot.h`)
  - `<case_name>.facts.json` (`{}`)
  - an entry in `cases.json` compatible with `tools/wlblib/cases/eval_plans.py`.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

DODGE_ACT_VERBS = ("AWAY", "TO", "AVOID", "OUT_OF", "KITE")


@dataclasses.dataclass
class MinedFailure:
    kind: str  # "missed_dodge", "death", "stall"
    bot: str
    source_file: str
    t_ms: int
    sample: Dict[str, Any]
    boss: str = ""
    spell: str = ""
    radius_yd: float = 0.0
    damage: int = 0
    stall_s: float = 0.0
    killer: str = ""


def _slugify(text: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")
    return s or "unknown"


def _direction_from_delta(dx: float, dy: float) -> str:
    """Compute compass Direction enum name from (dx, dy) in WoW world coordinates."""
    if abs(dx) < 0.5 and abs(dy) < 0.5:
        return "HERE"
    # WoW coordinates: +X is North, +Y is West.
    angle = math.degrees(math.atan2(-dy, dx)) % 360.0
    dirs = ("N", "NE", "E", "SE", "S", "SW", "W", "NW")
    idx = int((angle + 22.5) // 45.0) % 8
    return dirs[idx]


def _dist_2d(x1: float, y1: float, x2: float, y2: float) -> float:
    return math.hypot(x2 - x1, y2 - y1)


def _is_dodge_act(row: Dict[str, Any]) -> bool:
    if row.get("kind") != "event" or row.get("event") != "act_sent":
        return False
    detail = str(row.get("detail", "")).upper()
    if "[ERR:" in detail:
        return False
    return any(detail.startswith(v) for v in DODGE_ACT_VERBS)


def _inside_cast_radius(sample: Dict[str, Any]) -> bool:
    radius = float(sample.get("target_cast_radius", -1.0) or -1.0)
    if radius <= 0.0:
        return False
    tdist = float(sample.get("target_dist", -1.0) or -1.0)
    if 0.0 <= tdist <= radius:
        return True
    # Also check ground-targeted cast destination (target_x, target_y).
    if "target_x" in sample and "target_y" in sample and "x" in sample and "y" in sample:
        dx = float(sample["target_x"]) - float(sample["x"])
        dy = float(sample["target_y"]) - float(sample["y"])
        if math.hypot(dx, dy) <= radius:
            return True
    return False


def load_trace_rows(jsonl_path: Path) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    with jsonl_path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(obj, dict):
                rows.append(obj)
    return rows


def discover_trace_files(trace_dir: Path) -> List[Path]:
    bot_dir = trace_dir / "bot"
    if bot_dir.is_dir():
        return sorted(bot_dir.rglob("*.jsonl"))
    return sorted(trace_dir.rglob("*.jsonl"))


def mine_trace_rows(
    rows: Sequence[Dict[str, Any]],
    source_file: str,
    boss_filter: Optional[str] = None,
    stall_threshold_s: float = 120.0,
    min_damage: int = 1000,
) -> List[MinedFailure]:
    """Mine missed_dodge, death, and stall failures from a single trace's rows."""
    failures: List[MinedFailure] = []
    boss_filter_low = boss_filter.lower() if boss_filter else None

    # Track recent state for missed_dodge and death.
    last_live_sample: Optional[Dict[str, Any]] = None
    last_combat_sample: Optional[Dict[str, Any]] = None
    active_cast_sample: Optional[Dict[str, Any]] = None
    active_cast_start_ms: int = 0
    active_cast_end_ms: int = 0
    active_cast_dodged: bool = False
    active_cast_recorded: bool = False

    # Track stall stretch.
    stall_start_sample: Optional[Dict[str, Any]] = None
    stall_start_ms: int = 0
    stall_recorded: bool = False

    for row in rows:
        kind = row.get("kind")
        t_ms = int(row.get("t_ms", 0) or 0)

        if kind == "sample":
            bot = str(row.get("name", "Bot"))
            in_combat = bool(row.get("in_combat", False))
            dead = bool(row.get("dead", False))
            moving = bool(row.get("moving", False))
            casting = bool(row.get("casting", ""))

            if not dead:
                last_live_sample = row
                if in_combat:
                    last_combat_sample = row

            # --- Cast window tracking for missed_dodge ---
            t_cast = str(row.get("target_casting", "") or "")
            t_rad = float(row.get("target_cast_radius", -1.0) or -1.0)
            # Ignore room-wide spells (>60 yd, e.g. Gravity Lapse 500 yd,
            # Murmur's Touch 80 yd) that cannot be outranged by moving out.
            if t_cast and 0.0 < t_rad <= 60.0 and _inside_cast_radius(row):
                if (
                    active_cast_sample is None
                    or active_cast_sample.get("target_casting") != t_cast
                    or (t_ms - active_cast_end_ms) > 2000
                ):
                    active_cast_sample = row
                    active_cast_start_ms = t_ms
                    active_cast_dodged = False
                    active_cast_recorded = False
                active_cast_end_ms = t_ms
            elif active_cast_sample is not None and (t_ms - active_cast_end_ms) > 2000:
                active_cast_sample = None
                active_cast_dodged = False
                active_cast_recorded = False

            # --- Stall tracking ---
            is_idle = (not in_combat) and (not dead) and (not moving) and (not casting)
            if is_idle:
                if stall_start_sample is None:
                    stall_start_sample = row
                    stall_start_ms = t_ms
                    stall_recorded = False
                elif not stall_recorded and (t_ms - stall_start_ms) >= int(stall_threshold_s * 1000):
                    if not boss_filter_low:
                        failures.append(
                            MinedFailure(
                                kind="stall",
                                bot=bot,
                                source_file=source_file,
                                t_ms=t_ms,
                                sample=row,
                                stall_s=(t_ms - stall_start_ms) / 1000.0,
                            )
                        )
                    stall_recorded = True
            else:
                stall_start_sample = None
                stall_start_ms = 0
                stall_recorded = False

        elif kind == "event":
            ev = str(row.get("event", ""))
            bot = str(row.get("name", last_live_sample.get("name", "Bot") if last_live_sample else "Bot"))

            if _is_dodge_act(row):
                if active_cast_sample is not None and t_ms >= active_cast_start_ms - 500:
                    active_cast_dodged = True

            elif ev == "damage_taken":
                dmg = int(row.get("value", 0) or 0)
                attacker = str(row.get("detail", "") or "")
                if (
                    active_cast_sample is not None
                    and not active_cast_dodged
                    and not active_cast_recorded
                    and (t_ms - active_cast_end_ms) <= 1500
                    and dmg >= min_damage
                ):
                    boss_name = str(active_cast_sample.get("target", "") or attacker or "Boss")
                    if boss_filter_low and boss_filter_low not in boss_name.lower() and boss_filter_low not in attacker.lower():
                        continue
                    failures.append(
                        MinedFailure(
                            kind="missed_dodge",
                            bot=bot,
                            source_file=source_file,
                            t_ms=int(active_cast_sample.get("t_ms", t_ms)),
                            sample=active_cast_sample,
                            boss=boss_name,
                            spell=str(active_cast_sample.get("target_casting", "")),
                            radius_yd=float(active_cast_sample.get("target_cast_radius", 0.0)),
                            damage=dmg,
                        )
                    )
                    active_cast_recorded = True

            elif ev == "died":
                killer = str(row.get("detail", "") or "Unknown")
                ctx_sample = last_combat_sample or last_live_sample
                if ctx_sample is not None:
                    raw_target = str(ctx_sample.get("target", "") or "")
                    boss_name = raw_target or killer
                    if boss_filter_low and boss_filter_low not in boss_name.lower() and boss_filter_low not in killer.lower():
                        continue
                    spell_name = str(ctx_sample.get("target_casting", "") or "")
                    if boss_filter_low and boss_filter_low not in raw_target.lower():
                        boss_name = killer
                        spell_name = ""
                    failures.append(
                        MinedFailure(
                            kind="death",
                            bot=bot,
                            source_file=source_file,
                            t_ms=t_ms,
                            sample=ctx_sample,
                            boss=boss_name,
                            spell=spell_name,
                            radius_yd=max(0.0, float(ctx_sample.get("target_cast_radius", 0.0) or 0.0)) if spell_name else 0.0,
                            killer=killer,
                        )
                    )

    return failures


def build_snapshot(failure: MinedFailure, seq: int = 1) -> Dict[str, Any]:
    """Build a valid `wlb::snapshot::Snapshot` JSON dict from a MinedFailure."""
    s = failure.sample
    bot_name = failure.bot or str(s.get("name", "Bot"))
    level = int(s.get("level", 80) or 80)
    hp_pct = float(s.get("hp_pct", 100.0) if s.get("hp_pct") is not None else 100.0)
    mana_pct = float(s.get("mana_pct", 100.0) if s.get("mana_pct") is not None else 100.0)
    hp_max = max(100, level * 300)
    hp_cur = max(1, int(round(hp_max * hp_pct / 100.0)))
    mana_max = max(100, level * 350)
    mana_cur = int(round(mana_max * mana_pct / 100.0))

    x = float(s.get("x", 0.0) or 0.0)
    y = float(s.get("y", 0.0) or 0.0)
    z = float(s.get("z", 0.0) or 0.0)
    facing = float(s.get("o", 0.0) or 0.0)
    zone = str(s.get("zone", "") or "Unknown Zone")
    in_instance = bool(s.get("in_dungeon", False)) or int(s.get("inst", 0) or 0) > 0
    in_combat = bool(s.get("in_combat", failure.kind != "stall"))

    units: List[Dict[str, Any]] = []
    areas: List[Dict[str, Any]] = []
    options: List[Dict[str, Any]] = []

    target_name = failure.boss or str(s.get("target", "") or "")
    if target_name and failure.kind != "stall":
        tx = float(s.get("target_x", x + 10.0) if s.get("target_x") is not None else x + 10.0)
        ty = float(s.get("target_y", y) if s.get("target_y") is not None else y)
        dist = float(s.get("target_dist", -1.0) or -1.0)
        if dist < 0.0:
            dist = round(_dist_2d(x, y, tx, ty), 1)
        direction = _direction_from_delta(tx - x, ty - y)
        t_level = int(s.get("target_level", max(level + 2, 1)) or max(level + 2, 1))
        t_hp = float(s.get("target_hp_pct", 75.0) if (s.get("target_hp_pct") is not None and float(s.get("target_hp_pct", -1)) >= 0) else 75.0)
        t_guid = "0xF130000000000001"

        activity: Dict[str, Any]
        if failure.spell:
            activity = {
                "state": "CASTING",
                "cast": {
                    "spell": {"name": failure.spell},
                    "remaining_ms": 1500,
                },
                "with": bot_name,
            }
        else:
            activity = {
                "state": "FIGHTING",
                "with": bot_name,
            }

        units.append(
            {
                "guid": t_guid,
                "name": target_name,
                "rel": {
                    "dist_yd": round(dist, 1),
                    "dir": direction,
                    "height": "LEVEL",
                    "in_los": True,
                    "position": {"x": tx, "y": ty, "z": z},
                },
                "kind": "NPC",
                "level": t_level,
                "level_color": "SKULL_LEVEL" if t_level >= level + 3 else "ORANGE",
                "reaction": "HOSTILE",
                "classification": "WORLD_BOSS" if in_instance else "ELITE",
                "health_pct": round(t_hp, 1),
                "targeting_me": bool(s.get("target_targeting_me", False)),
                "activity": activity,
                "detail": "NAMEPLATE",
            }
        )

        if failure.spell and failure.radius_yd > 0.0:
            areas.append(
                {
                    "guid": "0xF150000000000001",
                    "name": failure.spell,
                    "rel": {
                        "dist_yd": round(dist, 1),
                        "dir": direction,
                        "height": "LEVEL",
                        "in_los": True,
                        "position": {"x": tx, "y": ty, "z": z},
                    },
                    "spell": {"name": failure.spell},
                    "radius_yd": round(failure.radius_yd, 1),
                    "inside_me": True,
                    "hostile": True,
                    "lands_in_s": 1.5,
                }
            )

        options.append(
            {
                "text": f"Attack {target_name}",
                "guid": t_guid,
                "kind": "ATTACK",
                "name": target_name,
            }
        )
        options.append(
            {
                "text": "Keep doing what I'm doing",
                "kind": "KEEP",
            }
        )
    else:
        # Out-of-combat / stall options.
        options = [
            {"text": "Keep doing what I'm doing", "kind": "KEEP"},
            {"text": "Explore nearby", "kind": "EXPLORE"},
            {"text": "Rest to recover", "kind": "REST"},
            {"text": "Go to town", "kind": "GO_TO"},
        ]

    nearby: Dict[str, Any] = {"units": units}
    if areas:
        nearby["areas"] = areas

    snapshot: Dict[str, Any] = {
        "bot": bot_name,
        "seq": seq,
        "time_ms": failure.t_ms,
        "monotonic_us": int(s.get("mono_us", 0) or 0),
        "base": {
            "self": {
                "unit": {
                    "guid": "0x0000000000000101",
                    "name": bot_name,
                    "kind": "PLAYER",
                    "level": level,
                    "health_pct": round(hp_pct, 1),
                    "power_pct": round(mana_pct, 1),
                    "player": {"class": "Mage"},
                },
                "health": hp_cur,
                "health_max": hp_max,
                "power_type": "MANA",
                "power": mana_cur,
                "power_max": mana_max,
                "state": "ALIVE",
                "in_combat": in_combat,
            },
            "where": {
                "map": zone,
                "zone": zone,
                "subzone": zone,
                "position": {"x": x, "y": y, "z": z},
                "facing": facing,
                "in_instance": in_instance,
            },
            "nearby": nearby,
        },
        "options": options,
    }
    return snapshot


def build_case_entry(
    failure: MinedFailure,
    case_name: str,
    notes_text: str = "",
) -> Dict[str, Any]:
    """Build a `cases.json` entry dict compatible with `eval_plans.py`."""
    source = f"mined trace ({failure.source_file} @ t={failure.t_ms})"
    entry: Dict[str, Any] = {
        "name": case_name,
        "source": source,
    }

    if failure.kind == "missed_dodge":
        dist = float(failure.sample.get("target_dist", 0.0) or 0.0)
        entry["reason"] = "entered combat"
        entry["summary"] = (
            f"{failure.bot} took {failure.damage} damage from {failure.boss}'s "
            f"{failure.spell} ({failure.radius_yd:.0f} yd radius) at {dist:.1f} yd without dodging"
        )
        entry["rule"] = f"writes a standing order reacting to {failure.spell} by moving out/away"
        entry["notes"] = notes_text or (
            f"{failure.boss}, {failure.spell}: {failure.radius_yd:.0f} yd AoE cast; "
            f"move out immediately when cast."
        )
        entry["order_names"] = list(dict.fromkeys(n for n in (failure.spell, failure.boss) if n))
        entry["min_orders"] = 1
        entry["must_order_contain"] = [failure.spell]

    elif failure.kind == "death":
        hp = float(failure.sample.get("hp_pct", 0.0) or 0.0)
        cast_info = f" during {failure.spell}" if failure.spell else ""
        entry["reason"] = "entered combat"
        entry["summary"] = (
            f"{failure.bot} died to {failure.killer} (last HP {hp:.0f}%{cast_info})"
        )
        if failure.spell:
            entry["rule"] = f"writes a standing order for {failure.spell} and keeps fighting"
            entry["notes"] = notes_text or (
                f"{failure.boss}, {failure.spell}: lethal boss mechanic; react immediately."
            )
            entry["order_names"] = list(dict.fromkeys(n for n in (failure.spell, failure.boss) if n))
            entry["min_orders"] = 1
            entry["must_order_contain"] = [failure.spell]
        else:
            entry["rule"] = "stays in combat (Attack or Keep); never Rest/Explore/Town"
            if notes_text:
                entry["notes"] = notes_text
            entry["order_names"] = [failure.boss] if failure.boss else []
            entry["forbid_kinds"] = ["rest", "explore", "go to town"]

    else:  # stall
        entry["reason"] = "plan done"
        entry["summary"] = (
            f"{failure.bot} stalled idle out of combat for {failure.stall_s:.0f}s "
            f"in {failure.sample.get('zone', 'unknown zone')}"
        )
        entry["rule"] = "moves on (Explore or Go to town); does not Keep idling or Rest at full HP"
        entry["notes"] = notes_text
        entry["order_names"] = []
        entry["first_kinds"] = ["explore", "go to town"]
        entry["forbid_kinds"] = ["keep doing what I'm doing"]

    return entry


def mine_directory(
    trace_dir: Path,
    boss_filter: Optional[str] = None,
    stall_threshold_s: float = 120.0,
    min_damage: int = 1000,
    max_cases: int = 20,
) -> List[MinedFailure]:
    files = discover_trace_files(trace_dir)
    all_failures: List[MinedFailure] = []
    for f in files:
        rows = load_trace_rows(f)
        rel = str(f.relative_to(trace_dir)) if f.is_relative_to(trace_dir) else f.name
        found = mine_trace_rows(
            rows,
            source_file=rel,
            boss_filter=boss_filter,
            stall_threshold_s=stall_threshold_s,
            min_damage=min_damage,
        )
        all_failures.extend(found)
        if len(all_failures) >= max_cases:
            break
    return all_failures[:max_cases]


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Mine failure moments from behavior JSONL traces into eval fixtures."
    )
    parser.add_argument("--trace-dir", type=Path, required=True, help="Root trace directory.")
    parser.add_argument("--boss", type=str, default=None, help="Filter failures by boss name.")
    parser.add_argument("--notes-file", type=Path, default=None, help="Boss notes file to attach.")
    parser.add_argument("--out-dir", type=Path, default=None, help="Output directory for fixtures.")
    parser.add_argument("--max-cases", type=int, default=20, help="Maximum cases to emit.")
    parser.add_argument(
        "--stall-threshold-s",
        type=float,
        default=120.0,
        help="Idle duration threshold in seconds for stall detection (default: 120).",
    )
    parser.add_argument(
        "--min-damage",
        type=int,
        default=1000,
        help="Minimum damage_taken value for missed_dodge detection (default: 1000).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report mined failures without writing any files.",
    )
    args = parser.parse_args(argv)

    if not args.dry_run and args.out_dir is None:
        print("Error: --out-dir is required unless --dry-run is set.", file=sys.stderr)
        return 2

    notes_text = ""
    if args.notes_file is not None:
        notes_text = args.notes_file.read_text(encoding="utf-8").strip()

    failures = mine_directory(
        args.trace_dir,
        boss_filter=args.boss,
        stall_threshold_s=args.stall_threshold_s,
        min_damage=args.min_damage,
        max_cases=args.max_cases,
    )

    cases: List[Dict[str, Any]] = []
    used_names: Dict[str, int] = {}

    for idx, failure in enumerate(failures, 1):
        parts = ["mined", failure.kind]
        if failure.boss:
            parts.append(_slugify(failure.boss))
        if failure.spell:
            parts.append(_slugify(failure.spell))
        base_name = "_".join(parts)
        count = used_names.get(base_name, 0) + 1
        used_names[base_name] = count
        case_name = f"{base_name}_{count}"

        snap = build_snapshot(failure, seq=idx)
        entry = build_case_entry(failure, case_name, notes_text=notes_text)
        cases.append(entry)

        if args.dry_run:
            print(f"[dry-run] {case_name}: {entry['summary']}")
        else:
            assert args.out_dir is not None
            args.out_dir.mkdir(parents=True, exist_ok=True)
            (args.out_dir / f"{case_name}.snapshot.json").write_text(
                json.dumps(snap, indent=2) + "\n", encoding="utf-8"
            )
            (args.out_dir / f"{case_name}.facts.json").write_text("{}\n", encoding="utf-8")

    if not args.dry_run and args.out_dir is not None:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "cases.json").write_text(
            json.dumps(cases, indent=2) + "\n", encoding="utf-8"
        )
        print(f"Wrote {len(cases)} mined eval cases to {args.out_dir}")
    else:
        print(f"Found {len(cases)} mined eval cases (dry-run)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
