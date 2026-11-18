"""Offline plan quality eval: saved snapshots run through `harness decide`, scored.

Each case in harness/testdata/eval/cases.json pairs a real (or hand-edited) snapshot
with properties a sensible plan must satisfy in terms of that screen's options.
"""

import json
import re
import statistics
import subprocess

from wlblib import build
from wlblib.common import OUT, ROOT, Failed, say

NAME = "plans"
DESCRIPTION = "plan sensibility across saved snapshots via `harness decide`"

EVAL_DIR = ROOT / "harness" / "testdata" / "eval"
CASES_FILE = EVAL_DIR / "cases.json"
RULES_FILE = ROOT / "content" / "RULES.md"
HARNESS_BIN = OUT / "common" / "harness"

SHOOTOUT_MODELS = (
    "Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf",
    "gemma-4-26B-A4B-it-UD-Q4_K_XL.gguf",
)

REPLY_RE = re.compile(
    r"^== reply \(([\d.]+) ms(?:, first line ([\d.]+) ms)?; "
    r"prompt (\d+) tokens, (\d+) read again;",
    re.M,
)
PLAN_RE = re.compile(r"^== plan: (.+); look again in (\d+) s$", re.M)
STEP_RE = re.compile(r"^(\d+)(?:x(\d+))? (.+)$")
GOAL_RE = re.compile(r"^goal:\s*(.*)$", re.M | re.I)
TELL_RE = re.compile(r"^tell ([A-Za-z]+):\s*(.*)$", re.M | re.I)
OPEN_RE = re.compile(r"^open: (.+)$", re.M)
HOVER_RE = re.compile(r"^hover: (.+)$", re.M)
# A numbered line of the screen: "  3. Attack Mottled Boar: level 2 boar, ..." -> 3, the text
# before the details.
OPTION_RE = re.compile(r"^\s+(\d+)\. ([^:\n]+)", re.M)


def load_cases(names=()):
    """The cases to score: by default those whose options live bots have; `planned` for the
    ones that need options the module doesn't offer yet (Talk, Rest, Explore, Go to town:
    review 2026-11-17, finding 5), or cases by name."""
    cases = json.loads(CASES_FILE.read_text())
    by_name = {c["name"]: c for c in cases}
    if not names or list(names) == [NAME]:
        return [c for c in cases if not c.get("planned")]
    if list(names) == ["planned"]:
        return [c for c in cases if c.get("planned")]
    unknown = [n for n in names if n not in by_name]
    if unknown:
        raise Failed(f"no eval case {', '.join(unknown)} (have: {', '.join(by_name)})")
    return [by_name[n] for n in names]


def case_names():
    return [c["name"] for c in json.loads(CASES_FILE.read_text())]


def ensure_harness():
    build.common("default", tests=False)


WHISPER_FROM_RE = re.compile(r"whisper from ([A-Za-z]+)")


def decide_one(case, temperature=0.0, slot=0):
    snap_path = EVAL_DIR / f"{case['name']}.snapshot.json"
    cmd = [
        str(HARNESS_BIN), "decide", str(snap_path),
        f"--reason={case['reason']}",
        f"--rules={RULES_FILE}",
        f"--slot={slot}",
        f"--temperature={temperature}",
    ]
    # Who whispered (as the live harness remembers it): who a `tell` line may answer.
    whisperers = WHISPER_FROM_RE.findall(case["reason"])
    if whisperers:
        cmd.append(f"--whisperers={','.join(whisperers)}")
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = res.stdout
    reply_m = REPLY_RE.search(out)
    ms = float(reply_m.group(1)) if reply_m else 0.0
    first_ms = float(reply_m.group(2) or ms) if reply_m else 0.0
    prompt_tok = int(reply_m.group(3)) if reply_m else 0
    prompt_comp = int(reply_m.group(4)) if reply_m else 0
    plan_m = PLAN_RE.search(out)
    if res.returncode != 0 or not plan_m:
        return {"valid": False, "ms": ms, "first_ms": first_ms, "steps": [],
                "look_s": 0, "goal": "", "raw": out[-300:]}
    steps = []
    for part in plan_m.group(1).split(", "):
        sm = STEP_RE.match(part.strip())
        if sm:
            steps.append({
                "option": int(sm.group(1)),
                "times": int(sm.group(2) or 1),
                "kind": sm.group(3),
            })
    goal_m = GOAL_RE.search(out)
    screen = out.split("== reply", 1)[0]
    option_texts = {int(n): text.strip() for n, text in OPTION_RE.findall(screen)}
    reply = out.split("== reply", 1)[1] if "== reply" in out else ""
    tell_m = TELL_RE.search(reply)
    open_m = OPEN_RE.search(reply)
    hover_m = HOVER_RE.search(reply)
    for step in steps:
        step["text"] = option_texts.get(step["option"], "")
    return {
        "valid": True,
        "ms": ms,
        "first_ms": first_ms,
        "prompt_tokens": prompt_tok,
        "prompt_computed": prompt_comp,
        "steps": steps,
        "plan_desc": plan_m.group(1),
        "look_s": int(plan_m.group(2)),
        "goal": goal_m.group(1).strip() if goal_m else "",
        "tell_to": tell_m.group(1).capitalize() if tell_m else "",
        "tell": tell_m.group(2).strip() if tell_m else "",
        "open": [p.strip() for p in open_m.group(1).split(",")] if open_m else [],
        "hover": hover_m.group(1).strip() if hover_m else "",
    }


