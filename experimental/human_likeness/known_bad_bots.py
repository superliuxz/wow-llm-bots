#!/usr/bin/env python3
"""Known-bad bot trace generators for Layer D scorer calibration.

Implements `docs/plans/self-improving-loop.md` section 3D:
  1. `plain_playerbots`: straight-line pathing, zero jumps, instant snap-turns,
     mechanical combat cadence.
  2. `random_jumper`: injects periodic/random jumps (~2.3/min) on top of rigid
     straight-line bot movement — permanent regression sentinel against
     rate-faking.
  3. `instant_reply`: replies to every whisper in 0.35-0.45 s with overly
     formal/polished prose.
  4. `same_loop_walker`: walks the exact same 4-corner polygon repeatedly
     (triggers Mitterhofer waypoint repetition & Lee self-similarity).

All generators emit the exact same raw JSONL schema as `BehaviorTraceWriter`
and pass through `trace_parity.py` before scoring.
"""

from __future__ import annotations

import math
import random
from typing import Any


def _make_sample(
    t_ms: int,
    role: str,
    name: str,
    x: float,
    y: float,
    o: float,
    moving: bool = True,
    jumping: bool = False,
    in_combat: bool = False,
    casting: str = "",
    xp: int = 0,
) -> dict[str, Any]:
    return {
        "kind": "sample",
        "t_ms": t_ms,
        "mono_us": t_ms * 1000,
        "role": role,
        "name": name,
        "map": 1,
        "zone": "Durotar",
        "in_town": False,
        "in_group": False,
        "in_dungeon": False,
        "x": round(x, 2),
        "y": round(y, 2),
        "z": 10.0,
        "o": round(o % (2.0 * math.pi), 3),
        "moving": moving,
        "jumping": jumping,
        "in_combat": in_combat,
        "dead": False,
        "level": 5,
        "xp": xp,
        "hp_pct": 95.0,
        "mana_pct": 90.0,
        "casting": casting,
        "target": "Boar" if in_combat else "",
        "attackers": 1 if in_combat else 0,
    }


def generate_plain_playerbots(
    name: str = "BadPlain",
    duration_s: int = 180,
    seed: int = 101,
) -> list[dict[str, Any]]:
    """Rigid straight-line segments, 0 jumps, instant >90 deg snap-turns."""
    rng = random.Random(seed)
    rows: list[dict[str, Any]] = []
    t0 = 1_795_000_000_000
    x, y, o = 0.0, 0.0, 0.0
    step_ms = 200
    n = int(duration_s * 1000 / step_ms)
    for k in range(n):
        t = t0 + k * step_ms
        # Snap-turn 90-150 deg every 12 s (60 ticks).
        if k > 0 and k % 60 == 0:
            o = (o + rng.choice([math.pi * 0.65, -math.pi * 0.75, math.pi * 0.85])) % (2.0 * math.pi)
        x += 7.0 * 0.2 * math.cos(o)
        y += 7.0 * 0.2 * math.sin(o)
        rows.append(_make_sample(t, "bot", name, x, y, o, moving=True, jumping=False))
    return rows


def generate_random_jumper(
    name: str = "BadJumper",
    duration_s: int = 180,
    seed: int = 102,
) -> list[dict[str, Any]]:
    """Rigid straight-line movement with mechanical jumps faking ~2.3 jumps/min."""
    rows = generate_plain_playerbots(name=name, duration_s=duration_s, seed=seed)
    # Inject mechanical jump every 26 s (130 ticks) — hits ~2.3 jumps/min.
    for k in range(65, len(rows), 130):
        rows[k]["jumping"] = True
    return rows


