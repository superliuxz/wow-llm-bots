#!/usr/bin/env python3
"""Long-horizon & population-level anti-bot diagnostics (Step 5).

Implements `docs/plans/self-improving-loop.md` sections 3C/4.5:
  - Multi-session route repetition per character (Mitterhofer et al. 2009).
  - Inter-bot vs inter-human behavioral clustering / similarity (NCSoft Lineage W
    2024): flags when bots behave like clones of each other.
  - Population chat phrasing diversity (unique ratio & cross-bot n-gram overlap).
  - Session length & schedule summary, with optional comparison against the
    WoW Avatar History dataset (MMSys 2011) if provided.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import statistics
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import adversary_suite  # noqa: E402
import trace_parity  # noqa: E402


def _char_feature_vector(rows: list[dict[str, Any]], clip_s: float = 45.0) -> list[float] | None:
    clips = trace_parity.extract_clips(rows, clip_s=clip_s)
    if not clips:
        return None
    feats = [adversary_suite.extract_clip_features(c) for c in clips]
    vec = [
        statistics.mean(f[k] for f in feats)
        for k in adversary_suite.FEATURE_NAMES
    ]
    return vec


def _pairwise_cosine_similarity(vecs: dict[str, list[float]]) -> float | None:
    """Computes mean pairwise cosine similarity across z-normalized character vectors."""
    names = sorted(vecs.keys())
    if len(names) < 2:
        return None
    dim = len(vecs[names[0]])
    # Normalize each feature dimension to unit scale so speed/straightness/rates
    # contribute comparably.
    scales = []
    for d in range(dim):
        col = [abs(vecs[n][d]) for n in names]
        scales.append(max(col) if max(col) > 1e-6 else 1.0)
    normed = []
    for n in names:
        v = [vecs[n][d] / scales[d] for d in range(dim)]
        mag = math.hypot(*v)
        normed.append([x / mag for x in v] if mag > 0 else v)

    sims = []
    for i in range(len(normed)):
        for j in range(i + 1, len(normed)):
            sims.append(sum(a * b for a, b in zip(normed[i], normed[j])))
    return round(statistics.mean(sims), 3) if sims else None


def chat_population_diversity(traces: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    """Measures unique phrasing and cross-character bigram overlap across outbound chat."""
    per_char_msgs: dict[str, list[str]] = {}
    all_msgs: list[str] = []
    for name, rows in traces.items():
        msgs = [
            str(r.get("text", "")).strip().lower()
            for r in rows
            if r.get("kind") == "chat" and r.get("dir") == "out" and str(r.get("text", "")).strip()
        ]
        if msgs:
            per_char_msgs[name] = msgs
            all_msgs.extend(msgs)

    if not all_msgs:
        return {"messages": 0, "unique_ratio": None, "cross_char_bigram_overlap": None}

    unique_ratio = len(set(all_msgs)) / len(all_msgs)

    def bigrams(msgs: list[str]) -> set[tuple[str, str]]:
        out: set[tuple[str, str]] = set()
        for m in msgs:
            toks = m.split()
            for i in range(len(toks) - 1):
                out.add((toks[i], toks[i + 1]))
        return out

    names = sorted(per_char_msgs.keys())
    overlaps: list[float] = []
    for i in range(len(names)):
        bi = bigrams(per_char_msgs[names[i]])
        if not bi:
            continue
        for j in range(i + 1, len(names)):
            bj = bigrams(per_char_msgs[names[j]])
            if not bj:
                continue
            overlaps.append(len(bi & bj) / len(bi | bj))

    return {
        "messages": len(all_msgs),
        "unique_ratio": round(unique_ratio, 3),
        "cross_char_bigram_overlap": round(statistics.mean(overlaps), 3) if overlaps else None,
    }


def session_durations_min(traces: dict[str, list[dict[str, Any]]], gap_s: float = 300.0) -> list[float]:
    """Splits each character's trace into sessions separated by >= `gap_s` and returns lengths (min)."""
    durs: list[float] = []
    gap_ms = int(gap_s * 1000.0)
    for rows in traces.values():
        ts = sorted(int(r["t_ms"]) for r in rows if r.get("kind") == "sample" and "t_ms" in r)
        if len(ts) < 2:
            continue
        start = ts[0]
        prev = ts[0]
        for t in ts[1:]:
            if t - prev > gap_ms:
                if prev > start:
                    durs.append((prev - start) / 60_000.0)
                start = t
            prev = t
        if prev > start:
            durs.append((prev - start) / 60_000.0)
    return [round(d, 2) for d in durs]


