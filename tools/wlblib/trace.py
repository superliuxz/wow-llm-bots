"""Shared behavior trace loader (`<trace_dir>/<role>/<name>/*.jsonl`)."""

from __future__ import annotations

import json
import pathlib
from typing import Any


def load_jsonl_files(paths: list[pathlib.Path]) -> list[dict[str, Any]]:
    """Loads JSONL trace rows from `paths`, sorted by `(t_ms, mono_us)`."""
    rows: list[dict[str, Any]] = []
    for f in sorted(paths):
        with f.open(encoding="utf-8", errors="replace") as fh:
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
    rows.sort(key=lambda r: (int(r.get("t_ms", 0)), int(r.get("mono_us", 0))))
    return rows


def load_traces(trace_dir: pathlib.Path) -> dict[str, dict[str, list[dict[str, Any]]]]:
    """Loads `<trace_dir>/<role>/<name>/*.jsonl` sorted by `(t_ms, mono_us)`."""
    out: dict[str, dict[str, list[dict[str, Any]]]] = {"human": {}, "bot": {}}
    if not trace_dir.exists():
        return out
    for role in ("human", "bot"):
        rdir = trace_dir / role
        if not rdir.is_dir():
            continue
        for cdir in sorted(rdir.iterdir()):
            if not cdir.is_dir():
                continue
            rows = load_jsonl_files(list(cdir.glob("*.jsonl")))
            if rows:
                out[role][cdir.name] = rows
    return out


def filter_role_rows(
    traces: dict[str, list[dict[str, Any]]], expected_role: str
) -> dict[str, list[dict[str, Any]]]:
    """Keeps only records whose embedded `role` matches `expected_role`."""
    out: dict[str, list[dict[str, Any]]] = {}
    for name, rows in traces.items():
        filtered = [r for r in rows if r.get("role") == expected_role]
        if filtered:
            out[name] = filtered
    return out


def summarize_combat_share(
    traces: dict[str, dict[str, list[dict[str, Any]]]],
    human_name: str,
    bot_names: tuple[str, ...] | list[str],
    since_t_ms: int = 0,
) -> dict[str, Any]:
    """Summarizes human vs. bot damage dealt and killing blows from loaded traces.

    Counts `damage_dealt` and `killing_blow` events with `t_ms >= since_t_ms`.
    Returns totals, per-character breakdowns, and `bot_damage_share` / `bot_kill_share`
    (None when total is 0).
    """
    def _count(rows: list[dict[str, Any]]) -> tuple[int, int]:
        dmg = 0
        kills = 0
        for r in rows:
            if r.get("kind") != "event":
                continue
            if int(r.get("t_ms", 0)) < since_t_ms:
                continue
            ev = r.get("event")
            if ev == "damage_dealt":
                dmg += max(0, int(r.get("value", 0)))
            elif ev == "killing_blow":
                kills += 1
        return dmg, kills

    human_dmg, human_kills = _count(traces.get("human", {}).get(human_name, []))
    bots_dmg: dict[str, int] = {}
    bots_kills: dict[str, int] = {}
    for b in bot_names:
        d, k = _count(traces.get("bot", {}).get(b, []))
        bots_dmg[b] = d
        bots_kills[b] = k

    bot_dmg_total = sum(bots_dmg.values())
    bot_kills_total = sum(bots_kills.values())
    total_dmg = human_dmg + bot_dmg_total
    total_kills = human_kills + bot_kills_total
    return {
        "human_name": human_name,
        "human_damage": human_dmg,
        "human_kills": human_kills,
        "bots_damage": bots_dmg,
        "bots_kills": bots_kills,
        "bot_damage_total": bot_dmg_total,
        "bot_kills_total": bot_kills_total,
        "total_damage": total_dmg,
        "total_kills": total_kills,
        "bot_damage_share": (bot_dmg_total / total_dmg) if total_dmg > 0 else None,
        "bot_kill_share": (bot_kills_total / total_kills) if total_kills > 0 else None,
    }