def generate_instant_reply(
    name: str = "BadInstant",
    duration_s: int = 180,
    seed: int = 103,
) -> list[dict[str, Any]]:
    """Answers every whisper in 0.35-0.45 s with overly formal prose."""
    rng = random.Random(seed)
    rows = generate_plain_playerbots(name=name, duration_s=duration_s, seed=seed)
    t0 = 1_795_000_000_000
    prompts = [
        ("PlayerA", "hey wanna group for boars?"),
        ("PlayerB", "where is the quest giver?"),
        ("PlayerA", "nice pull"),
        ("PlayerC", "what level are u"),
        ("PlayerB", "thx!"),
    ]
    replies = [
        "Greetings, fellow adventurer! I would be delighted to assist you with that quest.",
        "Certainly! The quest giver is located directly ahead near the main camp.",
        "Thank you kindly for the compliment! I strive for optimal combat efficiency.",
        "I am currently level five and progressing steadily through this zone.",
        "You are most welcome! Please let me know if I may be of further assistance.",
    ]
    for idx, ((peer, q), ans) in enumerate(zip(prompts, replies)):
        t_in = t0 + (idx + 1) * 30_000
        delay_ms = int(rng.uniform(350, 450))
        rows.append({
            "kind": "chat", "t_ms": t_in, "mono_us": t_in * 1000,
            "role": "bot", "name": name, "dir": "in", "channel": "whisper",
            "peer": peer, "text": q, "text_len": len(q),
        })
        rows.append({
            "kind": "chat", "t_ms": t_in + delay_ms, "mono_us": (t_in + delay_ms) * 1000,
            "role": "bot", "name": name, "dir": "out", "channel": "whisper",
            "peer": peer, "text": ans, "text_len": len(ans),
        })
    rows.sort(key=lambda r: (r["t_ms"], r["mono_us"]))
    return rows


def generate_same_loop_walker(
    name: str = "BadLooper",
    duration_s: int = 240,
    seed: int = 104,
) -> list[dict[str, Any]]:
    """Walks the exact same 4-corner polygon repeatedly across laps."""
    corners = [(0.0, 0.0), (40.0, 0.0), (40.0, 40.0), (0.0, 40.0)]
    rows: list[dict[str, Any]] = []
    t0 = 1_795_000_000_000
    step_ms = 200
    n = int(duration_s * 1000 / step_ms)
    x, y = corners[0]
    target_idx = 1
    speed = 7.0
    for k in range(n):
        t = t0 + k * step_ms
        tx, ty = corners[target_idx]
        dx, dy = tx - x, ty - y
        dist = math.hypot(dx, dy)
        o = math.atan2(dy, dx)
        if dist <= speed * 0.2:
            x, y = tx, ty
            target_idx = (target_idx + 1) % len(corners)
        else:
            x += speed * 0.2 * math.cos(o)
            y += speed * 0.2 * math.sin(o)
        rows.append(_make_sample(t, "bot", name, x, y, o, moving=True))
    return rows


def generate_synthetic_human(
    name: str = "RefHuman",
    duration_s: int = 240,
    seed: int = 201,
) -> list[dict[str, Any]]:
    """Organic curved steering, varied pauses, smooth turns, casual delayed chat.

    Emits irregular 300-500 ms client-like heartbeats to also exercise
    `trace_parity.py` resampling.
    """
    rng = random.Random(seed)
    rows: list[dict[str, Any]] = []
    t0 = 1_795_000_000_000
    x, y, o = 50.0, 50.0, 0.4
    t = t0
    t_end = t0 + duration_s * 1000
    pause_until = 0
    angular_vel = 0.0
    next_jump_t = t0 + rng.randint(15_000, 45_000)

    while t <= t_end:
        dt_ms = rng.randint(250, 480)
        dt_s = dt_ms / 1000.0
        if t < pause_until:
            moving = False
        else:
            if rng.random() < 0.025:
                pause_until = t + rng.randint(1200, 4000)
                moving = False
            else:
                moving = True
        if moving:
            # Smooth correlated mouse-steering drift (no instant snap-turns).
            angular_vel = 0.65 * angular_vel + rng.gauss(0.0, 0.35)
            angular_vel = max(-1.8, min(1.8, angular_vel))
            o = (o + angular_vel * dt_s) % (2.0 * math.pi)
            spd = 7.0 + rng.gauss(0.0, 0.25)
            x += spd * dt_s * math.cos(o)
            y += spd * dt_s * math.sin(o)
        else:
            # Gentle look-around while paused.
            o = (o + rng.gauss(0.0, 0.25) * dt_s) % (2.0 * math.pi)
        jumping = False
        if moving and t >= next_jump_t:
            jumping = True
            next_jump_t = t + rng.randint(18_000, 60_000)
        rows.append(_make_sample(t, "human", name, x, y, o, moving=moving, jumping=jumping))
        t += dt_ms

    # Casual human whispers with natural 3-8 s reading/typing delays.
    casual = [
        ("Peer1", "hey wanna group?", "sure inv"),
        ("Peer2", "where is the quest?", "by the hill i think"),
        ("Peer1", "nice", "ty"),
    ]
    for idx, (peer, q, a) in enumerate(casual):
        tin = t0 + (idx + 1) * 55_000
        delay = int(rng.uniform(3200, 7800))
        rows.append({
            "kind": "chat", "t_ms": tin, "mono_us": tin * 1000,
            "role": "human", "name": name, "dir": "in", "channel": "whisper",
            "peer": peer, "text": "", "text_len": len(q),
        })
        rows.append({
            "kind": "chat", "t_ms": tin + delay, "mono_us": (tin + delay) * 1000,
            "role": "human", "name": name, "dir": "out", "channel": "whisper",
            "peer": peer, "text": a, "text_len": len(a),
        })
    rows.sort(key=lambda r: (r["t_ms"], r["mono_us"]))
    return rows


