#!/usr/bin/env python3
"""Recording parity: common fixed-time trajectory reconstructor & situation labeller.

Implements `docs/plans/self-improving-loop.md` section F:
  1. Rebuilds both human (`MSG_MOVE_*` heartbeats) and bot (`SMSG_MONSTER_MOVE`
     splines / 5 Hz samples) traces into ONE canonical fixed-time (5 Hz, 200 ms)
     `(pos, facing)` + event timeline so neither packet timing nor packet type
     can leak to any downstream judge or classifier.
  2. Labels every frame into a situation (`in_group`, `chatting`, `solo_fight`,
     `looting`, `in_town`, `travel`) purely from reconstructed state & events.
  3. Extracts metadata-stripped, origin-shifted 30-60 s clips per situation for
     blind pairwise judging and per-situation diagnostics.
"""

from __future__ import annotations

import json
import math
import pathlib
from typing import Any

SITUATIONS = ("in_group", "chatting", "solo_fight", "looting", "in_town", "travel")
DEFAULT_STEP_MS = 200  # 5 Hz fixed grid


def angle_diff(a: float, b: float) -> float:
    """Unsigned shortest angular difference in [0, pi]."""
    d = abs(b - a) % (2.0 * math.pi)
    return min(d, 2.0 * math.pi - d)


def _signed_angle_diff(a: float, b: float) -> float:
    """Signed shortest angular difference from a to b in (-pi, pi]."""
    d = (b - a + math.pi) % (2.0 * math.pi) - math.pi
    return d


def _interp_angle(a: float, b: float, alpha: float) -> float:
    """Shortest-arc linear interpolation between angles a and b."""
    return (a + _signed_angle_diff(a, b) * alpha) % (2.0 * math.pi)


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
            rows: list[dict[str, Any]] = []
            for f in sorted(cdir.glob("*.jsonl")):
                for line in f.read_text().splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rows.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue
            if rows:
                rows.sort(key=lambda r: (int(r.get("t_ms", 0)), int(r.get("mono_us", 0))))
                out[role][cdir.name] = rows
    return out