def check_case(case, got):
    """Returns a list of violated requirements (empty means PASS)."""
    if not got["valid"] or not got["steps"]:
        return ["model did not produce a valid plan"]
    problems = []
    first = got["steps"][0]
    all_opts = {s["option"] for s in got["steps"]}
    all_kinds = {s["kind"] for s in got["steps"]}

    if "first_options" in case and first["option"] not in case["first_options"]:
        problems.append(f"first step option {first['option']} ({first['kind']}) "
                        f"not in {case['first_options']}")
    if "first_kinds" in case and first["kind"] not in case["first_kinds"]:
        problems.append(f"first step '{first['kind']}' (opt {first['option']}) "
                        f"not in {case['first_kinds']}")
    for req_kind in case.get("must_include_kinds", ()):
        if req_kind not in all_kinds:
            problems.append(f"plan never includes '{req_kind}'")
    for bad_opt in case.get("forbid_options", ()):
        if bad_opt in all_opts:
            problems.append(f"plan chose forbidden option {bad_opt}")
    for bad_kind in case.get("forbid_kinds", ()):
        if bad_kind in all_kinds:
            problems.append(f"plan chose forbidden kind '{bad_kind}'")
    # Option texts as the screen wrote them; a check matches a text by its start
    # ("Attack Hare", "Keep doing").
    def matches(text, prefixes):
        return any(text.startswith(p) for p in prefixes)

    if "first_texts" in case and not matches(first["text"], case["first_texts"]):
        problems.append(f"first step '{first['text']}' not one of {case['first_texts']}")
    for step in got["steps"]:
        if matches(step["text"], case.get("forbid_texts", ())):
            problems.append(f"plan chose forbidden '{step['text']}'")
    if "must_tell_to" in case and got["tell_to"] != case["must_tell_to"]:
        problems.append(f"no whisper back to {case['must_tell_to']}"
                        + (f" (told {got['tell_to']})" if got["tell_to"] else ""))
    if case.get("forbid_tell_to") and got["tell_to"] == case["forbid_tell_to"]:
        problems.append(f"whispered {got['tell_to']}: {got['tell']}")
    if case.get("forbid_tell") and got["tell_to"]:
        problems.append(f"whispered {got['tell_to']} unprompted: {got['tell']}")
    # Looking (docs/plans/options-and-panels.md): `open:` / `hover:` cost a second wake.
    for panel in case.get("must_open", ()):
        if panel not in got["open"]:
            problems.append(f"didn't open {panel} (open: {got['open'] or 'none'})")
    if "must_hover" in case and not got["hover"].startswith(case["must_hover"]):
        problems.append(f"didn't hover {case['must_hover']} (hover: {got['hover'] or 'none'})")
    if case.get("forbid_look") and (got["open"] or got["hover"]):
        problems.append(f"looked for no reason (open: {got['open']}, hover: {got['hover']!r})")
    if "min_look_s" in case and got["look_s"] < case["min_look_s"]:
        problems.append(f"look {got['look_s']}s < {case['min_look_s']}s")
    return problems


def run_suite(cases, label=None):
    ensure_harness()
    if label:
        say(f"== eval ({label}) ==")
    rows = []
    for case in cases:
        got = decide_one(case)
        problems = check_case(case, got)
        ok = not problems
        desc = (f"{got.get('plan_desc', 'INVALID')}; look {got['look_s']}s "
                f"(do: {got['first_ms']:.0f} ms / total {got['ms']:.0f} ms, "
                f"read {got.get('prompt_computed', 0)}/{got.get('prompt_tokens', 0)} tok)")
        if got.get("goal"):
            desc += f" | goal: {got['goal']}"
        if got.get("tell_to"):
            desc += f" | tell {got['tell_to']}: {got['tell']}"
        if got.get("open"):
            desc += f" | open: {', '.join(got['open'])}"
        if got.get("hover"):
            desc += f" | hover: {got['hover']}"
        verdict = "PASS" if ok else f"FAIL ({'; '.join(problems)})"
        say(f"  {verdict:4s} {case['name']:25s} -> {desc}")
        rows.append({"case": case["name"], "ok": ok, "got": got, "problems": problems})
    passed = sum(r["ok"] for r in rows)
    med_ms = statistics.median(r["got"]["ms"] for r in rows) if rows else 0.0
    med_first_ms = statistics.median(r["got"]["first_ms"] for r in rows) if rows else 0.0
    warm_comp = [r["got"].get("prompt_computed", 0) for r in rows[1:] if r["got"]["valid"]]
    med_comp = statistics.median(warm_comp) if warm_comp else 0
    pct = 100.0 * passed / len(rows) if rows else 0.0
    say(f"score{f' [{label}]' if label else ''}: {passed}/{len(rows)} ({pct:.0f}%), "
        f"first line {med_first_ms:.0f} ms, total {med_ms:.0f} ms, "
        f"warm prompt_computed {med_comp:.0f} tok")
    return {"label": label or "default", "passed": passed, "total": len(rows),
            "median_first_ms": med_first_ms, "median_ms": med_ms, "rows": rows}