def load_avatar_history_sessions(path: pathlib.Path) -> list[float]:
    """Loads optional WoW Avatar History CSV/TSV (`char,level,zone,timestamp` or `session_min`)."""
    if not path.exists():
        return []
    durs: list[float] = []
    with path.open("r", newline="") as f:
        dialect = csv.Sniffer().sniff(f.read(2048), delimiters=",\t;")
        f.seek(0)
        reader = csv.DictReader(f, dialect=dialect)
        if reader.fieldnames and "session_min" in reader.fieldnames:
            for row in reader:
                try:
                    durs.append(float(row["session_min"]))
                except (ValueError, TypeError):
                    continue
    return durs


def analyze_population(
    bot_traces: dict[str, list[dict[str, Any]]],
    human_traces: dict[str, list[dict[str, Any]]] | None = None,
    avatar_history_path: pathlib.Path | None = None,
    clip_s: float = 45.0,
) -> dict[str, Any]:
    """Computes long-horizon & population-level behavioral metrics."""
    human_traces = human_traces or {}
    bot_vecs = {
        n: v for n, r in bot_traces.items()
        if (v := _char_feature_vector(r, clip_s=clip_s)) is not None
    }
    human_vecs = {
        n: v for n, r in human_traces.items()
        if (v := _char_feature_vector(r, clip_s=clip_s)) is not None
    }

    bot_sim = _pairwise_cosine_similarity(bot_vecs)
    human_sim = _pairwise_cosine_similarity(human_vecs)
    bot_wp = {n: adversary_suite.waypoint_repetition_score(r)["repeat_ratio"] for n, r in bot_traces.items()}
    bot_chat = chat_population_diversity(bot_traces)
    bot_sessions = session_durations_min(bot_traces)
    human_sessions = session_durations_min(human_traces)
    avatar_sessions = (
        load_avatar_history_sessions(avatar_history_path)
        if avatar_history_path
        else []
    )

    tells: list[str] = []
    if bot_sim is not None and bot_sim >= 0.96:
        tells.append(
            f"multi-bot behavioral cloning (inter-bot cosine similarity={bot_sim:.3f}"
            + (f" vs human={human_sim:.3f}" if human_sim is not None else "")
            + ")"
        )
    if bot_chat["unique_ratio"] is not None and bot_chat["unique_ratio"] < 0.6 and bot_chat["messages"] >= 5:
        tells.append(f"low chat phrasing diversity (unique_ratio={bot_chat['unique_ratio']:.2f})")

    return {
        "bot_characters": len(bot_traces),
        "human_characters": len(human_traces),
        "inter_bot_similarity": bot_sim,
        "inter_human_similarity": human_sim,
        "per_bot_waypoint_repeat": bot_wp,
        "chat_diversity": bot_chat,
        "bot_session_min": {
            "count": len(bot_sessions),
            "median": round(statistics.median(bot_sessions), 2) if bot_sessions else None,
        },
        "human_session_min": {
            "count": len(human_sessions),
            "median": round(statistics.median(human_sessions), 2) if human_sessions else None,
        },
        "avatar_history_median_min": (
            round(statistics.median(avatar_sessions), 2) if avatar_sessions else None
        ),
        "tells": tells,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze population & long-horizon bot tells")
    ap.add_argument("bot_dir", type=pathlib.Path)
    ap.add_argument("--human-dir", type=pathlib.Path, default=None)
    ap.add_argument("--avatar-history", type=pathlib.Path, default=None)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    b_tr = trace_parity.load_traces(args.bot_dir)["bot"]
    h_tr = trace_parity.load_traces(args.human_dir)["human"] if args.human_dir else {}
    res = analyze_population(b_tr, h_tr, avatar_history_path=args.avatar_history)
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print(
            f"== Population metrics (bots={res['bot_characters']}, "
            f"inter_bot_sim={res['inter_bot_similarity']}, "
            f"chat_unique={res['chat_diversity']['unique_ratio']}) =="
        )
        for t in res["tells"] or ["(no population tells flagged)"]:
            print(f"  - {t}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