def generate_synthetic_current_bot(
    name: str = "CurrBot",
    duration_s: int = 240,
    seed: int = 301,
) -> list[dict[str, Any]]:
    """Intermediate quality: better than plain playerbots, still below human."""
    rng = random.Random(seed)
    rows: list[dict[str, Any]] = []
    t0 = 1_795_000_000_000
    x, y, o = -30.0, 20.0, 1.0
    step_ms = 200
    n = int(duration_s * 1000 / step_ms)
    pause_until = 0
    for k in range(n):
        t = t0 + k * step_ms
        if t < pause_until:
            moving = False
        else:
            moving = True
            # Walks straight ~16 s segments with moderate pauses and occasional
            # mechanical turns (less robotic than plain_playerbots, less fluid than human).
            if k > 0 and k % 80 == 0:
                pause_until = t + rng.randint(600, 1400)
                o = (o + rng.choice([0.9, -0.9, 1.3, -1.3])) % (2.0 * math.pi)
            else:
                o = (o + rng.gauss(0.0, 0.015)) % (2.0 * math.pi)
            x += 7.0 * 0.2 * math.cos(o)
            y += 7.0 * 0.2 * math.sin(o)
        rows.append(_make_sample(t, "bot", name, x, y, o, moving=moving, jumping=(k % 210 == 50)))
    return rows


def perturb_random_jumper(
    real_bot_traces: dict[str, list[dict[str, Any]]],
    jump_every_s: float = 26.0,
) -> dict[str, list[dict[str, Any]]]:
    """Adds mechanical periodic jumps (~2.3/min) to real bot traces (rate-faking sentinel)."""
    out: dict[str, list[dict[str, Any]]] = {}
    gap_ms = int(jump_every_s * 1000.0)
    for name, rows in real_bot_traces.items():
        mutated: list[dict[str, Any]] = []
        last_jump_t: int | None = None
        for r in rows:
            c = dict(r)
            if c.get("kind") == "sample":
                t = int(c.get("t_ms", 0))
                if last_jump_t is None or (t - last_jump_t) >= gap_ms:
                    c["jumping"] = True
                    last_jump_t = t
            mutated.append(c)
        out[f"{name}_RandomJumper"] = mutated
    return out


