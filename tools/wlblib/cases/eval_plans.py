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


def load_cases(names=(), live=False):
    """The cases to score: by default those whose options live bots have; `planned` for the
    ones that need options the module doesn't offer yet (Talk, Rest, Explore, Go to town:
    review 2026-11-17, finding 5), or cases by name.

    `held_out` cases are the true held-out split: fresh scenes never used to tune RULES.md, so
    only the live check (`live=True`) scores them; the T=0 run, the one wording is tuned on,
    never sees them, not even by name."""
    cases = json.loads(CASES_FILE.read_text())
    by_name = {c["name"]: c for c in cases}
    if not names or list(names) == [NAME]:
        return [c for c in cases if not c.get("planned") and (live or not c.get("held_out"))]
    if list(names) == ["planned"]:
        return [c for c in cases if c.get("planned")]
    if list(names) == ["held_out"]:
        if not live:
            raise Failed("held-out cases are scored only by `wlb test eval --live`")
        return [c for c in cases if c.get("held_out") and not c.get("planned")]
    unknown = [n for n in names if n not in by_name]
    if unknown:
        raise Failed(f"no eval case {', '.join(unknown)} (have: {', '.join(by_name)})")
    held = [n for n in names if by_name[n].get("held_out")]
    if held and not live:
        raise Failed(f"{', '.join(held)}: held-out, scored only by `wlb test eval --live` "
                     "(never tune RULES.md on them)")
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
    for req_opt in case.get("must_include_options", ()):
        if req_opt not in all_opts:
            problems.append(f"plan never includes option {req_opt}")
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
    acts = got.get("acts", ())
    if case.get("forbid_act") and acts:
        problems.append(f"wrote unexpected acts: {acts}")
    for needle in case.get("must_act_contain", ()):
        if not any(needle.lower() in a.lower() for a in acts):
            problems.append(f"no act mentions {needle!r} (acts: {acts or 'none'})")
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
                 "tell": "", "open": [], "hover": "", "orders": [], "acts": []}
        plans.append(plain)
        whisperers = (
            WHISPER_FROM_RE.findall(case["reason"])
            + list(case.get("whisperers", []))
        ) or ["Someone"]
        plans.append({**plain, "tell_to": whisperers[0], "tell": "hi", "open": ["bags"],
                      "hover": "x", "look_s": 10, "orders": ["Defile on me -> away"],
                      "acts": ["wake"]})
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


def act_line(got):
    """The reply's `act:` line (done once, at once), or "": decide's JSON has no field for it."""
    return next((line for line in got.get("raw", "").splitlines() if line.startswith("act:")), "")


# The held-out check at the live harness's temperature (`wlb test eval --live`): the T=0 run is
# what RULES.md wording is tuned on, so it can overfit one sampled answer per case; live play
# samples at LIVE_TEMPERATURE, where a case should pass most of LIVE_SAMPLES draws.
LIVE_TEMPERATURE = 0.7
LIVE_SAMPLES = 3
# Mean share of scored cases a live run must pass (T=0 scores ~57/59 = 97%).
LIVE_MIN_PASS_RATE = 0.85


def run_live(cases, samples=LIVE_SAMPLES, temperature=LIVE_TEMPERATURE):
    """Every scored case `samples` times at `temperature`; prints each case's passes and the mean
    score, and returns {"mean_rate", "passed_draws", "draws", "unstable": [...]} for the tuned
    cases plus "held_out": the same for the held-out ones (None if none ran)."""
    ensure_harness()
    say(f"== eval (live: T={temperature:g}, {samples} draws per case) ==")
    cases = [c for c in cases if can_fail(c)]
    rows = []
    for case in cases:
        verdicts, firsts = [], []
        for i in range(samples):
            got = decide_one(case, temperature=temperature, slot=0)
            problems = check_case(case, got)
            verdicts.append(not problems)
            firsts.append(got.get("plan_desc", "INVALID") if not problems
                          else f"FAIL: {problems[0]}")
        n = sum(verdicts)
        mark = "PASS" if n == samples else ("FAIL" if n == 0 else "FLAKY")
        tag = " [held-out]" if case.get("held_out") else ""
        say(f"  {mark:5s} {n}/{samples} {case['name']:25s}{tag} -> {' | '.join(firsts)}")
        rows.append((case["name"], n))
    held = {c["name"] for c in cases if c.get("held_out")}
    tuned_rows = [(name, n) for name, n in rows if name not in held]
    held_rows = [(name, n) for name, n in rows if name in held]
    tuned = _live_score("live score", tuned_rows, samples)
    result = {**tuned, "held_out": None}
    if held_rows:
        # Reported, not gated: a held-out miss is a finding about RULES.md, not a case to tune.
        result["held_out"] = _live_score("held-out score", held_rows, samples)
    return result


def _live_score(label, rows, samples):
    draws = samples * len(rows)
    passed = sum(n for _, n in rows)
    rate = passed / draws if draws else 0.0
    unstable = [name for name, n in rows if n < samples]
    say(f"{label}: {passed}/{draws} draws ({100 * rate:.0f}%; mean {passed / samples:.1f}/"
        f"{len(rows)} cases), all draws pass on {len(rows) - len(unstable)}/{len(rows)}")
    return {"mean_rate": rate, "passed_draws": passed, "draws": draws, "unstable": unstable}


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
        if act_line(got):
            desc += f" | {act_line(got)}"
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
    # Cases without `must_act_contain` shouldn't write an `act:` line (not scored yet:
    # content/RULES.md says to write one only for a player's ask; this keeps the count in view).
    asked_act = {c["name"] for c in cases if c.get("must_act_contain")}
    unasked = [r["case"] for r in rows if act_line(r["got"]) and r["case"] not in asked_act]
    say(f"unasked act: lines: {len(unasked)}" + (f" ({', '.join(unasked)})" if unasked else ""))
    return {"label": label or "default", "passed": passed, "total": len(rows),
            "median_first_ms": med_first_ms, "median_ms": med_ms, "rows": rows}
