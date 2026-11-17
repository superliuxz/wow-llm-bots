"""Writes the module's options into the eval snapshots, so each case shows the screen a live bot
sees (review 2026-11-17, finding 5: without `options` the harness invented Explore / Rest /
Town / Talk, which live bots never get).

For each case in harness/testdata/eval/cases.json: renders the screen before and after (with
`harness decide --dry-run`), writes `options` from `out/common/fixture_options` (the module's
AddOptions), and moves the case's option numbers (`first_options`, `forbid_options`) to the new
numbering by the option's text. Numbers whose option no longer exists are dropped; a case left
with an empty `first_options` is reported, to be judged again by hand. Cases marked `planned`
(they test options the module doesn't offer yet: Talk, Rest, Explore, Go to town) are skipped.

Usage: python3 -I tools/snapshot/add_fixture_options.py [--check]
  --check: exit 1 if any eval snapshot's options differ from what the module would offer.
"""

import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
EVAL = ROOT / "harness" / "testdata" / "eval"
HARNESS = ROOT / "out" / "common" / "harness"
TOOL = ROOT / "out" / "common" / "fixture_options"
OPTION_RE = re.compile(r"^\s+(\d+)\. ([^:\n]+)", re.M)


def screen_options(path):
    out = subprocess.run([str(HARNESS), "decide", str(path), "--dry-run"],
                         capture_output=True, text=True, check=True).stdout
    return {int(n): text.strip() for n, text in OPTION_RE.findall(out)}


def module_options(path):
    out = subprocess.run([str(TOOL), str(path)], capture_output=True, text=True,
                         check=True).stdout
    return json.loads(out).get("options", [])


def main():
    check = "--check" in sys.argv[1:]
    cases = json.loads((EVAL / "cases.json").read_text())
    stale, rejudge = [], []
    for case in cases:
        if case.get("planned"):
            continue  # Needs options the module doesn't offer yet: keeps the invented ones.
        path = EVAL / f"{case['name']}.snapshot.json"
        snap = json.loads(path.read_text())
        want = module_options(path)
        if snap.get("options") == want:
            continue
        if check:
            stale.append(case["name"])
            continue
        before = screen_options(path)
        snap["options"] = want
        path.write_text(json.dumps(snap, indent=2, ensure_ascii=False) + "\n")
        after = {text: n for n, text in screen_options(path).items()}
        for key in ("first_options", "forbid_options"):
            if key not in case:
                continue
            moved = [after[before[n]] for n in case[key] if before.get(n) in after]
            dropped = [before.get(n, f"#{n}") for n in case[key] if before.get(n) not in after]
            case[key] = moved
            if dropped:
                print(f"{case['name']}: {key} dropped {dropped}")
            if key == "first_options" and not moved:
                rejudge.append(case["name"])
    if check:
        if stale:
            print("options out of date (run tools/snapshot/add_fixture_options.py):",
                  ", ".join(stale))
            return 1
        return 0
    (EVAL / "cases.json").write_text(json.dumps(cases, indent=2, ensure_ascii=False) + "\n")
    if rejudge:
        print("judge again (no first option left):", ", ".join(rejudge))
    return 0


if __name__ == "__main__":
    sys.exit(main())
