#!/usr/bin/env python3
"""Pairwise blind ranking engine (TrueSkill / Bradley-Terry with 95% CIs).

Implements `docs/plans/self-improving-loop.md` section B:
  - Renders anonymized, situation-matched 30-60 s clips as ASCII 2D path plots
    + event timelines (third-person watching protocol, never video).
  - Fits Bradley-Terry ratings anchored to human clips (human = 1000.0) with
    bootstrapped 95% confidence intervals.
  - Supports both human ground-truth ratings (`blind_rate.py`) and an optional
    order-debiased LLM pairwise judge that counts ONLY while it agrees with
    human blind ratings and catches known-bad bots.
  - Implements the keep/revert decision rule (beat current outside 95% CI +
    A gates pass + confirmation agrees).
"""

from __future__ import annotations

import datetime
import json
import math
import pathlib
import random
import statistics
import urllib.request
from typing import Any

# Elo-equivalent logistic scale: 400 / ln(10) ≈ 173.7178
BT_SCALE = 400.0 / math.log(10.0)
HUMAN_ANCHOR_RATING = 1000.0
DEFAULT_JUDGE_GATE_FILE = (
    pathlib.Path.home() / ".local" / "share" / "wow-llm-bots" / "judge_gate.json"
)


def load_judge_gate(path: pathlib.Path = DEFAULT_JUDGE_GATE_FILE) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except Exception:
        return {}


def update_judge_gate(
    updates: dict[str, Any],
    path: pathlib.Path = DEFAULT_JUDGE_GATE_FILE,
) -> dict[str, Any]:
    state = load_judge_gate(path)
    state.update(updates)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2) + "\n")
    return state


def is_judge_gate_valid(
    gate: dict[str, Any],
    llm_url: str,
    max_age_days: float = 14.0,
) -> tuple[bool, str]:
    """Enforces the plan's rule: LLM judge counts ONLY while it agrees with
    human blind ratings AND catches the known-bad bots (Layer D)."""
    if not gate:
        return False, "judge_gate missing (run blind_rate.py llm-agree and scorer_calibration.py --llm-url)"
    if gate.get("llm_url") and gate["llm_url"].rstrip("/") != llm_url.rstrip("/"):
        return False, f"judge_gate calibrated for {gate['llm_url']}, not {llm_url}"
    if not gate.get("agreement_valid"):
        return False, (
            f"LLM judge failed/missing human agreement check "
            f"(agree={gate.get('agreement')}, kappa={gate.get('kappa')})"
        )
    if not gate.get("bad_bots_caught"):
        return False, "LLM judge has not passed Layer D known-bad bot calibration"
    now = datetime.datetime.now(datetime.timezone.utc)
    for ts_key in ("agreement_at", "calibration_at"):
        ts_str = gate.get(ts_key)
        if not ts_str:
            return False, f"judge_gate missing {ts_key}"
        try:
            dt = datetime.datetime.fromisoformat(ts_str)
            if (now - dt).total_seconds() > max_age_days * 86400.0:
                return False, f"judge_gate {ts_key} expired (> {max_age_days:.0f} days old)"
        except ValueError:
            return False, f"judge_gate malformed {ts_key}"
    return True, "ok"


def _render_ascii_path(frames: list[dict[str, Any]], width: int = 36, height: int = 12) -> str:
    """Renders a 2D top-down ASCII plot of relative (x, y) trajectory."""
    if not frames:
        return "(no trajectory)"
    xs = [f["x"] for f in frames]
    ys = [f["y"] for f in frames]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    span_x = max(max_x - min_x, 1.0)
    span_y = max(max_y - min_y, 1.0)
    span = max(span_x, span_y)

    grid = [[" " for _ in range(width)] for _ in range(height)]

    def to_cell(x: float, y: float) -> tuple[int, int]:
        cx = int(round((x - min_x) / span * (width - 1)))
        cy = int(round((y - min_y) / span * (height - 1)))
        cx = max(0, min(width - 1, cx))
        cy = max(0, min(height - 1, height - 1 - cy))
        return cx, cy

    for i, f in enumerate(frames):
        cx, cy = to_cell(f["x"], f["y"])
        ch = "."
        if f.get("in_combat"):
            ch = "*"
        if not f.get("moving"):
            ch = "o"
        if f.get("jumping"):
            ch = "^"
        if i == 0:
            ch = "S"
        elif i == len(frames) - 1:
            ch = "E"
        # Preserve S/E/jump/combat markers over plain dots.
        cur = grid[cy][cx]
        if cur in (" ", ".") or ch in ("S", "E", "^"):
            grid[cy][cx] = ch

    border = "+" + "-" * width + "+"
    lines = [border]
    for row in grid:
        lines.append("|" + "".join(row) + "|")
    lines.append(border)
    lines.append(f"scale={span:.1f}yd  legend: S=start E=end .=walk o=stop ^=jump *=combat")
    return "\n".join(lines)


