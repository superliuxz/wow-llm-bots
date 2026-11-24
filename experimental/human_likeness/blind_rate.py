#!/usr/bin/env python3
"""Interactive batch blind pairwise rater (ground truth for Layer B ranking).

Usage:
  # Rate a batch of 30 matched-situation pairs from named trace directories:
  tools/bench/blind_rate.py rate \
    --source human=/home/will/.local/share/wow-llm-bots/human_baseline \
    --source current=var/soak_traces_baseline \
    --source candidate=var/soak_traces \
    --pairs 30

  # Re-check LLM judge agreement against your recorded blind ratings:
  tools/bench/blind_rate.py llm-agree --ratings ~/.local/share/wow-llm-bots/blind_ratings.jsonl

  # Show Bradley-Terry ratings + 95% CIs across all rated entrants:
  tools/bench/blind_rate.py summary
"""

from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import random
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import pairwise_ranker  # noqa: E402
import trace_parity  # noqa: E402

DEFAULT_RATINGS_FILE = (
    pathlib.Path.home() / ".local" / "share" / "wow-llm-bots" / "blind_ratings.jsonl"
)


def _load_clips_by_situation(
    source_dir: pathlib.Path,
    role_filter: str | None = None,
    clip_s: float = 45.0,
) -> tuple[dict[str, list[dict[str, Any]]], set[str]]:
    traces = trace_parity.load_traces(source_dir)
    by_sit: dict[str, list[dict[str, Any]]] = {}
    players: set[str] = set()
    roles = (role_filter,) if role_filter else ("human", "bot")
    for role in roles:
        for name, rows in traces.get(role, {}).items():
            players.add(name)
            for clip in trace_parity.extract_clips(rows, clip_s=clip_s):
                by_sit.setdefault(clip["situation"], []).append(clip)
    return by_sit, players


def sample_matched_pairs(
    sources: dict[str, dict[str, list[dict[str, Any]]]],
    n_pairs: int = 30,
    seed: int | None = None,
    exclude_labels: set[str] | None = None,
) -> list[dict[str, Any]]:
    """Samples `n_pairs` situation-matched anonymized pairs across sources."""
    rng = random.Random(seed) if seed is not None else random.SystemRandom()
    excluded = exclude_labels or set()
    names = sorted(k for k in sources.keys() if k not in excluded)
    if len(names) < 2:
        return []

    # Collect all (sit, name_i, name_j) combinations that share at least 1 clip.
    candidates: list[tuple[str, str, str]] = []
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            ni, nj = names[i], names[j]
            shared = set(sources[ni].keys()) & set(sources[nj].keys())
            for sit in sorted(shared):
                if sources[ni][sit] and sources[nj][sit]:
                    candidates.append((sit, ni, nj))
    if not candidates:
        return []

    pairs: list[dict[str, Any]] = []
    for _ in range(n_pairs):
        sit, left, right = rng.choice(candidates)
        if rng.choice([True, False]):
            left, right = right, left
        clip_a = rng.choice(sources[left][sit])
        clip_b = rng.choice(sources[right][sit])
        pairs.append({
            "situation": sit,
            "a_label": left,
            "b_label": right,
            "clip_a": clip_a,
            "clip_b": clip_b,
        })
    return pairs


