#!/usr/bin/env python3
"""Soak chatter checker: flags repeated, narrating, or sycophantic bot chat.

Scans harness decision logs (`<decisions_dir>/bots/<Name>/events/*.jsonl` or
`<dir>/<Name>/events/*.jsonl` or individual `.jsonl` files), extracts outbound
chat lines (`say:`, `party:`, `general:`, `lfg:`, `guild:`, `tell <Name>:`),
and counts flagged lines per bot-hour against a configurable ceiling.

Uses fast deterministic string/regex/similarity checks first, with an optional
pluggable LLM judge (`--llm-judge` or `judge=...`) for nuanced cases.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable
import dataclasses
import difflib
import json
import pathlib
import re
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

from wlblib import llm as llm_lib  # noqa: E402

DEFAULT_MAX_CHATTER_PER_HOUR = 6.0
DEFAULT_REPEAT_WINDOW_S = 600.0
DEFAULT_SIMILARITY_THRESHOLD = 0.80
DEFAULT_SHORT_ACK_REPEAT_WINDOW_S = 60.0
DEFAULT_SESSION_EXACT_REPEAT_LIMIT = 2  # 3rd+ exact non-trivial repeat in session.

# Ultra-short standard WoW acknowledgements that are normal to reuse across
# different situations in a session (still flagged if spammed within 60 s or
# repeated 3+ times in the short window).
SHORT_ACKS = frozenset({
    "ty",
    "thx",
    "thanks",
    "np",
    "yw",
    "r",
    "ready",
    "gg",
    "gj",
    "tyfg",
    "ok",
    "k",
    "kk",
    "yes",
    "no",
    "yep",
    "yup",
    "nope",
    "sure",
    "sry",
    "mb",
    "oom",
    "omw",
    "brb",
    "sec",
    "inc",
    "nice",
    "gz",
    "grats",
    "ding",
    "wait",
    "got it",
    "my bad",
    "sounds good",
})

_CHAT_LINE_RE = re.compile(
    r"^(say|party|general|lfg|guild)\s*:\s*(.+)$",
    re.IGNORECASE,
)
_TELL_LINE_RE = re.compile(
    r"^tell\s+([A-Za-z][A-Za-z0-9_-]{1,23})\s*:\s*(.+)$",
    re.IGNORECASE,
)
_NON_ALNUM_RE = re.compile(r"[^a-z0-9\s]+")
_MULTI_SPACE_RE = re.compile(r"\s+")

_ROUTINE_GERUNDS = (
    r"attacking|fighting|killing|looting|resting|drinking|eating|walking|"
    r"running|heading|moving|following|exploring|casting|healing|buffing|"
    r"equipping|selling|buying|training|turning in|handing in|accepting|"
    r"talking to|gathering|mining|herbing|skinning"
)
_ROUTINE_VERBS = (
    r"attack|fight|kill|loot|rest|drink|eat|walk|run|head|move|follow|"
    r"explore|equip|sell|buy|train|turn in|hand in|accept|talk to|"
    r"gather|mine|herb|skin"
)

NARRATION_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "first_person_narration",
        re.compile(
            rf"\b(?:i am|i'm|im|i will|i'll|going to|gonna|about to|currently|just)\s+"
            rf"(?:{_ROUTINE_GERUNDS})\b",
            re.IGNORECASE,
        ),
    ),
    (
        "first_person_intent",
        re.compile(
            rf"\b(?:(?:i am|i'm|im|i will|i'll)\s+(?:now\s+)?(?:going to\s+|gonna\s+)?|going to\s+|gonna\s+|about to\s+)"
            rf"(?:{_ROUTINE_VERBS})\s+(?:the|a|an|this|that|my|our|some|over|back|to)\b",
            re.IGNORECASE,
        ),
    ),
    (
        "gerund_play_by_play",
        re.compile(
            rf"^(?:now\s+)?(?:{_ROUTINE_GERUNDS})\s+"
            rf"(?:the|a|an|this|that|my|our|to|back|over|for|some)\b",
            re.IGNORECASE,
        ),
    ),
    (
        "time_to_narration",
        re.compile(
            rf"\b(?:time to|let me|now to)\s+(?:{_ROUTINE_VERBS})\b",
            re.IGNORECASE,
        ),
    ),
    (
        "obvious_state_commentary",
        re.compile(
            r"\b(?:"
            r"(?:my|our)\s+(?:health|hp|mana)\s+is\s+(?:full|restored|back|topped|good)|"
            r"(?:that|the)\s+(?:mob|enemy|boss|target|fight|pack)\s+is\s+(?:dead|down|over|cleared)|"
            r"(?:we are|we're|i am|i'm)\s+(?:now\s+)?(?:in combat|out of combat)|"
            r"(?:combat is over|finished looting|done looting|all looted|i leveled up)"
            r")\b",
            re.IGNORECASE,
        ),
    ),
)

SYCOPHANCY_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "assistant_compliance",
        re.compile(
            r"\b(?:"
            r"absolutely|certainly|of course|happy to help|glad to help|"
            r"my pleasure|right away|consider it done|at your service|"
            r"whatever you say|you got it,?\s*boss|as you wish|happy to oblige|"
            r"i would be happy to|i'd be happy to"
            r")\b",
            re.IGNORECASE,
        ),
    ),
    (
        "excessive_praise",
        re.compile(
            r"\b(?:"
            r"(?:great|awesome|brilliant|excellent|fantastic|wonderful|amazing|superb)\s+"
            r"(?:idea|plan|call|strategy|lead|leadership|point|job\s+everyone)|"
            r"you(?:'re| are)\s+(?:the best|amazing|awesome|incredible|brilliant|so good|a great|an amazing|totally right|absolutely right)|"
            r"best\s+(?:tank|healer|leader|player|group|party)\s+ever|"
            r"couldn't\s+(?:do it without you|agree more)|"
            r"spot on|"
            r"thank you so much for|i really appreciate\s+(?:you|your|that)"
            r")\b",
            re.IGNORECASE,
        ),
    ),
)


@dataclasses.dataclass(frozen=True)
class ChatLine:
    """One outbound chat line emitted by a bot decision."""

    bot: str
    t_ms: int
    channel: str
    text: str
    reason: str = ""
    screen: str = ""


# Pluggable judge signature: (line, recent_bot_lines) -> category/reason or None.
JudgeFn = Callable[[ChatLine, list[ChatLine]], str | None]


def normalize_text(text: str) -> str:
    """Lowercases and strips punctuation/extra spaces for repetition checks."""
    low = text.strip().lower()
    cleaned = _NON_ALNUM_RE.sub(" ", low)
    return _MULTI_SPACE_RE.sub(" ", cleaned).strip()


def text_similarity(norm_a: str, norm_b: str) -> float:
    """Returns max of SequenceMatcher ratio and token Jaccard similarity."""
    if not norm_a or not norm_b:
        return 0.0
    if norm_a == norm_b:
        return 1.0
    seq_ratio = difflib.SequenceMatcher(None, norm_a, norm_b).ratio()
    tok_a = set(norm_a.split())
    tok_b = set(norm_b.split())
    union = tok_a | tok_b
    jaccard = (len(tok_a & tok_b) / len(union)) if union else 0.0
    return max(seq_ratio, jaccard)


def extract_plan_chats(plan_text: str) -> list[tuple[str, str]]:
    """Extracts `(channel, text)` chat lines from a formatted plan string."""
    if not isinstance(plan_text, str) or not plan_text.strip():
        return []
    out: list[tuple[str, str]] = []
    for raw_line in plan_text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        m = _CHAT_LINE_RE.match(line)
        if m:
            channel = m.group(1).lower()
            text = m.group(2).strip()
            if text:
                out.append((channel, text))
            continue
        mt = _TELL_LINE_RE.match(line)
        if mt:
            target = mt.group(1)
            text = mt.group(2).strip()
            if text:
                out.append((f"tell:{target}", text))
    return out


def _iter_decision_files(decisions_dir: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    """Finds `(bot_name, jsonl_path)` pairs under a soak decisions directory."""
    if not decisions_dir.exists():
        return []
    if decisions_dir.is_file() and decisions_dir.suffix == ".jsonl":
        bot_name = decisions_dir.parent.parent.name if decisions_dir.parent.name == "events" else decisions_dir.stem
        return [(bot_name, decisions_dir)]

    bots_root = decisions_dir / "bots" if (decisions_dir / "bots").is_dir() else decisions_dir
    found: list[tuple[str, pathlib.Path]] = []
    for bot_dir in sorted(bots_root.iterdir()):
        if not bot_dir.is_dir():
            continue
        ev_dir = bot_dir / "events"
        search_dir = ev_dir if ev_dir.is_dir() else bot_dir
        for fp in sorted(search_dir.glob("*.jsonl")):
            found.append((bot_dir.name, fp))
    return found


def load_decisions_chats(
    decisions_dirs: list[pathlib.Path],
) -> tuple[list[ChatLine], dict[str, tuple[int, int]]]:
    """Loads outbound chat lines and per-bot `(min_t_ms, max_t_ms)` spans.

    When multiple directories are given (multi-run soaks), bot names are
    namespaced per directory (`<bot>_r00`, `<bot>_r01`) so separate runs never
    mix repetition windows or inflate time spans.
    """
    chats: list[ChatLine] = []
    bot_spans: dict[str, tuple[int, int]] = {}
    multi = len(decisions_dirs) > 1

    for idx, ddir in enumerate(decisions_dirs):
        for bot_name, fp in _iter_decision_files(ddir):
            scoped_bot = f"{bot_name}_r{idx:02d}" if multi else bot_name
            with fp.open(encoding="utf-8", errors="replace") as fh:
                for raw in fh:
                    raw = raw.strip()
                    if not raw:
                        continue
                    try:
                        rec = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    if not isinstance(rec, dict):
                        continue
                    t_ms = int(rec.get("time_ms", rec.get("t_ms", 0)))
                    if scoped_bot in bot_spans:
                        lo, hi = bot_spans[scoped_bot]
                        bot_spans[scoped_bot] = (min(lo, t_ms), max(hi, t_ms))
                    else:
                        bot_spans[scoped_bot] = (t_ms, t_ms)

                    plan = rec.get("plan")
                    if not isinstance(plan, str) or not plan:
                        continue
                    reason = str(rec.get("reason", ""))
                    screen = str(rec.get("screen", rec.get("prompt_user", "")))
                    for channel, text in extract_plan_chats(plan):
                        chats.append(
                            ChatLine(
                                bot=scoped_bot,
                                t_ms=t_ms,
                                channel=channel,
                                text=text,
                                reason=reason,
                                screen=screen,
                            )
                        )

    chats.sort(key=lambda c: (c.t_ms, c.bot, c.channel))
    return chats, bot_spans


def classify_deterministic(
    line: ChatLine,
    recent_bot_lines: list[ChatLine],
    *,
    session_exact_counts: dict[str, int] | None = None,
    repeat_window_s: float = DEFAULT_REPEAT_WINDOW_S,
    similarity_threshold: float = DEFAULT_SIMILARITY_THRESHOLD,
) -> tuple[str, str] | None:
    """Returns `(category, detail)` if `line` fails deterministic chatter checks."""
    text = line.text.strip()
    norm = normalize_text(text)
    if not norm:
        return None

    # 1. Sycophancy checks.
    for rule_name, pat in SYCOPHANCY_PATTERNS:
        if pat.search(text):
            return ("sycophantic", f"pattern:{rule_name}")

    # 2. Narration checks.
    for rule_name, pat in NARRATION_PATTERNS:
        if pat.search(text):
            return ("narrating", f"pattern:{rule_name}")

    # 3. Repetition checks (per-bot).
    is_short_ack = norm in SHORT_ACKS
    window_ms = int(
        (DEFAULT_SHORT_ACK_REPEAT_WINDOW_S if is_short_ack else repeat_window_s) * 1000.0
    )
    for prev in reversed(recent_bot_lines):
        dt_ms = line.t_ms - prev.t_ms
        if dt_ms < 0:
            continue
        if dt_ms > window_ms:
            break
        prev_norm = normalize_text(prev.text)
        if not prev_norm:
            continue
        if norm == prev_norm:
            return ("repeated", f"exact_within_{dt_ms // 1000}s")
        if not is_short_ack and len(norm) >= 16 and len(prev_norm) >= 16:
            sim = text_similarity(norm, prev_norm)
            if sim >= similarity_threshold:
                return ("repeated", f"similar({sim:.2f})_within_{dt_ms // 1000}s")

    # Catch slow catchphrase loops across a long session for non-trivial lines.
    if not is_short_ack and len(norm) >= 12 and session_exact_counts is not None:
        if session_exact_counts.get(norm, 0) >= DEFAULT_SESSION_EXACT_REPEAT_LIMIT:
            return ("repeated", f"session_repeat_count={session_exact_counts[norm] + 1}")

    return None


def make_llm_judge(
    *,
    base_url: str = llm_lib.DEFAULT_URL,
    temperature: float = 0.0,
    timeout: int = 30,
) -> JudgeFn:
    """Builds a pluggable LLM judge backed by local `llama-server`."""

    def _judge(line: ChatLine, recent_bot_lines: list[ChatLine]) -> str | None:
        recent_block = "\n".join(
            f"- [{r.channel}] {r.text}" for r in recent_bot_lines[-5:]
        ) or "(none)"
        messages = [
            {
                "role": "system",
                "content": (
                    "You judge World of Warcraft companion bot chat lines. "
                    "Normal casual WoW callouts (oom, drinking, r, ty, np, gg, inc, "
                    "sheep moon, kill skull, rez pls, short answers to players) are OK. "
                    "Flag ONLY lines that are clearly:\n"
                    "- REPEATED: repeating a recent line or catchphrase unnecessarily\n"
                    "- NARRATING: narrating routine own actions or stating the obvious\n"
                    "- SYCOPHANTIC: flattering, over-eager assistant tone, or agreeing to please\n"
                    "Reply with a single line: OK, REPEATED: <reason>, "
                    "NARRATING: <reason>, or SYCOPHANTIC: <reason>."
                ),
            },
            {
                "role": "user",
                "content": (
                    f"Bot: {line.bot}\n"
                    f"Channel: {line.channel}\n"
                    f"Recent lines by this bot:\n{recent_block}\n"
                    f"Current line: {line.text}"
                ),
            },
        ]
        reply = llm_lib.chat_completion(
            messages,
            base_url=base_url,
            temperature=temperature,
            max_tokens=40,
            timeout=timeout,
        ).strip()
        first = reply.splitlines()[0].strip() if reply else ""
        upper = first.upper()
        if not upper or upper == "OK" or upper.startswith("OK:") or upper.startswith("OK "):
            return None
        for cat in ("REPEATED", "NARRATING", "SYCOPHANTIC"):
            if upper.startswith(cat):
                detail = first[len(cat):].lstrip(":- ").strip()
                return f"{cat.lower()}: {detail}" if detail else cat.lower()
        return None

    return _judge


def _parse_judge_verdict(verdict: str | None) -> tuple[str, str] | None:
    if not verdict:
        return None
    v = verdict.strip()
    if not v or v.lower() in ("ok", "pass", "none"):
        return None
    low = v.lower()
    for cat in ("repeated", "narrating", "sycophantic"):
        if low == cat:
            return (cat, "llm_judge")
        if low.startswith(f"{cat}:"):
            detail = v[len(cat) + 1:].strip() or "llm_judge"
            return (cat, detail)
    return ("judge", v)


def check_chat_lines(
    lines: list[ChatLine],
    *,
    duration_hours: float | None = None,
    bot_spans: dict[str, tuple[int, int]] | None = None,
    max_per_hour: float = DEFAULT_MAX_CHATTER_PER_HOUR,
    repeat_window_s: float = DEFAULT_REPEAT_WINDOW_S,
    similarity_threshold: float = DEFAULT_SIMILARITY_THRESHOLD,
    judge: JudgeFn | None = None,
) -> dict[str, Any]:
    """Evaluates chat lines for repeated, narrating, or sycophantic chatter."""
    ordered = sorted(lines, key=lambda c: (c.t_ms, c.bot, c.channel))

    recent_by_bot: dict[str, list[ChatLine]] = {}
    session_counts_by_bot: dict[str, dict[str, int]] = {}
    counts: dict[str, int] = {"repeated": 0, "narrating": 0, "sycophantic": 0}
    flagged: list[dict[str, Any]] = []
    judge_calls = 0
    judge_errors = 0

    for line in ordered:
        history = recent_by_bot.setdefault(line.bot, [])
        session_counts = session_counts_by_bot.setdefault(line.bot, {})
        verdict = classify_deterministic(
            line,
            history,
            session_exact_counts=session_counts,
            repeat_window_s=repeat_window_s,
            similarity_threshold=similarity_threshold,
        )
        source = "deterministic"
        if verdict is None and judge is not None:
            judge_calls += 1
            try:
                verdict = _parse_judge_verdict(judge(line, list(history)))
                source = "judge"
            except Exception:
                judge_errors += 1
                verdict = None

        if verdict is not None:
            cat, detail = verdict
            counts[cat] = counts.get(cat, 0) + 1
            flagged.append({
                "bot": line.bot,
                "t_ms": line.t_ms,
                "channel": line.channel,
                "text": line.text,
                "category": cat,
                "source": source,
                "detail": detail,
            })

        history.append(line)
        norm = normalize_text(line.text)
        if norm:
            session_counts[norm] = session_counts.get(norm, 0) + 1

    if duration_hours is not None:
        effective_hours = max(0.0, float(duration_hours))
    elif bot_spans:
        effective_hours = sum(
            max(0.0, (hi - lo) / 3_600_000.0) for lo, hi in bot_spans.values()
        )
    else:
        inferred_spans: dict[str, tuple[int, int]] = {}
        for c in ordered:
            if c.bot in inferred_spans:
                lo, hi = inferred_spans[c.bot]
                inferred_spans[c.bot] = (min(lo, c.t_ms), max(hi, c.t_ms))
            else:
                inferred_spans[c.bot] = (c.t_ms, c.t_ms)
        effective_hours = sum(
            max(0.0, (hi - lo) / 3_600_000.0) for lo, hi in inferred_spans.values()
        )

    flagged_count = len(flagged)
    if effective_hours > 0.0:
        flagged_per_hour = flagged_count / effective_hours
    else:
        flagged_per_hour = 0.0 if flagged_count == 0 else float(flagged_count)

    ok = flagged_per_hour <= max_per_hour
    return {
        "ok": ok,
        "total_lines": len(ordered),
        "flagged_lines": flagged_count,
        "flagged_per_hour": round(flagged_per_hour, 2),
        "max_per_hour": float(max_per_hour),
        "bot_hours": round(effective_hours, 4),
        "counts": counts,
        "judge_calls": judge_calls,
        "judge_errors": judge_errors,
        "examples": flagged[:20],
        "flagged": flagged,
    }


def check_decision_dirs(
    decisions_dirs: list[pathlib.Path],
    *,
    duration_hours: float | None = None,
    max_per_hour: float = DEFAULT_MAX_CHATTER_PER_HOUR,
    repeat_window_s: float = DEFAULT_REPEAT_WINDOW_S,
    similarity_threshold: float = DEFAULT_SIMILARITY_THRESHOLD,
    judge: JudgeFn | None = None,
) -> dict[str, Any]:
    """Loads decision logs from `decisions_dirs` and runs the chatter checker."""
    chats, bot_spans = load_decisions_chats(decisions_dirs)
    return check_chat_lines(
        chats,
        duration_hours=duration_hours,
        bot_spans=bot_spans,
        max_per_hour=max_per_hour,
        repeat_window_s=repeat_window_s,
        similarity_threshold=similarity_threshold,
        judge=judge,
    )


def format_report(res: dict[str, Any]) -> str:
    c = res["counts"]
    status = "PASS" if res["ok"] else "FAIL"
    lines = [
        f"CHATTER CHECK: {status} "
        f"({res['flagged_per_hour']:.2f}/h <= {res['max_per_hour']:.2f}/h; "
        f"flagged={res['flagged_lines']}/{res['total_lines']} over {res['bot_hours']:.2f} bot-h; "
        f"repeated={c.get('repeated', 0)}, narrating={c.get('narrating', 0)}, "
        f"sycophantic={c.get('sycophantic', 0)})",
    ]
    for ex in res.get("examples", [])[:10]:
        lines.append(
            f"  [{ex['category']}/{ex['source']}] {ex['bot']} ({ex['channel']}): "
            f"{ex['text']!r} ({ex['detail']})"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "decisions_dirs",
        nargs="+",
        type=pathlib.Path,
        help="Soak decision log directories (or .jsonl files)",
    )
    ap.add_argument(
        "--max-per-hour",
        type=float,
        default=DEFAULT_MAX_CHATTER_PER_HOUR,
        help=f"Max flagged lines per bot-hour (default: {DEFAULT_MAX_CHATTER_PER_HOUR})",
    )
    ap.add_argument(
        "--duration-hours",
        type=float,
        default=None,
        help="Override total bot-hours (default: inferred from decision timestamps)",
    )
    ap.add_argument(
        "--repeat-window-s",
        type=float,
        default=DEFAULT_REPEAT_WINDOW_S,
        help=f"Repetition window in seconds (default: {DEFAULT_REPEAT_WINDOW_S})",
    )
    ap.add_argument(
        "--similarity",
        type=float,
        default=DEFAULT_SIMILARITY_THRESHOLD,
        help=f"Near-duplicate similarity threshold (default: {DEFAULT_SIMILARITY_THRESHOLD})",
    )
    ap.add_argument(
        "--llm-judge",
        action="store_true",
        help="Also run local llama-server LLM judge on deterministically-clean lines",
    )
    ap.add_argument(
        "--llm-url",
        default=llm_lib.DEFAULT_URL,
        help=f"llama-server base URL for --llm-judge (default: {llm_lib.DEFAULT_URL})",
    )
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    judge = make_llm_judge(base_url=args.llm_url) if args.llm_judge else None
    res = check_decision_dirs(
        args.decisions_dirs,
        duration_hours=args.duration_hours,
        max_per_hour=args.max_per_hour,
        repeat_window_s=args.repeat_window_s,
        similarity_threshold=args.similarity,
        judge=judge,
    )
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        print(format_report(res))
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
