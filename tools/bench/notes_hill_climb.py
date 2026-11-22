#!/usr/bin/env python3
"""C2: Self-revising boss notes.md competence hill-climb loop.

After a wipe (or sub-optimal boss run), summarizes the fight timeline from
behavior traces (what hit whom, when bots died, and which standing orders
fired), asks a slower reflection LLM call to propose a revised `notes.md`,
validates the proposed standing orders, and keeps the edit ONLY if a short
boss benchmark re-run improves mechanical competence (kills, deaths, damage
taken) without regressing kills or collapsing DPS.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import datetime
import json
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from typing import Any, Callable

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
VAR = ROOT / "var"

PREFIXES = ["Load", "Ptwo", "Pthr", "Pfou"]

# Exact enums & tokens from harness/grammar/orders.wlb (case-sensitive).
_WHO_RE = r"(?:me|tank|offtank|healer|leader|anyone|boss|it)"
_ICON_RE = r"(?:skull|cross|square|moon|triangle|diamond|circle|star)"
_STATE_RE = r"(?:flying|grounded|untargetable|submerged)"
_MY_STATE_RE = r"(?:in vehicle|mounted|swimming|nearest to it)"
_NUM_RE = r"\d{1,3}(?:\.\d)?"
_COUNT_RE = r"\d{1,3}"
_POINTS_RE = r"\d{1,5}"
_DIGIT_RE = r"[0-9]"

_ZERO_ARG_ACTS = frozenset({"back", "jump", "leave vehicle", "wake"})
_STOP_WHAT = frozenset({"casting", "attacking", "moving"})
_PET_CMDS = frozenset({"stay", "follow", "passive", "defensive"})
_ICONS = frozenset({"skull", "cross", "square", "moon", "triangle", "diamond", "circle", "star"})
_ARG_ACT_PREFIXES = (
    "to ",
    "out of ",
    "avoid ",
    "target ",
    "use ",
    "face away ",
    "face ",
    "cancel ",
    "interact ",
    "board ",
    "equip ",
    "kite ",
)

# Exact case-sensitive LHS `Check` patterns from harness/grammar/orders.wlb (lines 43-71).
_CHECK_PATTERNS = (
    re.compile(rf"^.+\s+stacks[<>]{_COUNT_RE} on {_WHO_RE}$"),
    re.compile(rf"^.+\s+ends<{_NUM_RE}s on {_WHO_RE}$"),
    re.compile(rf"^.+\s+cast on {_WHO_RE}$"),
    re.compile(rf"^.+\s+cast in last {_NUM_RE}s$"),
    re.compile(r"^.+\s+cast$"),
    re.compile(rf"^.+\s+appears in last {_NUM_RE}s$"),
    re.compile(r"^.+\s+appears$"),
    re.compile(r"^in .+$"),
    re.compile(rf"^.+\s+count[<>]{_COUNT_RE}$"),
    re.compile(rf"^.+\s+on {_WHO_RE}$"),
    re.compile(rf"^.+\s+within {_NUM_RE}(?: of .+)?$"),
    re.compile(rf"^.+\s+beyond {_NUM_RE}(?: of .+)?$"),
    re.compile(r"^.+\s+targets .+$"),
    re.compile(rf"^.+\s+hp[<>]{_NUM_RE}$"),
    re.compile(rf"^mana_points[<>]{_POINTS_RE}$"),
    re.compile(rf"^mana[<>]{_NUM_RE}$"),
    re.compile(rf"^.+\s+is marked {_ICON_RE}$"),
    re.compile(rf"^.+\s+is {_STATE_RE}$"),
    re.compile(r"^.+\s+(?:in|out of) sight$"),
    re.compile(r"^me behind .+$"),
    re.compile(r"^my target is .+$"),
    re.compile(r"^me has .+$"),
    re.compile(rf"^me {_MY_STATE_RE}$"),
    re.compile(rf"^phase {_DIGIT_RE}$"),
    re.compile(rf"^(?:after|every) {_NUM_RE}s$"),
)

_NUM_FULL_RE = re.compile(rf"^{_NUM_RE}$")

_THINK_RE = re.compile(r"<think>.*?</think>", re.DOTALL | re.IGNORECASE)
_CODE_FENCE_RE = re.compile(r"```(?:markdown|md)?\s*\n(.*?)```", re.DOTALL | re.IGNORECASE)
_BACKTICK_ORDER_RE = re.compile(r"`([^`\n]+->[^`\n]+)`")


@dataclasses.dataclass(frozen=True)
class CompetenceScore:
    kills: int
    runs: int
    median_dps: float
    median_deaths: float
    median_damage_taken: float = 0.0

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def score_from_boss_result(boss_res: dict[str, Any], arm: str = "llm") -> CompetenceScore:
    """Extracts a CompetenceScore for `arm` from a bench_bosses.py boss result dict."""
    by_arm = boss_res.get("by_arm", {})
    arm_stats = by_arm.get(arm, {})
    trials = [t for t in boss_res.get("trials", []) if t.get("mode") == arm]
    dmg_totals: list[float] = []
    for t in trials:
        chars = t.get("characters", {})
        if chars:
            dmg_totals.append(
                sum(float(c.get("damage_taken", 0.0)) for c in chars.values())
            )
    med_dmg = statistics.median(dmg_totals) if dmg_totals else 0.0
    return CompetenceScore(
        kills=int(arm_stats.get("kills", 0)),
        runs=int(arm_stats.get("runs", 0)),
        median_dps=float(arm_stats.get("median_dps", 0.0)),
        median_deaths=float(arm_stats.get("median_deaths", 0.0)),
        median_damage_taken=round(med_dmg, 1),
    )


def is_competence_improvement(
    incumbent: CompetenceScore, candidate: CompetenceScore
) -> tuple[bool, str]:
    """Mechanical competence gate (kills, deaths, damage taken — never human-likeness)."""
    if candidate.runs <= 0:
        return False, "candidate produced 0 valid runs"
    inc_rate = incumbent.kills / max(1, incumbent.runs)
    cand_rate = candidate.kills / candidate.runs
    if cand_rate < inc_rate - 1e-9:
        return (
            False,
            f"kill rate regressed ({candidate.kills}/{candidate.runs} < "
            f"{incumbent.kills}/{incumbent.runs})",
        )
    if cand_rate > inc_rate + 1e-9:
        if incumbent.median_dps > 0 and candidate.median_dps < incumbent.median_dps * 0.85:
            return (
                False,
                f"DPS regressed >15% despite kill gain ({candidate.median_dps:.1f} vs "
                f"{incumbent.median_dps:.1f})",
            )
        return (
            True,
            f"kill rate improved ({candidate.kills}/{candidate.runs} > "
            f"{incumbent.kills}/{incumbent.runs})",
        )
    # Equal kill rate: guard against DPS collapse (>10% drop) and death increase.
    if incumbent.median_dps > 0 and candidate.median_dps < incumbent.median_dps * 0.90:
        return (
            False,
            f"DPS regressed >10% ({candidate.median_dps:.1f} vs {incumbent.median_dps:.1f})",
        )
    if candidate.median_deaths > incumbent.median_deaths + 1e-9:
        return (
            False,
            f"deaths increased ({candidate.median_deaths:.1f} > {incumbent.median_deaths:.1f})",
        )
    if candidate.median_deaths < incumbent.median_deaths - 1e-9:
        return (
            True,
            f"deaths decreased ({candidate.median_deaths:.1f} < {incumbent.median_deaths:.1f})",
        )
    if (
        incumbent.median_damage_taken > 0
        and candidate.median_damage_taken > 0
        and candidate.median_damage_taken <= incumbent.median_damage_taken * 0.95
    ):
        return (
            True,
            f"damage taken decreased >=5% ({candidate.median_damage_taken:.0f} vs "
            f"{incumbent.median_damage_taken:.0f})",
        )
    return False, "no measurable improvement in kills, deaths, or damage taken"


def extract_standing_orders(notes_md: str) -> list[str]:
    """Extracts standing-order lines (`cond -> act`) from backticks or fenced blocks."""
    orders: list[str] = []
    in_fence = False
    for raw_line in notes_md.splitlines():
        line = raw_line.strip()
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence and "->" in line:
            orders.append(line)
            continue
        for m in _BACKTICK_ORDER_RE.finditer(line):
            orders.append(m.group(1).strip())
    return orders


def _validate_when(when: str) -> str | None:
    s = when.strip()
    if s == "always":
        return None
    conds = [c.strip() for c in s.split(" & ")]
    if not (1 <= len(conds) <= 4):
        return f"condition must have 1..4 clauses (got {len(conds)})"
    for c in conds:
        if not c:
            return "empty condition clause"
        body = c[4:].strip() if c.startswith("not ") else c
        if not any(pat.match(body) for pat in _CHECK_PATTERNS):
            return f"unsupported condition clause: {c!r}"
    return None


def _validate_act(step: str) -> str | None:
    s = step.strip()
    if s in _ZERO_ARG_ACTS:
        return None
    if s == "stop":
        return None
    if s.startswith("stop "):
        what = s[5:].strip()
        return None if what in _STOP_WHAT else f"invalid stop target: {step!r}"
    if s == "away":
        return None
    if s.startswith("away "):
        rest = s[5:].strip()
        if not rest:
            return f"missing argument for 'away': {step!r}"
        parts = rest.split()
        # `away` allows `<Thing>` and/or trailing `<Num>`, never two trailing numbers.
        if len(parts) >= 2 and _NUM_FULL_RE.match(parts[-1]) and _NUM_FULL_RE.match(parts[-2]):
            return f"too many numeric args for 'away': {step!r}"
        return None
    if s.startswith("mark "):
        rest = s[5:].strip()
        parts = rest.rsplit(" ", 1)
        if len(parts) == 2 and parts[0].strip() and parts[1] in _ICONS:
            return None
        return f"invalid mark target/icon: {step!r}"
    if s.startswith("pet "):
        rest = s[4:].strip()
        if rest in _PET_CMDS:
            return None
        if rest.startswith("attack ") and rest[7:].strip():
            return None
        return f"invalid pet command: {step!r}"
    for prefix in _ARG_ACT_PREFIXES:
        if s.startswith(prefix):
            if s[len(prefix) :].strip():
                return None
            return f"missing argument for {prefix.strip()!r}: {step!r}"
    return f"unsupported act: {step!r}"


def validate_standing_orders(notes_md: str) -> tuple[bool, list[str]]:
    """Checks proposed notes.md for valid standing-order syntax aligned with orders.wlb."""
    if not notes_md.strip():
        return False, ["empty notes.md"]
    errors: list[str] = []
    for order in extract_standing_orders(notes_md):
        if order.count("->") != 1:
            errors.append(f"malformed order (expected one '->'): {order!r}")
            continue
        cond, rhs = (p.strip() for p in order.split("->", 1))
        if not cond or not rhs:
            errors.append(f"empty condition or action: {order!r}")
            continue
        if cond_err := _validate_when(cond):
            errors.append(f"{cond_err} in {order!r}")
        steps = [s.strip() for s in rhs.split(", then ")]
        if not (1 <= len(steps) <= 3):
            errors.append(f"order must have 1..3 steps (got {len(steps)}): {order!r}")
            continue
        for step in steps:
            if act_err := _validate_act(step):
                errors.append(f"{act_err} in {order!r}")
    return (len(errors) == 0), errors


def summarize_fight_timeline(
    boss_res: dict[str, Any],
    trace_dir: pathlib.Path | None,
    arm: str = "llm",
    max_wipes: int = 2,
) -> str:
    """Builds a concise factual fight timeline from wiped trials and behavior traces."""
    boss_name = boss_res.get("boss", "Unknown Boss")
    trials = [t for t in boss_res.get("trials", []) if t.get("mode") == arm]
    wipes = [t for t in trials if not t.get("killed")]
    selected = wipes[:max_wipes] if wipes else sorted(trials, key=lambda t: t.get("total_dps", 0))[:1]
    if not selected:
        return f"No {arm} trials recorded for {boss_name}."

    events_by_bot: dict[str, list[dict[str, Any]]] = {}
    samples_by_bot: dict[str, list[dict[str, Any]]] = {}
    if trace_dir and trace_dir.exists():
        for fp in sorted(trace_dir.glob("bot/*/*.jsonl")):
            bot = fp.parent.name
            evts = events_by_bot.setdefault(bot, [])
            smps = samples_by_bot.setdefault(bot, [])
            with fp.open() as fh:
                for raw in fh:
                    raw = raw.strip()
                    if not raw:
                        continue
                    try:
                        obj = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    kind = obj.get("kind")
                    if kind == "event":
                        evts.append(obj)
                    elif kind == "sample":
                        smps.append(obj)
        for smps in samples_by_bot.values():
            smps.sort(key=lambda s: int(s.get("t_ms", 0)))

    lines = [
        f"Boss: {boss_name} ({arm}: {boss_res.get('by_arm', {}).get(arm, {}).get('kills', 0)}/"
        f"{boss_res.get('by_arm', {}).get(arm, {}).get('runs', len(trials))} kills)"
    ]
    for idx, t in enumerate(selected, 1):
        dur = float(t.get("duration_s", 0.0))
        dps = float(t.get("total_dps", 0.0))
        deaths = int(t.get("total_deaths", 0))
        outcome = "KILL" if t.get("killed") else "WIPE"
        lines.append(f"\nTrial {idx} ({outcome}, {dur:.1f}s, party DPS={dps:.0f}, deaths={deaths}):")
        p_idx = int(t.get("party_index", 0))
        inst = t.get("instance_id")
        pull_ms = int(t.get("pull_ms", 0))
        t0 = pull_ms
        t1 = pull_ms + int(dur * 1000) + 500
        pref = PREFIXES[p_idx] if 0 <= p_idx < len(PREFIXES) else "Load"
        dmg_taken: collections.Counter[str] = collections.Counter()
        act_counts: collections.Counter[str] = collections.Counter()
        death_events: list[tuple[float, str, str]] = []
        for suf in ("a", "b", "c", "d", "e", "f", "g", "h", "i", "j"):
            bot = pref + suf
            # FormatTraceSample emits `inst` while FormatTraceEvent does not:
            # bound each bot's event window before the first sample of a later instance.
            bot_t1 = t1
            if inst is not None:
                for s in samples_by_bot.get(bot, []):
                    stm = int(s.get("t_ms", 0))
                    if stm < t0:
                        continue
                    s_inst = s.get("inst")
                    if s_inst is not None and s_inst != inst:
                        bot_t1 = min(bot_t1, stm - 1)
                        break
            for e in events_by_bot.get(bot, []):
                tm = int(e.get("t_ms", 0))
                if tm < t0 or tm > bot_t1:
                    continue
                ev = e.get("event")
                if ev == "damage_taken":
                    dmg_taken[str(e.get("detail", "?"))] += int(e.get("value", 0))
                elif ev == "died":
                    rel_s = max(0.0, (tm - pull_ms) / 1000.0)
                    death_events.append((rel_s, bot, str(e.get("detail", "?"))))
                elif ev == "act_sent":
                    act_counts[str(e.get("detail", "?"))] += 1
        if dmg_taken:
            top = ", ".join(f"{k}={v}" for k, v in dmg_taken.most_common(5))
            lines.append(f"  Top damage taken: {top}")
        if death_events:
            death_events.sort()
            d_str = "; ".join(f"{s:.1f}s {b} killed by {k}" for s, b, k in death_events[:6])
            lines.append(f"  Deaths: {d_str}")
        if act_counts:
            a_str = ", ".join(f"{k} x{v}" for k, v in act_counts.most_common(6))
            lines.append(f"  Orders/acts fired: {a_str}")
    return "\n".join(lines)


def _strip_reflection_reply(text: str) -> str:
    cleaned = _THINK_RE.sub("", text).strip()
    m = _CODE_FENCE_RE.search(cleaned)
    if m:
        return m.group(1).strip() + "\n"
    return cleaned + "\n" if cleaned else ""


def propose_notes_edit(
    boss_name: str,
    current_notes: str,
    timeline_summary: str,
    rejected_history: list[str],
    llm_url: str = "http://127.0.0.1:8090/v1/chat/completions",
    timeout_s: float = 120.0,
) -> str:
    """Calls local llama-server to reflect on wipe timelines and propose revised notes.md."""
    rejected_block = ""
    if rejected_history:
        rejected_block = (
            "\nPreviously rejected edits (do NOT repeat these):\n"
            + "\n".join(f"- {r}" for r in rejected_history[-5:])
            + "\n"
        )
    prompt = (
        f"You are revising concise tactical boss notes (`notes.md`) for World of Warcraft 3.3.5a "
        f"boss **{boss_name}**.\n\n"
        f"Standing orders MUST use valid grammar lines inside backticks, e.g.:\n"
        f"- `<Spell> cast -> out of it`\n"
        f"- `<Spell> cast -> away <Boss> <yd>, then back`\n"
        f"- `in <Hazard> -> out of it`\n"
        f"- `<Debuff> on me -> away`\n"
        f"- `<Debuff> on anyone -> stop attacking`\n"
        f"- `<Add> appears -> target nearest <Add>`\n"
        f"- `<Add> within <yd> -> away <Add> <yd>`\n\n"
        f"Current `notes.md`:\n```markdown\n{current_notes.strip()}\n```\n\n"
        f"Recent fight timeline:\n{timeline_summary}\n"
        f"{rejected_block}\n"
        f"Propose a concise improved `notes.md` (under 15 lines) that addresses the main cause of "
        f"deaths or damage taken without hurting DPS. Return ONLY the revised markdown inside a "
        f"```markdown ... ``` fence."
    )
    payload = json.dumps(
        {
            "model": "local",
            "messages": [{"role": "user", "content": prompt}],
            "temperature": 0.2,
            "max_tokens": 1024,
            "chat_template_kwargs": {"enable_thinking": False},
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        llm_url,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout_s) as resp:
        body = json.loads(resp.read().decode("utf-8"))
    content = body["choices"][0]["message"]["content"]
    return _strip_reflection_reply(content)


def _default_bench_runner(
    boss_key: str,
    notes_path: pathlib.Path,
    runs: int,
    parallel: int,
    out_json: pathlib.Path,
    trace_dir: pathlib.Path,
) -> dict[str, Any]:
    cmd = [
        sys.executable,
        "-u",
        str(ROOT / "tools" / "bench" / "bench_bosses.py"),
        "--only",
        boss_key,
        "--mode",
        "llm",
        "--runs",
        str(runs),
        "--parallel",
        str(parallel),
        "--notes-file",
        str(notes_path),
        "--trace-dir",
        str(trace_dir),
        "--out",
        str(out_json),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)
    data = json.loads(out_json.read_text())
    return data[boss_key]


def run_notes_hill_climb(
    boss_key: str,
    initial_notes: str,
    out_dir: pathlib.Path,
    rounds: int = 3,
    runs_per_eval: int = 3,
    confirm_runs: int = 0,
    parallel: int = 2,
    bench_fn: Callable[
        [str, pathlib.Path, int, int, pathlib.Path, pathlib.Path], dict[str, Any]
    ]
    | None = None,
    propose_fn: Callable[[str, str, str, list[str]], str] | None = None,
    llm_url: str = "http://127.0.0.1:8090/v1/chat/completions",
) -> dict[str, Any]:
    """Runs the C2 competence hill-climb loop on `notes.md` for one boss."""
    out_dir.mkdir(parents=True, exist_ok=True)
    best_notes_path = out_dir / "best_notes.md"
    ledger_path = out_dir / "ledger.jsonl"
    bench = bench_fn or _default_bench_runner
    propose = propose_fn or (
        lambda bn, cn, tl, rej: propose_notes_edit(bn, cn, tl, rej, llm_url=llm_url)
    )

    best_notes = initial_notes.strip() + "\n"
    best_notes_path.write_text(best_notes)

    base_json = out_dir / "round_0_base.json"
    best_trace_dir = out_dir / "round_0_traces"
    best_res = bench(boss_key, best_notes_path, runs_per_eval, parallel, base_json, best_trace_dir)
    best_score = score_from_boss_result(best_res, arm="llm")
    boss_name = best_res.get("boss", boss_key)

    rejected_history: list[str] = []
    history: list[dict[str, Any]] = []

    def log_entry(entry: dict[str, Any]) -> None:
        entry["timestamp"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        history.append(entry)
        with ledger_path.open("a") as fh:
            fh.write(json.dumps(entry) + "\n")

    log_entry({"round": 0, "status": "baseline", "score": best_score.to_dict()})

    for r in range(1, rounds + 1):
        timeline = summarize_fight_timeline(best_res, best_trace_dir, arm="llm")
        candidate_notes = propose(boss_name, best_notes, timeline, rejected_history)
        orders_preview = "; ".join(extract_standing_orders(candidate_notes)) or "(no orders)"
        valid, errs = validate_standing_orders(candidate_notes)
        if not valid:
            reason = "invalid standing orders: " + "; ".join(errs)
            rejected_history.append(f"round {r} [{orders_preview}]: {reason}")
            log_entry({"round": r, "status": "rejected_syntax", "reason": reason})
            continue
        if candidate_notes.strip() == best_notes.strip():
            reason = "proposed notes identical to current best"
            rejected_history.append(f"round {r}: {reason}")
            log_entry({"round": r, "status": "rejected_noop", "reason": reason})
            continue

        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".md", dir=out_dir, prefix=f"round_{r}_cand_", delete=False
        ) as tf:
            tf.write(candidate_notes)
            cand_path = pathlib.Path(tf.name)

        cand_json = out_dir / f"round_{r}_cand.json"
        cand_trace_dir = out_dir / f"round_{r}_traces"
        cand_res = bench(boss_key, cand_path, runs_per_eval, parallel, cand_json, cand_trace_dir)
        cand_score = score_from_boss_result(cand_res, arm="llm")
        keep, reason = is_competence_improvement(best_score, cand_score)

        # Optional confirmation re-run guards against binomial noise on small n.
        logged_score = cand_score
        initial_score_for_log: CompetenceScore | None = None
        if keep and confirm_runs > 0:
            conf_json = out_dir / f"round_{r}_confirm.json"
            conf_trace_dir = out_dir / f"round_{r}_confirm_traces"
            conf_res = bench(
                boss_key, cand_path, confirm_runs, parallel, conf_json, conf_trace_dir
            )
            conf_score = score_from_boss_result(conf_res, arm="llm")
            keep, conf_reason = is_competence_improvement(best_score, conf_score)
            initial_score_for_log = cand_score
            logged_score = conf_score
            if keep:
                cand_res = conf_res
                cand_score = conf_score
                cand_trace_dir = conf_trace_dir
                reason = f"confirmed ({conf_reason})"
            else:
                reason = f"failed confirmation ({conf_reason})"

        if keep:
            best_notes = candidate_notes
            best_res = cand_res
            best_score = cand_score
            best_trace_dir = cand_trace_dir
            best_notes_path.write_text(best_notes)
            entry: dict[str, Any] = {
                "round": r,
                "status": "kept",
                "reason": reason,
                "score": logged_score.to_dict(),
            }
            if initial_score_for_log is not None:
                entry["initial_score"] = initial_score_for_log.to_dict()
            log_entry(entry)
        else:
            rejected_history.append(f"round {r} [{orders_preview}]: {reason}")
            entry = {
                "round": r,
                "status": "rejected",
                "reason": reason,
                "score": logged_score.to_dict(),
            }
            if initial_score_for_log is not None:
                entry["initial_score"] = initial_score_for_log.to_dict()
            log_entry(entry)

    return {
        "boss": boss_name,
        "best_notes_path": str(best_notes_path),
        "best_score": best_score.to_dict(),
        "rounds": history,
    }


def main() -> int:
    sys.path.insert(0, str(ROOT / "tools"))
    sys.path.insert(0, str(ROOT / "tools" / "bench"))
    import bench_bosses  # noqa: E402
    from wlblib.testing import LlamaServer  # noqa: E402

    ap = argparse.ArgumentParser(description="C2: self-revising notes.md hill-climb loop")
    ap.add_argument("--boss", required=True, choices=sorted(bench_bosses.BOSSES))
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--runs", type=int, default=3, help="Short bench runs per candidate")
    ap.add_argument(
        "--confirm-runs",
        type=int,
        default=3,
        help="Confirmation bench runs before keeping an improvement (0 disables)",
    )
    ap.add_argument("--parallel", type=int, default=2)
    ap.add_argument("--notes-file", default="", help="Starting notes.md (default: BOSSES[boss]['notes_md'])")
    ap.add_argument("--out-dir", default="", help="Output dir (default: var/notes_hill_climb/<boss>)")
    ap.add_argument("--llm-url", default="http://127.0.0.1:8090/v1/chat/completions")
    args = ap.parse_args()

    spec = bench_bosses.BOSSES[args.boss]
    initial_notes = (
        pathlib.Path(args.notes_file).read_text() if args.notes_file else spec["notes_md"]
    )
    out_dir = pathlib.Path(args.out_dir) if args.out_dir else (VAR / "notes_hill_climb" / args.boss)

    # Keep one persistent LlamaServer alive across both bench_bosses.py and reflection calls.
    with LlamaServer(slots=16):
        summary = run_notes_hill_climb(
            boss_key=args.boss,
            initial_notes=initial_notes,
            out_dir=out_dir,
            rounds=args.rounds,
            runs_per_eval=args.runs,
            confirm_runs=args.confirm_runs,
            parallel=args.parallel,
            llm_url=args.llm_url,
        )
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