def render_clip_view(clip: dict[str, Any], layer: str = "body") -> str:
    """Renders an anonymized clip as a plot + event timeline for blind judging."""
    frames = clip.get("frames", [])
    events = clip.get("events", [])
    situation = clip.get("situation", "unknown")
    dur = clip.get("duration_s", 0.0)

    lines = [f"[situation={situation}  duration={dur:.1f}s]"]
    if layer == "social":
        chat_evs = [e for e in events if str(e.get("event", "")).startswith("chat")]
        if not chat_evs:
            lines.append("(no chat events in clip)")
        else:
            for e in chat_evs:
                t_s = e["t_ms"] / 1000.0
                direction = "IN " if e["event"] == "chat_in" else "OUT"
                peer = e.get("peer", "Player1")
                # Format human and bot chat identically by length only so
                # chatting clips never leak role identity via text presence.
                tlen = e.get("text_len")
                if tlen is None:
                    tlen = len(str(e.get("text", "")))
                body = f"<len={int(tlen)}>"
                lines.append(f"  +{t_s:5.1f}s [{direction} {e.get('channel', 'whisper')} {peer}] {body}")
        return "\n".join(lines)

    lines.append(_render_ascii_path(frames))

    # Compact kinematic summary (no identifying metadata).
    if frames:
        speeds = [f["speed_yd_s"] for f in frames]
        turns = [f["turn_rate_rad_s"] for f in frames if f["turn_rate_rad_s"] > 0.05]
        moving_pct = 100.0 * sum(1 for f in frames if f.get("moving")) / len(frames)
        jumps = sum(
            1 for i in range(1, len(frames))
            if frames[i].get("jumping") and not frames[i - 1].get("jumping")
        )
        stops = sum(
            1 for i in range(1, len(frames))
            if not frames[i].get("moving") and frames[i - 1].get("moving")
        )
        max_turn_deg = math.degrees(max(turns)) if turns else 0.0
        med_turn_deg = math.degrees(statistics.median(turns)) if turns else 0.0
        lines.append(
            f"motion: moving={moving_pct:.0f}%  max_spd={max(speeds):.1f}yd/s  "
            f"stops={stops}  jumps={jumps}  turn_deg/s med={med_turn_deg:.0f} max={max_turn_deg:.0f}"
        )

    # Sampled trajectory timeline (1 line every ~2 s for readability).
    step = max(1, int(round(2000.0 / max(1, clip.get("step_ms", 200)))))
    lines.append("timeline (2s samples):")
    for f in frames[::step][:25]:
        t_s = f["t_ms"] / 1000.0
        flags = "".join([
            "M" if f.get("moving") else "-",
            "J" if f.get("jumping") else "-",
            "C" if f.get("in_combat") else "-",
            "K" if f.get("casting") else "-",
        ])
        lines.append(
            f"  +{t_s:4.1f}s ({f['x']:+5.1f},{f['y']:+5.1f}) "
            f"o={f['o']:4.2f} v={f['speed_yd_s']:4.1f} w={f['turn_rate_rad_s']:4.2f} [{flags}]"
        )

    if events:
        lines.append("events:")
        for e in events[:25]:
            t_s = e["t_ms"] / 1000.0
            extra = ""
            if e.get("text_len"):
                extra = f" len={e['text_len']}"
            elif e.get("value"):
                extra = f" val={e['value']}"
            lines.append(f"  +{t_s:4.1f}s {e['event']}{extra}")
    return "\n".join(lines)