def reconstruct_trajectory(
    rows: list[dict[str, Any]],
    step_ms: int = DEFAULT_STEP_MS,
    max_gap_s: float = 5.0,
) -> list[dict[str, Any]]:
    """Rebuilds raw samples + events into canonical fixed-time contiguous segments.

    Returns a list of segments:
      {
        "t0_ms": int,            # internal wall-clock start (stripped in clips)
        "step_ms": int,
        "frames": [CanonicalFrame, ...],
        "events": [CanonicalEvent, ...],
      }

    Both human heartbeat traces and bot spline traces pass through this exact
    same resampler so downstream consumers see identical grid spacing,
    quantization, and derived kinematics (speed, turn rate, moving flag).
    """
    samples = [
        r for r in rows
        if r.get("kind") == "sample"
        and all(k in r for k in ("t_ms", "x", "y", "z"))
    ]
    if len(samples) < 2:
        return []

    # Deduplicate identical timestamps (keep last state at that millisecond).
    dedup: list[dict[str, Any]] = []
    for s in samples:
        if dedup and int(s["t_ms"]) == int(dedup[-1]["t_ms"]):
            dedup[-1] = s
        else:
            dedup.append(s)
    samples = dedup
    if len(samples) < 2:
        return []

    raw_events: list[dict[str, Any]] = []
    for r in rows:
        kind = r.get("kind")
        if kind == "event":
            raw_events.append({
                "t_ms": int(r.get("t_ms", 0)),
                "event": str(r.get("event", "")),
                "value": int(r.get("value", 0)),
            })
        elif kind == "chat":
            direction = str(r.get("dir", ""))
            ev_name = f"chat_{direction}" if direction in ("in", "out") else "chat"
            text = str(r.get("text", ""))
            text_len = int(r["text_len"]) if "text_len" in r else len(text)
            raw_events.append({
                "t_ms": int(r.get("t_ms", 0)),
                "event": ev_name,
                "channel": str(r.get("channel", "whisper")),
                "text_len": text_len,
                "text": text,
                "peer": str(r.get("peer", "")),
            })

    # Split raw samples into contiguous spans separated by gaps > max_gap_s.
    max_gap_ms = int(max_gap_s * 1000.0)
    spans: list[list[dict[str, Any]]] = []
    cur_span: list[dict[str, Any]] = [samples[0]]
    for s in samples[1:]:
        dt = int(s["t_ms"]) - int(cur_span[-1]["t_ms"])
        if dt > max_gap_ms or dt < 0:
            if len(cur_span) >= 2:
                spans.append(cur_span)
            cur_span = [s]
        else:
            cur_span.append(s)
    if len(cur_span) >= 2:
        spans.append(cur_span)

    segments: list[dict[str, Any]] = []
    dt_s = step_ms / 1000.0

    for span in spans:
        t_start = int(span[0]["t_ms"])
        t_end = int(span[-1]["t_ms"])
        n_steps = (t_end - t_start) // step_ms + 1
        if n_steps < 2:
            continue

        # Collect jump pulse timestamps from raw rising edges so brief jumps
        # between 200 ms ticks are never lost by subsampling.
        jump_times: set[int] = set()
        for i, s in enumerate(span):
            if bool(s.get("jumping")) and (i == 0 or not bool(span[i - 1].get("jumping"))):
                grid_idx = round((int(s["t_ms"]) - t_start) / step_ms)
                jump_times.add(max(0, min(n_steps - 1, grid_idx)))

        frames: list[dict[str, Any]] = []
        raw_idx = 0
        for k in range(n_steps):
            t_cur = t_start + k * step_ms
            while raw_idx + 1 < len(span) and int(span[raw_idx + 1]["t_ms"]) < t_cur:
                raw_idx += 1
            s0 = span[raw_idx]
            s1 = span[min(raw_idx + 1, len(span) - 1)]
            t0 = int(s0["t_ms"])
            t1 = int(s1["t_ms"])
            if t1 > t0:
                alpha = max(0.0, min(1.0, (t_cur - t0) / float(t1 - t0)))
            else:
                alpha = 0.0

            x = round(float(s0["x"]) + (float(s1["x"]) - float(s0["x"])) * alpha, 2)
            y = round(float(s0["y"]) + (float(s1["y"]) - float(s0["y"])) * alpha, 2)
            z = round(float(s0["z"]) + (float(s1["z"]) - float(s0["z"])) * alpha, 2)
            o = round(_interp_angle(float(s0.get("o", 0.0)), float(s1.get("o", 0.0)), alpha), 3)

            # State flags taken from nearest sample (no packet-type leakage).
            ref = s0 if alpha < 0.5 else s1
            frames.append({
                "t_rel_ms": k * step_ms,
                "x": x,
                "y": y,
                "z": z,
                "o": o,
                "speed_yd_s": 0.0,
                "turn_rate_rad_s": 0.0,
                "moving": False,
                "jumping": (k in jump_times) or bool(ref.get("jumping")),
                "in_combat": bool(ref.get("in_combat")),
                "dead": bool(ref.get("dead")),
                "in_town": bool(ref.get("in_town")),
                "in_group": bool(ref.get("in_group")),
                "in_dungeon": bool(ref.get("in_dungeon")),
                "hp_pct": round(float(ref.get("hp_pct", 100.0)), 1),
                "mana_pct": round(float(ref.get("mana_pct", 100.0)), 1),
                "casting": bool(ref.get("casting")),
                "attackers": int(ref.get("attackers", 0)),
            })

        # Derive speed, turn rate, and moving purely from the canonical grid.
        for k in range(1, len(frames)):
            prev = frames[k - 1]
            cur = frames[k]
            dist = math.hypot(cur["x"] - prev["x"], cur["y"] - prev["y"])
            speed = round(dist / dt_s, 2)
            da = angle_diff(prev["o"], cur["o"])
            turn_rate = round(da / dt_s, 3)
            cur["speed_yd_s"] = speed
            cur["turn_rate_rad_s"] = turn_rate
            cur["moving"] = speed >= 0.5
        if len(frames) >= 2:
            frames[0]["speed_yd_s"] = frames[1]["speed_yd_s"]
            frames[0]["moving"] = frames[1]["moving"]

        # Align explicit events onto the same t_rel_ms timeline.
        seg_events: list[dict[str, Any]] = []
        for ev in raw_events:
            et = ev["t_ms"]
            if t_start <= et <= t_end:
                snapped = round((et - t_start) / step_ms) * step_ms
                ce: dict[str, Any] = {
                    "t_rel_ms": int(snapped),
                    "event": ev["event"],
                }
                if "value" in ev:
                    ce["value"] = ev["value"]
                if "channel" in ev:
                    ce["channel"] = ev["channel"]
                if "text_len" in ev:
                    ce["text_len"] = ev["text_len"]
                if "text" in ev:
                    ce["text"] = ev["text"]
                if "peer" in ev:
                    ce["peer"] = ev["peer"]
                seg_events.append(ce)

        # Synthesize combat_start / combat_end if legacy trace lacked explicit events.
        has_combat_ev = any(e["event"] == "combat_start" for e in seg_events)
        if not has_combat_ev:
            for k in range(1, len(frames)):
                if frames[k]["in_combat"] and not frames[k - 1]["in_combat"]:
                    seg_events.append({"t_rel_ms": frames[k]["t_rel_ms"], "event": "combat_start"})
                elif not frames[k]["in_combat"] and frames[k - 1]["in_combat"]:
                    seg_events.append({"t_rel_ms": frames[k]["t_rel_ms"], "event": "combat_end"})

        seg_events.sort(key=lambda e: e["t_rel_ms"])
        segments.append({
            "t0_ms": t_start,
            "step_ms": step_ms,
            "frames": frames,
            "events": seg_events,
        })

    return segments