def perturb_instant_reply(
    real_bot_traces: dict[str, list[dict[str, Any]]],
    seed: int = 103,
) -> dict[str, list[dict[str, Any]]]:
    """Injects 0.35-0.45 s overly formal whisper replies into real bot traces."""
    rng = random.Random(seed)
    prompts = [
        ("PlayerA", "hey wanna group for boars?"),
        ("PlayerB", "where is the quest giver?"),
        ("PlayerA", "nice pull"),
    ]
    replies = [
        "Greetings, fellow adventurer! I would be delighted to assist you with that quest.",
        "Certainly! The quest giver is located directly ahead near the main camp.",
        "Thank you kindly for the compliment! I strive for optimal combat efficiency.",
    ]
    out: dict[str, list[dict[str, Any]]] = {}
    for name, rows in real_bot_traces.items():
        samples = [r for r in rows if r.get("kind") == "sample" and "t_ms" in r]
        if not samples:
            continue
        t0 = int(samples[0]["t_ms"])
        mutated = [dict(r) for r in rows]
        for idx, ((peer, q), ans) in enumerate(zip(prompts, replies)):
            t_in = t0 + (idx + 1) * 15_000
            delay_ms = int(rng.uniform(350, 450))
            mutated.append({
                "kind": "chat", "t_ms": t_in, "mono_us": t_in * 1000,
                "role": "bot", "name": name, "dir": "in", "channel": "whisper",
                "peer": peer, "text": q, "text_len": len(q),
            })
            mutated.append({
                "kind": "chat", "t_ms": t_in + delay_ms, "mono_us": (t_in + delay_ms) * 1000,
                "role": "bot", "name": name, "dir": "out", "channel": "whisper",
                "peer": peer, "text": ans, "text_len": len(ans),
            })
        mutated.sort(key=lambda r: (int(r.get("t_ms", 0)), int(r.get("mono_us", 0))))
        out[f"{name}_InstantReply"] = mutated
    return out


def perturb_same_loop(
    real_bot_traces: dict[str, list[dict[str, Any]]],
    loop_s: float = 40.0,
    laps: int = 5,
) -> dict[str, list[dict[str, Any]]]:
    """Builds a real-route same-loop walker by repeating a real moving segment."""
    out: dict[str, list[dict[str, Any]]] = {}
    for name, rows in real_bot_traces.items():
        samples = [
            r for r in rows
            if r.get("kind") == "sample" and all(k in r for k in ("t_ms", "x", "y", "z"))
        ]
        if len(samples) < 20:
            continue
        # Take the first ~loop_s of real samples and ping-pong/repeat that exact
        # real route `laps` times so Mitterhofer waypoint repetition sees a real
        # repeated path on real terrain.
        t0 = int(samples[0]["t_ms"])
        seg = [s for s in samples if int(s["t_ms"]) - t0 <= int(loop_s * 1000)]
        if len(seg) < 10:
            seg = samples
        # Append reverse path so the loop closes smoothly without teleporting.
        one_lap = seg + list(reversed(seg[1:-1]))
        if len(one_lap) < 4:
            continue
        step_ms = 200
        tiled: list[dict[str, Any]] = []
        cur_t = t0
        for _ in range(laps):
            for s in one_lap:
                c = dict(s)
                c["t_ms"] = cur_t
                c["mono_us"] = cur_t * 1000
                tiled.append(c)
                cur_t += step_ms
        out[f"{name}_SameLoop"] = tiled
    return out


def build_known_bad_suite(
    duration_s: int = 240,
    real_bot_traces: dict[str, list[dict[str, Any]]] | None = None,
    plain_playerbots_traces: dict[str, list[dict[str, Any]]] | None = None,
) -> dict[str, dict[str, list[dict[str, Any]]]]:
    """Returns `{variant_name: {char_name: rows}}` for all 4 known-bad variants.

    When `real_bot_traces` are provided, `random_jumper`, `instant_reply`, and
    `same_loop_walker` are all derived from those real traces.
    """
    return {
        "plain_playerbots": (
            plain_playerbots_traces
            if plain_playerbots_traces
            else {"BadPlain": generate_plain_playerbots(duration_s=duration_s)}
        ),
        "random_jumper": (
            perturb_random_jumper(real_bot_traces)
            if real_bot_traces
            else {"BadJumper": generate_random_jumper(duration_s=duration_s)}
        ),
        "instant_reply": (
            perturb_instant_reply(real_bot_traces)
            if real_bot_traces
            else {"BadInstant": generate_instant_reply(duration_s=duration_s)}
        ),
        "same_loop_walker": (
            perturb_same_loop(real_bot_traces)
            if real_bot_traces
            else {"BadLooper": generate_same_loop_walker(duration_s=duration_s)}
        ),
    }
