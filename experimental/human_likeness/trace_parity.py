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

import hashlib
import json
import math
import pathlib
import statistics
from typing import Any

SITUATIONS = ("in_group", "chatting", "solo_fight", "looting", "in_town", "travel")
DEFAULT_STEP_MS = 200  # 5 Hz fixed grid

# Allow-list of gameplay event types emitted through the identical server path
# for both humans and bots. Human-only packet/UI events (e.g. `vendor_repair`,
# `item_open`, `emote`, `mail_take_item`, `vendor_buy`, `client_input`) are
# excluded so they can never leak role identity into blind parity clips.
PARITY_ALLOWED_EVENTS = frozenset({
    "combat_start",
    "combat_end",
    "spell_cast",
    "damage_dealt",
    "damage_taken",
    "heal_done",
    "mob_kill",
    "died",
    "resurrect",
    "release_spirit",
    "xp_gain",
    "level_up",
    "quest_accept",
    "quest_turnin",
    "quest_reward",
    "loot",
    "item_gain",
    "item_equip",
    "item_remove",
    "item_craft",
    "spell_learn",
    "talent_learn",
    "skill_up",
    "zone_change",
    "area_change",
    "map_change",
    "group_join",
    "group_leave",
    "go_interact",
    "item_use",
    "login",
    "logout",
})


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


def trace_set_hash(trace_dir: pathlib.Path, role: str | None = None) -> str:
    """Deterministic SHA-256 over a trace directory (identifies a candidate/baseline)."""
    h = hashlib.sha256()
    if not trace_dir.exists():
        return ""
    roles = (role,) if role else ("human", "bot")
    files: list[pathlib.Path] = []
    for r in roles:
        rdir = trace_dir / r
        if rdir.is_dir():
            files.extend(sorted(rdir.rglob("*.jsonl")))
    if not files:
        return ""
    for f in files:
        h.update(f.relative_to(trace_dir).as_posix().encode("utf-8"))
        h.update(b"\0")
        h.update(f.read_bytes())
        h.update(b"\0")
    return h.hexdigest()[:16]


def estimate_source_hz(rows: list[dict[str, Any]], max_gap_ms: int = 5000) -> float | None:
    """Estimates the raw source sampling rate (Hz) from median contiguous dt."""
    ts = sorted(
        int(r["t_ms"])
        for r in rows
        if r.get("kind") == "sample" and "t_ms" in r
    )
    dts = [ts[i] - ts[i - 1] for i in range(1, len(ts)) if 0 < ts[i] - ts[i - 1] <= max_gap_ms]
    if not dts:
        return None
    med_ms = statistics.median(dts)
    return round(1000.0 / med_ms, 2) if med_ms > 0 else None


def check_source_rate_parity(
    human_traces: dict[str, list[dict[str, Any]]],
    bot_traces: dict[str, list[dict[str, Any]]],
    min_hz: float = 3.5,
    max_ratio: float = 1.5,
) -> dict[str, Any]:
    """Rejects comparisons where human/bot source rates differ or are too coarse (e.g. old 1 Hz)."""
    h_rates = [hz for r in human_traces.values() if (hz := estimate_source_hz(r)) is not None]
    b_rates = [hz for r in bot_traces.values() if (hz := estimate_source_hz(r)) is not None]
    h_hz = round(statistics.median(h_rates), 2) if h_rates else None
    b_hz = round(statistics.median(b_rates), 2) if b_rates else None
    if h_hz is None or b_hz is None:
        return {"ok": False, "human_hz": h_hz, "bot_hz": b_hz, "reason": "missing_traces"}
    if h_hz < min_hz:
        return {
            "ok": False, "human_hz": h_hz, "bot_hz": b_hz,
            "reason": f"human source rate too low ({h_hz} Hz < {min_hz} Hz; re-record at 5 Hz)",
        }
    if b_hz < min_hz:
        return {
            "ok": False, "human_hz": h_hz, "bot_hz": b_hz,
            "reason": f"bot source rate too low ({b_hz} Hz < {min_hz} Hz)",
        }
    ratio = max(h_hz, b_hz) / max(min(h_hz, b_hz), 1e-6)
    if ratio > max_ratio:
        return {
            "ok": False, "human_hz": h_hz, "bot_hz": b_hz,
            "reason": f"source rate mismatch ({h_hz} Hz vs {b_hz} Hz, ratio {ratio:.2f} > {max_ratio})",
        }
    return {"ok": True, "human_hz": h_hz, "bot_hz": b_hz, "reason": "ok"}


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