def label_situations(
    frames: list[dict[str, Any]],
    events: list[dict[str, Any]],
    chat_window_ms: int = 15_000,
    loot_window_ms: int = 5_000,
    combat_window_ms: int = 3_000,
) -> list[str]:
    """Labels each canonical frame with one situation from `SITUATIONS`."""
    chat_ts = [e["t_rel_ms"] for e in events if e["event"].startswith("chat")]
    loot_ts = [e["t_rel_ms"] for e in events if e["event"] == "loot"]
    combat_ts = [
        e["t_rel_ms"] for e in events
        if e["event"] in ("damage_taken", "combat_start")
    ]

    def _near(t_ms: int, ts: list[int], half_win: int) -> bool:
        # Linear scan is tiny for 30-60 s windows; bisect would also work.
        for x in ts:
            if abs(x - t_ms) <= half_win:
                return True
            if x > t_ms + half_win:
                break
        return False

    labels: list[str] = []
    for f in frames:
        t = f["t_rel_ms"]
        if f.get("in_group") or f.get("in_dungeon"):
            labels.append("in_group")
        elif _near(t, chat_ts, chat_window_ms):
            labels.append("chatting")
        elif f.get("in_combat") or _near(t, combat_ts, combat_window_ms):
            labels.append("solo_fight")
        elif _near(t, loot_ts, loot_window_ms):
            labels.append("looting")
        elif f.get("in_town"):
            labels.append("in_town")
        else:
            labels.append("travel")
    return labels


