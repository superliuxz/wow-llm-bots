"""Checks the standing-order tables against the grammar of docs/specs/triggers.md ("The grammar").

  docs/specs/trigger-conditions.tsv   every `fits` row's `when` cell must parse as a `when`;
                                      `always` rows must say `always`
  docs/specs/trigger-actions.tsv      every `fits` row's `act` cell must parse as an `<act>` sequence

Every row needs a known verdict. A name (a spell, aura, unit, object or item as a player sees it)
is free text here: the harness checks names against the bot's own notes and screen, not this script.
The Python regexes are a second copy of the grammar: once orders are parsed in
`harness/src/plan.cc`, a unit test runs the real parser over the tables and this script is retired.

Usage: python3 -I tools/check-trigger-grammar.py [conditions|actions]   (exit 1 on any failure)
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPECS = ROOT / "docs" / "specs"

# Limits (the grammar's caps).
MAX_CONDITIONS = 4  # per `when`, joined by " & "
MAX_STEPS = 3  # per order, joined by ", then "

# ---- names, things ------------------------------------------------------------------------------

NUM = r"\d+(?:\.\d+)?"
CNT = r"\d+"
CMP = r"[<>]"
NAME = r"[A-Za-z0-9'][A-Za-z0-9' :,-]*?"  # a name, as a player sees it
ICON = r"(?:skull|cross|square|moon|triangle|diamond|circle|star)"
WHO = r"(?:me|tank|offtank|healer|leader|anyone|boss|it)"
KIND = rf"(?:ally|player|enemy|{NAME})"
THING = (rf"(?:{WHO}|my target|marker {ICON}|nearest {KIND}|ally with {NAME}|group|allies|focus|"
         rf"{NAME})")
STATE = r"(?:flying|grounded|untargetable|submerged)"
MYSTATE = r"(?:in vehicle|mounted|swimming|nearest to it)"

# ---- when ---------------------------------------------------------------------------------------

TESTS = [
    rf"{NAME} on {WHO}",
    rf"{NAME} stacks{CMP}{CNT} on {WHO}",
    rf"{NAME} ends<{NUM}s on {WHO}",
    rf"{NAME} cast",
    rf"{NAME} cast on {WHO}",
    rf"{NAME} cast in last {NUM}s",
    rf"{NAME} appears",
    rf"{NAME} appears in last {NUM}s",
    rf"{NAME} count{CMP}{CNT}",
    rf"{THING} (?:within|beyond) {NUM}(?: of {THING})?",
    rf"{THING} targets {THING}",
    rf"{THING} hp{CMP}{NUM}",
    rf"mana{CMP}{NUM}",  # my mana, percent of maximum
    rf"mana_points{CMP}{CNT}",  # my mana, an absolute amount
    rf"{THING} is {STATE}",
    rf"{THING} is marked {ICON}",
    rf"{THING} (?:in|out of) sight",
    rf"me behind {THING}",
    rf"my target is {THING}",
    rf"me {MYSTATE}",
    rf"me has {NAME}",
    r"phase \d",
    rf"after {NUM}s",
    rf"every {NUM}s",
]
COND = re.compile(rf"(?:not )?(?:{'|'.join(f'(?:{t})' for t in TESTS)})")


def parses_when(when):
    if when == "always":
        return True
    conds = when.split(" & ")
    return len(conds) <= MAX_CONDITIONS and all(COND.fullmatch(c) for c in conds)


# ---- act ----------------------------------------------------------------------------------------

PLACE = (rf"(?:behind {THING}|in front of {THING}|max range of {THING}|{NUM} yd from {THING}|"
         rf"{THING})")
PET = rf"(?:attack {THING}|stay|follow|passive|defensive)"
ACTS = [
    rf"to {PLACE}",
    rf"away(?: {THING})?(?: {NUM})?",
    rf"avoid {THING}",
    rf"target {THING}",
    rf"use {NAME}(?: on {THING})?",
    r"stop(?: casting| attacking| moving)?",
    rf"face {THING}",
    rf"face away {THING}",
    r"jump",
    rf"cancel {NAME}",
    rf"mark {THING} {ICON}",
    rf"interact {THING}",
    rf"board {THING}",
    r"leave vehicle",
    rf"equip {NAME}",
    rf"pet {PET}",
    rf"kite {THING}",
    r"wake",
]
ACT = re.compile("(?:" + "|".join(f"(?:{a})" for a in ACTS) + ")")


def parses_act(act):
    steps = act.split(", then ")
    return len(steps) <= MAX_STEPS and all(ACT.fullmatch(s) for s in steps)


# ---- the tables ---------------------------------------------------------------------------------

CONDITION_VERDICTS = {"fits", "always", "unsayable", "bugnet", "plumbing", "cheat"}
ACTION_VERDICTS = {"fits", "reflex", "unsayable", "bugnet", "plumbing", "cheat"}


def check(path, verdicts, cell, parser):
    lines = path.read_text().splitlines()
    header, rows = lines[0].split("\t"), [line.split("\t") for line in lines[1:]]
    col = {name: i for i, name in enumerate(header)}
    failures = []
    for n, row in enumerate(rows, 2):
        if len(row) != len(header):
            failures.append(f"{path.name}:{n}: {len(row)} columns, want {len(header)}")
            continue
        verdict, value, name = row[col["verdict"]], row[col[cell]], row[col["class"]]
        if verdict not in verdicts:
            failures.append(f"{path.name}:{n}: {name}: unknown verdict {verdict!r}")
        elif verdict == "fits" and not parser(value):
            failures.append(f"{path.name}:{n}: {name}: doesn't parse: {value!r}")
        elif verdict == "always" and value != "always":
            failures.append(f"{path.name}:{n}: {name}: `always` row says {value!r}")
    return len(rows), failures


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    failures = []
    if which in ("all", "conditions"):
        n, f = check(SPECS / "trigger-conditions.tsv", CONDITION_VERDICTS, "when", parses_when)
        print(f"conditions: {n} rows, {len(f)} failures")
        failures += f
    if which in ("all", "actions"):
        n, f = check(SPECS / "trigger-actions.tsv", ACTION_VERDICTS, "act", parses_act)
        print(f"actions: {n} rows, {len(f)} failures")
        failures += f
    for failure in failures:
        print(failure)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
