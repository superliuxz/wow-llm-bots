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