def _solve_bt_weights(
    entrants: list[str],
    comparisons: list[dict[str, Any]],
    reg: float = 0.05,
    max_iter: int = 200,
    tol: float = 1e-6,
) -> dict[str, float]:
    """Fits regularized Bradley-Terry log-strengths via MM iterations."""
    idx = {name: i for i, name in enumerate(entrants)}
    n = len(entrants)
    if n == 0:
        return {}

    wins = [0.0] * n
    pair_counts: dict[tuple[int, int], float] = {}
    for c in comparisons:
        a = idx.get(c["a"])
        b = idx.get(c["b"])
        if a is None or b is None or a == b:
            continue
        w = c.get("winner", "tie")
        if w == "a":
            wins[a] += 1.0
        elif w == "b":
            wins[b] += 1.0
        else:
            wins[a] += 0.5
            wins[b] += 0.5
        lo, hi = (a, b) if a < b else (b, a)
        pair_counts[(lo, hi)] = pair_counts.get((lo, hi), 0.0) + 1.0

    # Light uniform prior (virtual ties against dummy mean strength 1.0).
    p = [1.0] * n
    for _ in range(max_iter):
        new_p = [0.0] * n
        for i in range(n):
            denom = reg / (p[i] + 1.0)
            for (lo, hi), cnt in pair_counts.items():
                if lo == i:
                    denom += cnt / (p[i] + p[hi])
                elif hi == i:
                    denom += cnt / (p[i] + p[lo])
            num = wins[i] + 0.5 * reg
            new_p[i] = num / denom if denom > 0 else 1.0
        # Geometric-mean normalize for numerical stability.
        log_mean = sum(math.log(max(x, 1e-12)) for x in new_p) / n
        scale_f = math.exp(-log_mean)
        new_p = [x * scale_f for x in new_p]
        max_diff = max(abs(math.log(max(new_p[i], 1e-12)) - math.log(max(p[i], 1e-12))) for i in range(n))
        p = new_p
        if max_diff < tol:
            break

    return {entrants[i]: math.log(max(p[i], 1e-12)) for i in range(n)}


def fit_bradley_terry(
    comparisons: list[dict[str, Any]],
    anchor_label: str = "human",
    anchor_rating: float = HUMAN_ANCHOR_RATING,
    n_bootstrap: int = 400,
    seed: int = 0,
) -> dict[str, Any]:
    """Fits Bradley-Terry ratings + bootstrapped 95% CIs anchored to `anchor_label`.

    Each comparison dict has `{"a": str, "b": str, "winner": "a"|"b"|"tie"}`.
    Returns:
      {
        "ratings": {name: {"rating": float, "ci_low": float, "ci_high": float, "games": int}},
        "comparisons": int,
        "anchor": str,
        "bootstrap_samples": {name: [float, ...]},
      }
    """
    entrants = sorted({c["a"] for c in comparisons} | {c["b"] for c in comparisons})
    if not entrants:
        return {"ratings": {}, "comparisons": 0, "anchor": anchor_label, "bootstrap_samples": {}}

    games = {e: 0 for e in entrants}
    for c in comparisons:
        games[c["a"]] += 1
        games[c["b"]] += 1

    def to_ratings(log_s: dict[str, float]) -> dict[str, float]:
        ref = log_s.get(anchor_label, 0.0)
        return {k: anchor_rating + BT_SCALE * (v - ref) for k, v in log_s.items()}

    point = to_ratings(_solve_bt_weights(entrants, comparisons))

    rng = random.Random(seed)
    boot_vals: dict[str, list[float]] = {e: [] for e in entrants}
    m = len(comparisons)
    for _ in range(max(1, n_bootstrap)):
        sample = [comparisons[rng.randrange(m)] for _ in range(m)]
        b_rat = to_ratings(_solve_bt_weights(entrants, sample))
        for e in entrants:
            boot_vals[e].append(b_rat[e])

    out_ratings: dict[str, dict[str, Any]] = {}
    for e in entrants:
        s = sorted(boot_vals[e])
        lo = s[int(0.025 * (len(s) - 1))]
        hi = s[int(0.975 * (len(s) - 1))]
        out_ratings[e] = {
            "rating": round(point[e], 1),
            "ci_low": round(lo, 1),
            "ci_high": round(hi, 1),
            "games": games[e],
        }
    return {
        "ratings": out_ratings,
        "comparisons": m,
        "anchor": anchor_label,
        "bootstrap_samples": boot_vals,
    }


def beats_outside_ci(bt_result: dict[str, Any], candidate: str, current: str) -> tuple[bool, dict[str, float]]:
    """True iff `candidate` beats `current` with bootstrapped 95% CI > 0."""
    boots = bt_result.get("bootstrap_samples", {})
    if candidate not in boots or current not in boots:
        return False, {"delta": 0.0, "ci_low": 0.0, "ci_high": 0.0}
    c_rat = bt_result["ratings"][candidate]["rating"]
    b_rat = bt_result["ratings"][current]["rating"]
    deltas = sorted(a - b for a, b in zip(boots[candidate], boots[current]))
    lo = deltas[int(0.025 * (len(deltas) - 1))]
    hi = deltas[int(0.975 * (len(deltas) - 1))]
    return lo > 0.0, {
        "delta": round(c_rat - b_rat, 1),
        "ci_low": round(lo, 1),
        "ci_high": round(hi, 1),
    }


