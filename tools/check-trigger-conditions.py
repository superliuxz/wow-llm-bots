"""Checks docs/specs/trigger-conditions.tsv against the standing-order grammar.

Every row needs a known verdict; every `fits` row's `when` cell must parse as `when` in the
grammar of docs/specs/triggers.md ("The `when` side, revised"), and `always` rows must say
`always`. A `mech` (a spell, aura or unit name as a player sees it) is free text here: the
harness will check names against the bot's notes, not this script.

Usage: python3 -I tools/check-trigger-conditions.py [tsv]   (exit 1 on any failure)
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERDICTS = {"fits", "always", "unsayable", "bugnet", "plumbing", "cheat"}

NUM = r"\d+"
CMP = r"[<>]"
WHO = r"(?:me|tank|healer|anyone|boss)"
NAME = r"[A-Za-z0-9'][A-Za-z0-9' :,-]*?"  # a mech: a name, as a player sees it
THING = rf"(?:it|boss|tank|marker \w+|{NAME})"
STATE = r"(?:flying|grounded|untargetable|submerged)"
MYSTATE = r"(?:in vehicle|mounted|swimming|nearest to it)"
TESTS = [
    rf"{NAME} on {WHO}",
    rf"{NAME} stacks{CMP}{NUM} on {WHO}",
    rf"{NAME} cast on {WHO}",
    rf"{NAME} cast",
    rf"{NAME} appears",
    rf"{NAME} count>{NUM}",
    rf"{THING} (?:within|beyond) {NUM}",
    rf"{THING} targets {WHO}",
    rf"{THING} hp{CMP}{NUM}",
    rf"mana{CMP}{NUM}",
    rf"{THING} is {STATE}",
    rf"me {MYSTATE}",
    r"phase \d",
    rf"after {NUM}s",
    rf"every {NUM}s",
]
COND = re.compile(rf"(?:not )?(?:{'|'.join(f'(?:{t})' for t in TESTS)})")


def parses(when):
    if when == "always":
        return True
    conds = when.split(" & ")
    return len(conds) <= 3 and all(COND.fullmatch(c) for c in conds)


def main():
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else \
        ROOT / "docs" / "specs" / "trigger-conditions.tsv"
    lines = path.read_text().splitlines()
    header, rows = lines[0].split("\t"), [line.split("\t") for line in lines[1:]]
    col = {name: i for i, name in enumerate(header)}
    failures = []
    for n, row in enumerate(rows, 2):
        verdict, when = row[col["verdict"]], row[col["when"]]
        if len(row) != len(header):
            failures.append(f"{n}: {len(row)} columns, want {len(header)}")
        elif verdict not in VERDICTS:
            failures.append(f"{n}: unknown verdict {verdict!r}")
        elif verdict == "fits" and not parses(when):
            failures.append(f"{n}: {row[col['class']]}: doesn't parse: {when!r}")
        elif verdict == "always" and when != "always":
            failures.append(f"{n}: {row[col['class']]}: `always` row says {when!r}")
    for failure in failures:
        print(failure)
    print(f"{len(rows)} rows, {len(failures)} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