def load_ratings(path: pathlib.Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    out: list[dict[str, Any]] = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return out


def append_rating(path: pathlib.Path, row: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as f:
        f.write(json.dumps(row) + "\n")


def cmd_rate(args) -> int:
    sources: dict[str, dict[str, list[dict[str, Any]]]] = {}
    source_ids: dict[str, str] = {}
    exclude_labels: set[str] = set()
    for spec in args.source:
        if "=" not in spec:
            print(f"ERROR: --source must be LABEL=PATH, got {spec}", file=sys.stderr)
            return 2
        label, path_str = spec.split("=", 1)
        src_path = pathlib.Path(path_str)
        role_filter = "human" if label.startswith("human") else None
        clips_by_sit, players = _load_clips_by_situation(
            src_path, role_filter=role_filter, clip_s=args.clip_s
        )
        sources[label] = clips_by_sit
        source_ids[label] = trace_parity.trace_set_hash(src_path, role=role_filter) or label
        total_clips = sum(len(v) for v in clips_by_sit.values())
        print(
            f"loaded {label} (id={source_ids[label]}): {total_clips} clips across "
            f"{sorted(clips_by_sit.keys())} ({len(players)} player(s))"
        )
        # Prevent self-recognition bias: when you are the only human player,
        # human-vs-bot pairs aren't blind, so show bot-vs-bot pairs only unless
        # external judges or multiple human players exist (--include-human-anchors).
        if label.startswith("human") and not args.include_human_anchors and len(players) < 2:
            exclude_labels.add(label)
            print(
                f"note: excluding '{label}' from self-blind batch (only {len(players)} human "
                "player; pass --include-human-anchors when rating other humans or using external judges)"
            )

    pairs = sample_matched_pairs(
        sources, n_pairs=args.pairs, seed=args.seed, exclude_labels=exclude_labels
    )
    if not pairs:
        print("ERROR: no matched-situation pairs available across non-excluded sources.", file=sys.stderr)
        return 1

    layer = "social" if args.layer == "social" else "body"
    rated = 0
    for idx, p in enumerate(pairs, 1):
        print("\n" + "=" * 72)
        print(f"PAIR {idx}/{len(pairs)}  [situation: {p['situation']}]  (labels hidden)")
        print("=" * 72)
        print("\n--- CLIP A ---")
        print(pairwise_ranker.render_clip_view(p["clip_a"], layer=layer))
        print("\n--- CLIP B ---")
        print(pairwise_ranker.render_clip_view(p["clip_b"], layer=layer))
        while True:
            ans = input("\nWhich is MORE human-like? [a / b / t(ie) / s(kip) / q(uit)]: ").strip().lower()
            if ans in ("a", "b", "t", "tie", "s", "skip", "q", "quit"):
                break
        if ans in ("q", "quit"):
            break
        if ans in ("s", "skip"):
            continue
        winner = "tie" if ans in ("t", "tie") else ans
        row = {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "layer": layer,
            "situation": p["situation"],
            "a": p["a_label"],
            "b": p["b_label"],
            "a_id": source_ids.get(p["a_label"], ""),
            "b_id": source_ids.get(p["b_label"], ""),
            "winner": winner,
            "view_a": pairwise_ranker.render_clip_view(p["clip_a"], layer=layer),
            "view_b": pairwise_ranker.render_clip_view(p["clip_b"], layer=layer),
        }
        append_rating(args.ratings, row)
        rated += 1

    print(f"\nRecorded {rated} blind ratings -> {args.ratings}")
    return cmd_summary(args)


def cmd_llm_agree(args) -> int:
    rows = load_ratings(args.ratings)
    if not rows:
        print(f"No ratings found in {args.ratings}", file=sys.stderr)
        return 1
    batch = rows[-args.last :] if args.last > 0 else rows
    human_labels: list[str] = []
    llm_labels: list[str] = []
    human_vs_bot_pairs = 0
    for r in batch:
        va = r.get("view_a")
        vb = r.get("view_b")
        if not va or not vb:
            continue
        if str(r.get("a", "")).startswith("human") or str(r.get("b", "")).startswith("human"):
            human_vs_bot_pairs += 1
        layer = r.get("layer", "body")
        r1 = pairwise_ranker._ask_llm_once(va, vb, args.llm_url, layer)
        r2s = pairwise_ranker._ask_llm_once(vb, va, args.llm_url, layer)
        if r1 is None:
            continue
        if r2s is not None:
            r2 = {"a": "b", "b": "a", "tie": "tie"}[r2s]
            llm_ans = r1 if r1 == r2 else "tie"
        else:
            llm_ans = r1
        human_labels.append(r["winner"])
        llm_labels.append(llm_ans)

    res = pairwise_ranker.check_judge_agreement(human_labels, llm_labels)
    if human_vs_bot_pairs < 10:
        res["valid"] = False
        res["reason"] = f"insufficient human-vs-bot pairs ({human_vs_bot_pairs} < 10)"
    pairwise_ranker.update_judge_gate(
        {
            "llm_url": args.llm_url,
            "agreement": res["agreement"],
            "kappa": res["kappa"],
            "agreement_n": res["n"],
            "human_vs_bot_n": human_vs_bot_pairs,
            "agreement_valid": res["valid"],
            "agreement_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        },
        path=args.judge_gate,
    )
    print(
        f"LLM judge agreement on {res['n']} blind pairs (human-vs-bot={human_vs_bot_pairs}): "
        f"agree={res['agreement']*100:.1f}%, kappa={res['kappa']:.3f}, "
        f"valid={'YES' if res['valid'] else 'NO'} (saved -> {args.judge_gate})"
    )
    return 0 if res["valid"] else 1


def cmd_summary(args) -> int:
    rows = load_ratings(args.ratings)
    if not rows:
        print(f"No blind ratings yet in {args.ratings}")
        return 0
    bt = pairwise_ranker.fit_bradley_terry(rows, anchor_label=args.anchor)
    anchor_str = (
        f"anchor={bt['anchor']}={pairwise_ranker.HUMAN_ANCHOR_RATING:.0f}"
        if bt.get("anchored")
        else f"UNANCHORED (anchor '{bt['anchor']}' missing from ratings)"
    )
    print(f"\n== Blind Bradley-Terry ratings ({bt['comparisons']} pairs, {anchor_str}) ==")
    for name, info in sorted(bt["ratings"].items(), key=lambda kv: -kv[1]["rating"]):
        print(
            f"  {name:20s}: {info['rating']:6.1f}  "
            f"[95% CI {info['ci_low']:6.1f} .. {info['ci_high']:6.1f}]  "
            f"({info['games']} games)"
        )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Interactive blind pairwise clip rater")
    ap.add_argument("--ratings", type=pathlib.Path, default=DEFAULT_RATINGS_FILE)
    ap.add_argument("--anchor", default="human")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_rate = sub.add_parser("rate", help="Rate a blind batch of matched pairs")
    p_rate.add_argument("--source", action="append", required=True, help="LABEL=TRACE_DIR (repeat >=2x)")
    p_rate.add_argument("--pairs", type=int, default=30)
    p_rate.add_argument("--clip-s", type=float, default=45.0)
    p_rate.add_argument("--layer", choices=["body", "social"], default="body")
    p_rate.add_argument("--seed", type=int, default=None)
    p_rate.add_argument(
        "--include-human-anchors",
        action="store_true",
        help="Include human clips even with <2 distinct human players (for external judges)",
    )

    p_agree = sub.add_parser("llm-agree", help="Check LLM judge agreement with your blind ratings")
    p_agree.add_argument("--llm-url", default="http://127.0.0.1:8090")
    p_agree.add_argument("--last", type=int, default=30)
    p_agree.add_argument(
        "--judge-gate", type=pathlib.Path, default=pairwise_ranker.DEFAULT_JUDGE_GATE_FILE
    )

    sub.add_parser("summary", help="Print Bradley-Terry ratings from recorded blind ratings")

    args = ap.parse_args()
    return {
        "rate": cmd_rate,
        "llm-agree": cmd_llm_agree,
        "summary": cmd_summary,
    }[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