def _extract_parity_keyframes(
    span: list[dict[str, Any]],
    keyframe_dt_ms: int = 500,
    pause_threshold_ms: int = 600,
) -> list[dict[str, Any]]:
    """Reduces BOTH human and bot spans to identical change-point keyframes.

    Fixes human 500 ms movement-packet stair-steps vs smooth bot ticks:
      1. Drops redundant unchanged samples while moving (keeping pause endpoints
         when stationary for >= `pause_threshold_ms` so real stops survive).
      2. Thins continuous-motion keyframes to `>= keyframe_dt_ms` (matching
         human client heartbeat spacing) while preserving stop/start, jump,
         combat, and dead state transitions.
    Both humans and bots are then interpolated onto the same 5 Hz grid.
    """
    if len(span) <= 2:
        return span

    def _same_pose(a: dict[str, Any], b: dict[str, Any]) -> bool:
        return (
            math.hypot(float(a["x"]) - float(b["x"]), float(a["y"]) - float(b["y"])) < 0.01
            and abs(float(a["z"]) - float(b["z"])) < 0.01
            and angle_diff(float(a.get("o", 0.0)), float(b.get("o", 0.0))) < 0.005
        )

    def _state_changed(a: dict[str, Any], b: dict[str, Any]) -> bool:
        return (
            bool(a.get("in_combat")) != bool(b.get("in_combat"))
            or bool(a.get("jumping")) != bool(b.get("jumping"))
            or bool(a.get("dead")) != bool(b.get("dead"))
            or bool(a.get("casting")) != bool(b.get("casting"))
        )

    # Pass 1: Collapse runs of identical pose (human packet stair-steps).
    # If a run lasts >= pause_threshold_ms, keep BOTH its first and last sample
    # (a real stop); if shorter (< pause_threshold_ms), keep only the first
    # sample so interpolation glides smoothly across the ~500 ms heartbeat gap.
    change_pts: list[dict[str, Any]] = [span[0]]
    run_start_idx = 0
    for i in range(1, len(span)):
        cur = span[i]
        prev = span[run_start_idx]
        if _same_pose(prev, cur) and not _state_changed(prev, cur):
            continue
        run_dur = int(span[i - 1]["t_ms"]) - int(prev["t_ms"])
        if i - 1 > run_start_idx and run_dur >= pause_threshold_ms:
            change_pts.append(span[i - 1])
        change_pts.append(cur)
        run_start_idx = i
    if run_start_idx < len(span) - 1:
        change_pts.append(span[-1])

    # Pass 2: Thin continuous-motion keyframes to `>= keyframe_dt_ms` so smooth
    # 5 Hz bot splines carry the exact same temporal resolution as ~500 ms human
    # movement heartbeats (preserving real pauses and state edges).
    thinned: list[dict[str, Any]] = [change_pts[0]]
    for i in range(1, len(change_pts) - 1):
        cur = change_pts[i]
        prev = thinned[-1]
        dt = int(cur["t_ms"]) - int(prev["t_ms"])
        if (
            dt >= keyframe_dt_ms
            or _state_changed(prev, cur)
            or _same_pose(prev, cur)  # real pause endpoint
        ):
            thinned.append(cur)
    if change_pts[-1] is not thinned[-1]:
        thinned.append(change_pts[-1])
    return thinned


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

    # Deduplicate and thin any sub-grid burst samples to at most 1 sample per
    # `step_ms` (preserving jump/combat transitions) so a higher-rate source
    # never carries extra detail into the fixed-time grid.
    dedup: list[dict[str, Any]] = []
    for s in samples:
        if not dedup:
            dedup.append(s)
            continue
        dt = int(s["t_ms"]) - int(dedup[-1]["t_ms"])
        if dt == 0:
            dedup[-1] = s
        elif dt >= step_ms or bool(s.get("jumping")) != bool(dedup[-1].get("jumping")) or bool(s.get("in_combat")) != bool(dedup[-1].get("in_combat")):
            dedup.append(s)
    samples = dedup
    if len(samples) < 2:
        return []

    raw_events: list[dict[str, Any]] = []
    for r in rows:
        kind = r.get("kind")
        if kind == "event":
            ev_name = str(r.get("event", ""))
            # Only pass events emitted symmetrically by both humans and bots.
            if ev_name not in PARITY_ALLOWED_EVENTS:
                continue
            raw_events.append({
                "t_ms": int(r.get("t_ms", 0)),
                "event": ev_name,
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

        # Collect jump pulse timestamps and peak raw turn rates from raw samples
        # BEFORE keyframe thinning/resampling so brief jumps and instant server
        # snap-turns (e.g. 180 deg in 200 ms = 15.7 rad/s) are never erased by
        # interpolation onto the 5 Hz grid.
        jump_times: set[int] = set()
        raw_turn_by_bucket: dict[int, float] = {}
        for i, s in enumerate(span):
            t_cur_raw = int(s["t_ms"])
            grid_idx = max(0, min(n_steps - 1, round((t_cur_raw - t_start) / step_ms)))
            if bool(s.get("jumping")) and (i == 0 or not bool(span[i - 1].get("jumping"))):
                jump_times.add(grid_idx)
            if i > 0:
                dt_raw_s = max((t_cur_raw - int(span[i - 1]["t_ms"])) / 1000.0, 0.05)
                da_raw = angle_diff(float(span[i - 1].get("o", 0.0)), float(s.get("o", 0.0)))
                w_raw = da_raw / dt_raw_s
                if w_raw > raw_turn_by_bucket.get(grid_idx, 0.0):
                    raw_turn_by_bucket[grid_idx] = w_raw

        # Reduce BOTH humans and bots to identical change-point keyframes at
        # human-packet resolution (~500 ms) before interpolating onto the 5 Hz grid.
        kf_span = _extract_parity_keyframes(span)

        frames: list[dict[str, Any]] = []
        raw_idx = 0
        for k in range(n_steps):
            t_cur = t_start + k * step_ms
            while raw_idx + 1 < len(kf_span) and int(kf_span[raw_idx + 1]["t_ms"]) < t_cur:
                raw_idx += 1
            s0 = kf_span[raw_idx]
            s1 = kf_span[min(raw_idx + 1, len(kf_span) - 1)]
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

        # Derive speed and moving from the canonical grid, and preserve peak raw
        # turn rate from pre-resampling samples so snap turns are never hidden.
        for k in range(1, len(frames)):
            prev = frames[k - 1]
            cur = frames[k]
            dist = math.hypot(cur["x"] - prev["x"], cur["y"] - prev["y"])
            speed = round(dist / dt_s, 2)
            da = angle_diff(prev["o"], cur["o"])
            grid_turn_rate = da / dt_s
            turn_rate = round(max(grid_turn_rate, raw_turn_by_bucket.get(k, 0.0)), 3)
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
            # Strip raw chat text in anonymized clips so human (<len=N>) and
            # bot chat are represented identically in blind social views.
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