def _ask_llm_once(view_a: str, view_b: str, llm_url: str, layer: str) -> str | None:
    if layer == "social":
        criteria = (
            "Judge which player's chat looks MORE like a real World of Warcraft human player. "
            "IMPORTANT: overly polished, formal, articulate, or instant replies are a BOT tell; "
            "casual, terse, slightly imperfect phrasing with natural reading/typing delays is more human."
        )
    else:
        criteria = (
            "Judge which gameplay clip looks MORE like a real World of Warcraft human player "
            "(natural movement curves, smooth turning, organic pauses/jumps, human reaction timing) "
            "rather than a bot (robotic straight lines, snap-turns, mechanical repetition, or fake random jumps)."
        )
    prompt = (
        f"{criteria}\n"
        "Reply with ONLY one token: 'A', 'B', or 'TIE'.\n\n"
        f"=== Clip A ===\n{view_a}\n\n=== Clip B ===\n{view_b}\n"
    )
    payload = json.dumps({
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.0,
        "max_tokens": 6,
    }).encode("utf-8")
    req = urllib.request.Request(
        f"{llm_url.rstrip('/')}/v1/chat/completions",
        data=payload,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        tok = data["choices"][0]["message"]["content"].strip().upper().split()[0]
        if tok.startswith("A"):
            return "a"
        if tok.startswith("B"):
            return "b"
        if tok.startswith("TIE"):
            return "tie"
        return None
    except Exception:
        return None


def judge_pair_llm(
    clip_a: dict[str, Any],
    clip_b: dict[str, Any],
    llm_url: str,
    layer: str = "body",
    order_debias: bool = True,
) -> str | None:
    """Judges a matched pair with the LLM, optionally cancelling position bias."""
    va = render_clip_view(clip_a, layer=layer)
    vb = render_clip_view(clip_b, layer=layer)
    r1 = _ask_llm_once(va, vb, llm_url, layer)
    if r1 is None:
        return None
    if not order_debias:
        return r1
    r2_swapped = _ask_llm_once(vb, va, llm_url, layer)
    if r2_swapped is None:
        return r1
    r2 = {"a": "b", "b": "a", "tie": "tie"}[r2_swapped]
    return r1 if r1 == r2 else "tie"


def check_judge_agreement(
    human_labels: list[str],
    llm_labels: list[str],
    min_pairs: int = 30,
    min_agreement: float = 0.70,
    min_kappa: float = 0.40,
) -> dict[str, Any]:
    """Checks whether the LLM judge agrees with human blind ratings."""
    pairs = [
        (h, l) for h, l in zip(human_labels, llm_labels)
        if h in ("a", "b", "tie") and l in ("a", "b", "tie")
    ]
    n = len(pairs)
    if n == 0:
        return {"n": 0, "agreement": 0.0, "kappa": 0.0, "valid": False}
    agree = sum(1 for h, l in pairs if h == l) / n
    cats = ("a", "b", "tie")
    pe = sum(
        (sum(1 for h, _ in pairs if h == c) / n) * (sum(1 for _, l in pairs if l == c) / n)
        for c in cats
    )
    kappa = ((agree - pe) / (1.0 - pe)) if pe < 1.0 else (1.0 if agree == 1.0 else 0.0)
    valid = n >= min_pairs and agree >= min_agreement and kappa >= min_kappa
    return {
        "n": n,
        "agreement": round(agree, 3),
        "kappa": round(kappa, 3),
        "valid": valid,
    }


def decide_keep_revert(
    bt_result: dict[str, Any],
    candidate: str,
    current: str,
    gates_ok: bool,
    confirm_agrees: bool,
) -> dict[str, Any]:
    """Applies section 3B keep/revert rule."""
    beats, delta_info = beats_outside_ci(bt_result, candidate, current)
    keep = bool(gates_ok and beats and confirm_agrees)
    reason = "keep" if keep else (
        "gates_failed" if not gates_ok
        else ("within_uncertainty_or_worse" if not beats else "confirmation_disagreed")
    )
    return {
        "keep": keep,
        "reason": reason,
        "beats_outside_ci": beats,
        **delta_info,
    }