def _anonymize_clip(
    frames: list[dict[str, Any]],
    events: list[dict[str, Any]],
    situation: str,
    step_ms: int,
    canonicalize_heading: bool = True,
) -> dict[str, Any]:
    """Shifts clip to origin (0, 0, 0), t=0, and strips all identifying metadata."""
    t0 = frames[0]["t_rel_ms"]
    x0, y0, z0 = frames[0]["x"], frames[0]["y"], frames[0]["z"]
    o0 = frames[0]["o"] if canonicalize_heading else 0.0
    cos_o = math.cos(-o0)
    sin_o = math.sin(-o0)

    anon_frames: list[dict[str, Any]] = []
    for f in frames:
        dx = f["x"] - x0
        dy = f["y"] - y0
        dz = f["z"] - z0
        rx = round(dx * cos_o - dy * sin_o, 2)
        ry = round(dx * sin_o + dy * cos_o, 2)
        rz = round(dz, 2)
        ro = round((f["o"] - o0) % (2.0 * math.pi), 3)
        anon_frames.append({
            "t_ms": f["t_rel_ms"] - t0,
            "x": rx,
            "y": ry,
            "z": rz,
            "o": ro,
            "speed_yd_s": f["speed_yd_s"],
            "turn_rate_rad_s": f["turn_rate_rad_s"],
            "moving": f["moving"],
            "jumping": f["jumping"],
            "in_combat": f["in_combat"],
            "dead": f["dead"],
            "casting": f["casting"],
            "hp_pct": f["hp_pct"],
        })

    # Anonymize peer names in chat events to generic Peer1/Peer2.
    peer_map: dict[str, str] = {}
    anon_events: list[dict[str, Any]] = []
    t_end = frames[-1]["t_rel_ms"]
    for ev in events:
        et = ev["t_rel_ms"]
        if t0 <= et <= t_end:
            ae: dict[str, Any] = {
                "t_ms": et - t0,
                "event": ev["event"],
            }
            if "value" in ev:
                ae["value"] = ev["value"]
            if "text_len" in ev:
                ae["text_len"] = ev["text_len"]
            if "channel" in ev:
                ae["channel"] = ev["channel"]
            if ev.get("text"):
                ae["text"] = ev["text"]
            if ev.get("peer"):
                p = ev["peer"]
                if p not in peer_map:
                    peer_map[p] = f"Player{len(peer_map) + 1}"
                ae["peer"] = peer_map[p]
            anon_events.append(ae)

    return {
        "situation": situation,
        "step_ms": step_ms,
        "duration_s": round((t_end - t0) / 1000.0, 2),
        "frames": anon_frames,
        "events": anon_events,
    }


def extract_clips(
    rows: list[dict[str, Any]],
    clip_s: float = 45.0,
    step_s: float = 15.0,
    min_coverage: float = 0.65,
    step_ms: int = DEFAULT_STEP_MS,
) -> list[dict[str, Any]]:
    """Extracts anonymized, situation-labelled 30-60 s clips from raw trace rows."""
    segments = reconstruct_trajectory(rows, step_ms=step_ms)
    win_frames = max(2, int(round(clip_s * 1000.0 / step_ms)))
    stride_frames = max(1, int(round(step_s * 1000.0 / step_ms)))
    clips: list[dict[str, Any]] = []

    for seg in segments:
        frames = seg["frames"]
        events = seg["events"]
        if len(frames) < win_frames:
            continue
        labels = label_situations(frames, events)
        for start in range(0, len(frames) - win_frames + 1, stride_frames):
            end = start + win_frames
            win_labels = labels[start:end]
            # Dominant situation must cover at least `min_coverage` of the clip.
            counts: dict[str, int] = {}
            for lb in win_labels:
                counts[lb] = counts.get(lb, 0) + 1
            dom_sit, dom_cnt = max(counts.items(), key=lambda kv: kv[1])
            if dom_cnt / len(win_labels) < min_coverage:
                continue
            clips.append(
                _anonymize_clip(frames[start:end], events, dom_sit, step_ms)
            )
    return clips
