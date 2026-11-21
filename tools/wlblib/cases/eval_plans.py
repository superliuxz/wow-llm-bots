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

# Maps snapshot::Option::Kind JSON names to screen_renderer::OptionKindName.
OPTION_KIND_NAME = {
    "ATTACK": "attack",
    "TALK": "talk",
    "LOOT": "loot",
    "GATHER": "gather",
    "USE": "use",
    "REST": "rest",
    "EXPLORE": "explore",
    "GO_TO": "go to town",
    "KEEP": "keep doing what I'm doing",
}


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
        "--json",
    ]
    # Who whispered (as the live harness remembers it): who a `tell` line may answer.
    whisperers = list(dict.fromkeys(
        WHISPER_FROM_RE.findall(case["reason"]) + list(case.get("whisperers", []))
    ))
    if whisperers:
        cmd.append(f"--whisperers={','.join(whisperers)}")
    if case.get("notes"):
        cmd.append(f"--notes={case['notes']}")
    if case.get("order_names"):
        cmd.append(f"--order-names={','.join(case['order_names'])}")
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    try:
        return json.loads(res.stdout)
    except json.JSONDecodeError:
        return {"valid": False, "ms": 0.0, "first_ms": 0.0, "steps": [],
                "look_s": 0, "goal": "", "raw": res.stdout[-300:]}


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
    # Looking (docs/specs/options.md): `open:` / `hover:` cost a second wake.
    for panel in case.get("must_open", ()):
        if panel not in got["open"]:
            problems.append(f"didn't open {panel} (open: {got['open'] or 'none'})")
    if "must_hover" in case and not got["hover"].startswith(case["must_hover"]):
        problems.append(f"didn't hover {case['must_hover']} (hover: {got['hover'] or 'none'})")
    if case.get("forbid_look") and (got["open"] or got["hover"]):
        problems.append(f"looked for no reason (open: {got['open']}, hover: {got['hover']!r})")
    if "min_look_s" in case and got["look_s"] < case["min_look_s"]:
        problems.append(f"look {got['look_s']}s < {case['min_look_s']}s")
    orders = got.get("orders", ())
    if case.get("forbid_orders") and orders:
        problems.append(f"wrote unexpected standing orders: {orders}")
    if "min_orders" in case and len(orders) < case["min_orders"]:
        problems.append(f"wrote {len(orders)} standing orders, expected >= {case['min_orders']}")
    for needle in case.get("must_order_contain", ()):
        matching = [o for o in orders if needle.lower() in o.lower()]
        if not matching:
            problems.append(f"no standing order mentions {needle!r} (orders: {orders or 'none'})")
        elif case.get("must_order_act"):
            acts = [a.lower() for a in case["must_order_act"]]
            if not any(
                "->" in o and any(a in o.split("->", 1)[1].lower() for a in acts)
                for o in matching
            ):
                problems.append(
                    f"order for {needle!r} does not perform any of {case['must_order_act']!r} "
                    f"(orders: {matching})"
                )
    return problems


def _probe_plans(case):
    """Every one-step plan the case's screen allows, plain and with a whisper and a look: what a
    model could answer, for the checks to be tried on."""
    snap = json.loads((EVAL_DIR / f"{case['name']}.snapshot.json").read_text())
    plans = []
    for i, opt in enumerate(snap.get("options", []), 1):
        text = opt.get("text", "").split(":")[0].strip()
        kind = OPTION_KIND_NAME.get(opt.get("kind", ""), "?")
        step = {"option": i, "times": 1, "kind": kind, "text": text}
        plain = {"valid": True, "steps": [step], "look_s": 60, "goal": "", "tell_to": "",
                 "tell": "", "open": [], "hover": "", "orders": []}
        plans.append(plain)
        whisperers = (
            WHISPER_FROM_RE.findall(case["reason"])
            + list(case.get("whisperers", []))
        ) or ["Someone"]
        plans.append({**plain, "tell_to": whisperers[0], "tell": "hi", "open": ["bags"],
                      "hover": "x", "look_s": 10, "orders": ["Defile on me -> away"]})
    return plans


def can_fail(case):
    """Whether some answer fails the case's checks. One no answer fails tells nothing about the
    brain (review of f40b0b6, finding 14: 8 in-combat cases offered only right options)."""
    return any(check_case(case, plan) for plan in _probe_plans(case))


def random_pass_rate(case):
    """The share of the screen's options that pass as a plain one-step plan: what picking an
    option at random scores on this case."""
    plain = [p for p in _probe_plans(case) if not p["tell_to"]]
    return sum(not check_case(case, p) for p in plain) / len(plain) if plain else 0.0


def run_suite(cases, label=None):
    ensure_harness()
    if label:
        say(f"== eval ({label}) ==")
    unfailable = [c["name"] for c in cases if not can_fail(c)]
    if unfailable:
        say(f"  not scored (no answer fails them): {', '.join(unfailable)}")
    cases = [c for c in cases if c["name"] not in unfailable]
    random_score = sum(random_pass_rate(c) for c in cases)
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
    say(f"score{f' [{label}]' if label else ''}: {passed}/{len(rows)} ({pct:.0f}%; a random "
        f"option scores {random_score:.1f}), "
        f"first line {med_first_ms:.0f} ms, total {med_ms:.0f} ms, "
        f"warm prompt_computed {med_comp:.0f} tok")
    return {"label": label or "default", "passed": passed, "total": len(rows),
            "median_first_ms": med_first_ms, "median_ms": med_ms, "rows": rows}
