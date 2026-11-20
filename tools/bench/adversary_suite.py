#!/usr/bin/env python3
"""Explainable MMO adversary suite & per-situation diagnostic classifier (Layer C).

Implements `docs/plans/self-improving-loop.md` section 3C:
  **DIAGNOSTIC ONLY — NEVER USED AS A REWARD OR KEEP/REVERT SCORE.**

Features drawn from published MMO anti-bot research:
  1. Per-situation `scikit-learn` classifier on canonical 5 Hz observer-view
     clip features (retrained fresh every round on held-out humans vs bots),
     reporting cross-validated accuracy + 95% CI, ROC-AUC, and top feature
     importances.
  2. Server-side waypoint / route repetition (Mitterhofer et al. 2009, WoW):
     Douglas-Peucker turning-point extraction + repeated segment ratio.
  3. Inter-action timing regularity / entropy (Gianvecchio et al. "Battle of
     Botcraft"; NCSoft Aion 2023 action-interval detector).
  4. Cross-window self-similarity (Lee et al., NDSS 2016): cosine similarity of
     activity-transition histograms across windows.
  5. Chat & whisper diagnostics: reply delay vs text length, polish rate, and
     phrasing repetition.
  6. Per-situation diagnostic distributions (p10 / p50 / p90, never scalar
     averages or rewards).
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import pathlib
import random
import statistics
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))

# Auto-discover dedicated bench venv site-packages so `tools/wlb` and plain
# `python3` can use scikit-learn without manual venv activation.
for _sp in sorted(glob.glob(str(ROOT / "var" / "bench-venv" / "lib" / "python*" / "site-packages"))):
    if _sp not in sys.path:
        sys.path.append(_sp)

import trace_parity  # noqa: E402

try:
    import numpy as _np
    from sklearn.ensemble import RandomForestClassifier as _RF
    from sklearn.metrics import roc_auc_score as _roc_auc
    from sklearn.model_selection import StratifiedKFold as _SKF

    HAS_SKLEARN = True
except Exception:
    HAS_SKLEARN = False


FEATURE_NAMES = [
    "moving_frac",
    "speed_mean",
    "speed_std",
    "turn_rate_med",
    "turn_rate_p90",
    "snap_turn_rate",
    "straightness",
    "stops_per_min",
    "jumps_per_min",
    "jump_interval_cv",
    "action_interval_cv",
    "action_interval_entropy",
    "combat_frac",
    "cast_frac",
]


def _pct(vals: list[float], q: float) -> float | None:
    if not vals:
        return None
    s = sorted(vals)
    idx = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
    return float(s[idx])


def _shannon_entropy(vals: list[float], n_bins: int = 8) -> float:
    """Normalized Shannon entropy in [0, 1] over positive intervals."""
    if len(vals) < 3:
        return 0.0
    lo, hi = min(vals), max(vals)
    if hi - lo < 1e-6:
        return 0.0
    bins = [0] * n_bins
    for v in vals:
        b = min(n_bins - 1, int((v - lo) / (hi - lo) * n_bins))
        bins[b] += 1
    n = len(vals)
    ent = 0.0
    for c in bins:
        if c > 0:
            p = c / n
            ent -= p * math.log2(p)
    return ent / math.log2(n_bins)


def _cv(vals: list[float]) -> float:
    if len(vals) < 2:
        return 0.0
    m = statistics.mean(vals)
    if abs(m) < 1e-6:
        return 0.0
    return statistics.stdev(vals) / abs(m)


def extract_clip_features(clip: dict[str, Any]) -> dict[str, float]:
    """Extracts canonical, packet-agnostic kinematic & event features from a clip."""
    frames = clip.get("frames", [])
    events = clip.get("events", [])
    dur_min = max(float(clip.get("duration_s", 0.0)) / 60.0, 1e-6)
    if len(frames) < 2:
        return {k: 0.0 for k in FEATURE_NAMES}

    speeds = [float(f["speed_yd_s"]) for f in frames]
    mov_speeds = [s for s in speeds if s >= 0.5]
    turns = [float(f["turn_rate_rad_s"]) for f in frames if float(f["turn_rate_rad_s"]) > 0.05]
    snap_turns = sum(1 for f in frames if float(f["turn_rate_rad_s"]) > (math.pi / 2.0) / 0.2)

    # Path straightness over moving segments.
    seg_vals: list[float] = []
    seg_start = None
    seg_len = 0.0
    stops = 0
    jump_ts: list[float] = []
    for i in range(1, len(frames)):
        prev, cur = frames[i - 1], frames[i]
        step_d = math.hypot(cur["x"] - prev["x"], cur["y"] - prev["y"])
        if cur["moving"]:
            if seg_start is None:
                seg_start = (prev["x"], prev["y"])
                seg_len = 0.0
            seg_len += step_d
        else:
            if prev["moving"]:
                stops += 1
            if seg_start is not None and seg_len >= 5.0:
                net = math.hypot(prev["x"] - seg_start[0], prev["y"] - seg_start[1])
                seg_vals.append(min(1.0, net / seg_len))
            seg_start = None
            seg_len = 0.0
        if cur["jumping"] and not prev["jumping"]:
            jump_ts.append(cur["t_ms"] / 1000.0)
    if seg_start is not None and seg_len >= 5.0:
        last = frames[-1]
        net = math.hypot(last["x"] - seg_start[0], last["y"] - seg_start[1])
        seg_vals.append(min(1.0, net / seg_len))

    jump_dts = [jump_ts[i] - jump_ts[i - 1] for i in range(1, len(jump_ts))]
    ev_ts = sorted(e["t_ms"] / 1000.0 for e in events)
    ev_dts = [ev_ts[i] - ev_ts[i - 1] for i in range(1, len(ev_ts)) if ev_ts[i] > ev_ts[i - 1]]

    return {
        "moving_frac": sum(1 for f in frames if f["moving"]) / len(frames),
        "speed_mean": statistics.mean(mov_speeds) if mov_speeds else 0.0,
        "speed_std": statistics.stdev(mov_speeds) if len(mov_speeds) >= 2 else 0.0,
        "turn_rate_med": statistics.median(turns) if turns else 0.0,
        "turn_rate_p90": _pct(turns, 0.9) or 0.0,
        "snap_turn_rate": snap_turns / dur_min,
        "straightness": statistics.mean(seg_vals) if seg_vals else 0.0,
        "stops_per_min": stops / dur_min,
        "jumps_per_min": len(jump_ts) / dur_min,
        "jump_interval_cv": _cv(jump_dts),
        "action_interval_cv": _cv(ev_dts),
        "action_interval_entropy": _shannon_entropy(ev_dts),
        "combat_frac": sum(1 for f in frames if f["in_combat"]) / len(frames),
        "cast_frac": sum(1 for f in frames if f["casting"]) / len(frames),
    }


def _douglas_peucker(pts: list[tuple[float, float]], eps: float = 2.0) -> list[tuple[float, float]]:
    """Extracts turning-point waypoints (Mitterhofer et al. 2009)."""
    if len(pts) <= 2:
        return pts
    x0, y0 = pts[0]
    x1, y1 = pts[-1]
    dx, dy = x1 - x0, y1 - y0
    denom = math.hypot(dx, dy)
    max_d = -1.0
    max_i = 0
    for i in range(1, len(pts) - 1):
        px, py = pts[i]
        if denom < 1e-6:
            d = math.hypot(px - x0, py - y0)
        else:
            d = abs(dy * px - dx * py + x1 * y0 - y1 * x0) / denom
        if d > max_d:
            max_d = d
            max_i = i
    if max_d > eps:
        left = _douglas_peucker(pts[: max_i + 1], eps)
        right = _douglas_peucker(pts[max_i:], eps)
        return left[:-1] + right
    return [pts[0], pts[-1]]


def waypoint_repetition_score(rows: list[dict[str, Any]], cell_yd: float = 4.0) -> dict[str, Any]:
    """Mitterhofer et al. (2009) server-side WoW waypoint repetition detector.

    Quantizes Douglas-Peucker turning-point edges `(cell_a -> cell_b)` and
    computes the fraction of directed waypoint transitions repeated >= 2 times.
    """
    segs = trace_parity.reconstruct_trajectory(rows)
    edge_counts: dict[tuple[tuple[int, int], tuple[int, int]], int] = {}
    total_edges = 0
    for seg in segs:
        pts = [(f["x"], f["y"]) for f in seg["frames"] if f["moving"]]
        if len(pts) < 4:
            continue
        wps = _douglas_peucker(pts, eps=2.0)
        cells = [(int(round(x / cell_yd)), int(round(y / cell_yd))) for x, y in wps]
        for i in range(1, len(cells)):
            if cells[i] == cells[i - 1]:
                continue
            edge = (cells[i - 1], cells[i])
            edge_counts[edge] = edge_counts.get(edge, 0) + 1
            total_edges += 1
    if total_edges == 0:
        return {"total_edges": 0, "unique_edges": 0, "repeat_ratio": 0.0}
    repeated_edges = sum(c - 1 for c in edge_counts.values() if c > 1)
    return {
        "total_edges": total_edges,
        "unique_edges": len(edge_counts),
        "repeat_ratio": round(repeated_edges / total_edges, 3),
    }


def self_similarity_score(rows: list[dict[str, Any]], window_s: float = 60.0) -> float:
    """Lee et al. (NDSS 2016) cross-window activity self-similarity in [0, 1]."""
    segs = trace_parity.reconstruct_trajectory(rows)
    win_vecs: list[list[float]] = []
    for seg in segs:
        frames = seg["frames"]
        events = seg["events"]
        labels = trace_parity.label_situations(frames, events)
        step = max(2, int(round(window_s * 1000.0 / seg["step_ms"])))
        for start in range(0, len(labels) - step + 1, step):
            chunk = labels[start : start + step]
            # Activity proportions + transition count per situation.
            vec = [0.0] * len(trace_parity.SITUATIONS)
            idx_map = {s: i for i, s in enumerate(trace_parity.SITUATIONS)}
            for lb in chunk:
                vec[idx_map[lb]] += 1.0
            norm = math.hypot(*vec)
            if norm > 0:
                win_vecs.append([v / norm for v in vec])
    if len(win_vecs) < 2:
        return 0.0
    sims: list[float] = []
    for i in range(len(win_vecs)):
        for j in range(i + 1, len(win_vecs)):
            sims.append(sum(a * b for a, b in zip(win_vecs[i], win_vecs[j])))
    return round(statistics.mean(sims), 3)


def chat_diagnostics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """Whisper reply timing, length, polish rate, and phrasing repetition."""
    chats = [r for r in rows if r.get("kind") == "chat"]
    pending: dict[str, int] = {}
    delays: list[float] = []
    lens: list[int] = []
    texts: list[str] = []
    polished = 0
    for c in chats:
        peer = str(c.get("peer", ""))
        if c.get("dir") == "in":
            pending[peer] = int(c.get("t_ms", 0))
        elif c.get("dir") == "out":
            txt = str(c.get("text", "")).strip()
            tlen = int(c["text_len"]) if "text_len" in c else len(txt)
            lens.append(tlen)
            if txt:
                texts.append(txt)
                # Polish heuristic: starts uppercase AND ends with sentence punctuation.
                if txt[0].isupper() and txt[-1] in ".!?" and len(txt) >= 15:
                    polished += 1
            if peer in pending:
                d = (int(c.get("t_ms", 0)) - pending.pop(peer)) / 1000.0
                if 0.0 <= d <= 120.0:
                    delays.append(d)
    unique_ratio = (len(set(t.lower() for t in texts)) / len(texts)) if texts else None
    return {
        "outbound_count": len(lens),
        "reply_delay_p10_s": round(_pct(delays, 0.1), 2) if delays else None,
        "reply_delay_p50_s": round(_pct(delays, 0.5), 2) if delays else None,
        "reply_delay_p90_s": round(_pct(delays, 0.9), 2) if delays else None,
        "min_reply_delay_s": round(min(delays), 2) if delays else None,
        "polish_rate": round(polished / len(texts), 3) if texts else None,
        "unique_phrase_ratio": round(unique_ratio, 3) if unique_ratio is not None else None,
    }


def reaction_times_ms(rows: list[dict[str, Any]]) -> list[float]:
    """Measures `damage_taken` / `combat_start` -> first cast or move change (ms)."""
    segs = trace_parity.reconstruct_trajectory(rows)
    reacts: list[float] = []
    for seg in segs:
        frames = seg["frames"]
        events = seg["events"]
        triggers = [e["t_rel_ms"] for e in events if e["event"] in ("damage_taken", "combat_start")]
        casts = [e["t_rel_ms"] for e in events if e["event"] == "cast_start"]
        frame_by_t = {f["t_rel_ms"]: f for f in frames}
        for t0 in triggers:
            # First cast or significant turn/move within 5 s after trigger.
            best: float | None = None
            for ct in casts:
                if t0 < ct <= t0 + 5000:
                    best = float(ct - t0)
                    break
            f0 = frame_by_t.get(t0)
            if f0 is not None:
                for dt in range(seg["step_ms"], 5001, seg["step_ms"]):
                    fk = frame_by_t.get(t0 + dt)
                    if fk is None:
                        break
                    if fk["turn_rate_rad_s"] > 1.0 or (fk["moving"] != f0["moving"]):
                        if best is None or dt < best:
                            best = float(dt)
                        break
            if best is not None:
                reacts.append(best)
    return reacts


def fit_situation_classifier(
    human_clips: list[dict[str, Any]],
    bot_clips: list[dict[str, Any]],
    seed: int = 0,
) -> dict[str, Any]:
    """Retrains a fresh per-situation RandomForest classifier (diagnostic only).

    Reports 5-fold CV accuracy + bootstrapped 95% CI, ROC-AUC, and top feature
    importances explaining *what* distinguishes bots from humans.
    """
    n_h, n_b = len(human_clips), len(bot_clips)
    if n_h < 4 or n_b < 4:
        return {
            "status": "insufficient_clips",
            "human_clips": n_h,
            "bot_clips": n_b,
        }
    if not HAS_SKLEARN:
        return {
            "status": "sklearn_unavailable",
            "human_clips": n_h,
            "bot_clips": n_b,
        }

    X = _np.array(
        [[extract_clip_features(c)[f] for f in FEATURE_NAMES] for c in human_clips + bot_clips],
        dtype=float,
    )
    y = _np.array([1] * n_h + [0] * n_b, dtype=int)

    n_splits = min(5, n_h, n_b)
    skf = _SKF(n_splits=n_splits, shuffle=True, random_state=seed)
    probs = _np.zeros(len(y), dtype=float)
    preds = _np.zeros(len(y), dtype=int)
    imp_sum = _np.zeros(len(FEATURE_NAMES), dtype=float)

    for tr, te in skf.split(X, y):
        clf = _RF(n_estimators=60, max_depth=5, random_state=seed)
        clf.fit(X[tr], y[tr])
        p = clf.predict_proba(X[te])[:, 1]
        probs[te] = p
        preds[te] = (p >= 0.5).astype(int)
        imp_sum += clf.feature_importances_

    acc = float((preds == y).mean())
    auc = float(_roc_auc(y, probs))

    # Bootstrap 95% CI on out-of-fold accuracy.
    rng = random.Random(seed)
    correct = (preds == y).tolist()
    m = len(correct)
    boots = sorted(
        sum(correct[rng.randrange(m)] for _ in range(m)) / m
        for _ in range(300)
    )
    ci_lo = boots[int(0.025 * (len(boots) - 1))]
    ci_hi = boots[int(0.975 * (len(boots) - 1))]

    importances = imp_sum / n_splits
    top_features = sorted(
        [
            {"feature": FEATURE_NAMES[i], "importance": round(float(importances[i]), 3)}
            for i in range(len(FEATURE_NAMES))
        ],
        key=lambda d: -d["importance"],
    )[:5]

    return {
        "status": "ok",
        "human_clips": n_h,
        "bot_clips": n_b,
        "accuracy": round(acc, 3),
        "acc_ci_low": round(ci_lo, 3),
        "acc_ci_high": round(ci_hi, 3),
        "roc_auc": round(auc, 3),
        "top_tells": top_features,
    }


def run_adversary_suite(
    bot_traces: dict[str, list[dict[str, Any]]],
    human_traces: dict[str, list[dict[str, Any]]],
    clip_s: float = 45.0,
    seed: int = 0,
) -> dict[str, Any]:
    """Runs full Layer C diagnostic suite across all situations and characters."""
    human_by_sit: dict[str, list[dict[str, Any]]] = {}
    bot_by_sit: dict[str, list[dict[str, Any]]] = {}

    for rows in human_traces.values():
        for c in trace_parity.extract_clips(rows, clip_s=clip_s):
            human_by_sit.setdefault(c["situation"], []).append(c)
    for rows in bot_traces.values():
        for c in trace_parity.extract_clips(rows, clip_s=clip_s):
            bot_by_sit.setdefault(c["situation"], []).append(c)

    per_situation: dict[str, Any] = {}
    for sit in trace_parity.SITUATIONS:
        hc = human_by_sit.get(sit, [])
        bc = bot_by_sit.get(sit, [])
        if not hc and not bc:
            continue
        clf_res = fit_situation_classifier(hc, bc, seed=seed)
        # Per-situation diagnostic distributions (p10/p50/p90).
        b_feats = [extract_clip_features(c) for c in bc]
        h_feats = [extract_clip_features(c) for c in hc]
        dists = {}
        for k in ("straightness", "turn_rate_med", "snap_turn_rate", "jumps_per_min", "action_interval_entropy"):
            bv = [f[k] for f in b_feats]
            hv = [f[k] for f in h_feats]
            dists[k] = {
                "bot_p10_p50_p90": [round(_pct(bv, q), 2) for q in (0.1, 0.5, 0.9)] if bv else None,
                "human_p10_p50_p90": [round(_pct(hv, q), 2) for q in (0.1, 0.5, 0.9)] if hv else None,
            }
        per_situation[sit] = {
            "classifier": clf_res,
            "distributions": dists,
        }

    # Published MMO adversary features across full traces.
    bot_wp = [waypoint_repetition_score(r)["repeat_ratio"] for r in bot_traces.values()]
    human_wp = [waypoint_repetition_score(r)["repeat_ratio"] for r in human_traces.values()]
    bot_ss = [self_similarity_score(r) for r in bot_traces.values()]
    human_ss = [self_similarity_score(r) for r in human_traces.values()]
    bot_reacts = [x for r in bot_traces.values() for x in reaction_times_ms(r)]
    human_reacts = [x for r in human_traces.values() for x in reaction_times_ms(r)]

    bot_chat_all = [r for rows in bot_traces.values() for r in rows if r.get("kind") == "chat"]
    human_chat_all = [r for rows in human_traces.values() for r in rows if r.get("kind") == "chat"]

    tells: list[str] = []
    mean_b_wp = statistics.mean(bot_wp) if bot_wp else 0.0
    if mean_b_wp >= 0.45:
        tells.append(f"waypoint repetition high (Mitterhofer repeat_ratio={mean_b_wp:.2f})")
    mean_b_ss = statistics.mean(bot_ss) if bot_ss else 0.0
    if mean_b_ss >= 0.92:
        tells.append(f"cross-window self-similarity high (Lee NDSS16={mean_b_ss:.2f})")
    b_chat = chat_diagnostics(bot_chat_all)
    if b_chat["min_reply_delay_s"] is not None and b_chat["min_reply_delay_s"] < 0.8:
        tells.append(f"sub-human instant whisper reply ({b_chat['min_reply_delay_s']:.2f}s)")
    if b_chat["polish_rate"] is not None and b_chat["polish_rate"] >= 0.8 and b_chat["outbound_count"] >= 3:
        tells.append(f"overly polished chat prose (polish_rate={b_chat['polish_rate']:.2f})")

    for sit, info in per_situation.items():
        clf = info["classifier"]
        if clf.get("status") == "ok" and clf.get("accuracy", 0.0) >= 0.75:
            top = ", ".join(f"{t['feature']}({t['importance']:.2f})" for t in clf["top_tells"][:3])
            tells.append(f"[{sit}] classifier acc={clf['accuracy']*100:.0f}% (AUC={clf['roc_auc']:.2f}); top tells: {top}")

    return {
        "per_situation": per_situation,
        "mmo_detectors": {
            "waypoint_repeat_ratio": {
                "bot_mean": round(mean_b_wp, 3),
                "human_mean": round(statistics.mean(human_wp), 3) if human_wp else None,
            },
            "self_similarity": {
                "bot_mean": round(mean_b_ss, 3),
                "human_mean": round(statistics.mean(human_ss), 3) if human_ss else None,
            },
            "reaction_ms_p10_p50_p90": {
                "bot": [round(_pct(bot_reacts, q), 0) for q in (0.1, 0.5, 0.9)] if bot_reacts else None,
                "human": [round(_pct(human_reacts, q), 0) for q in (0.1, 0.5, 0.9)] if human_reacts else None,
            },
            "chat": {
                "bot": b_chat,
                "human": chat_diagnostics(human_chat_all),
            },
        },
        "tells": tells,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Run Layer C diagnostic adversary suite")
    ap.add_argument("bot_dir", type=pathlib.Path)
    ap.add_argument("--human-dir", type=pathlib.Path, required=True)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    b_tr = trace_parity.load_traces(args.bot_dir)["bot"]
    h_tr = trace_parity.load_traces(args.human_dir)["human"]
    res = run_adversary_suite(b_tr, h_tr)
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print("== Layer C Adversary Diagnostics (never a reward) ==")
        for t in res["tells"] or ["(no strong tells flagged)"]:
            print(f"  - {t}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
