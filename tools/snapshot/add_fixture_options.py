"""Writes the module's options into the eval snapshots, so each case shows the screen a live bot
sees (review 2026-11-17, finding 5; docs/specs/options.md, "One code path, live and offline").

Each case's <case>.facts.json (what the module knows beside the screen: NPC flags, the Explore
spots and Go to places at that spot, the bags and skills) is written by the module itself:
`--capture` starts the test server with LlmBrain.TestFactsQueries, which runs the live code
(test/facts_dump.cc) on every eval snapshot. The case's `scene` states what its snapshot doesn't
show: `team`, `has_food`, `has_drink`, `herbalism`, `mining`.

Then, for each case: renders the screen before and after (with `harness decide --dry-run`), writes
`options` from `out/common/fixture_options` (the module's AddOptions on snapshot + facts), and
moves the case's option numbers (`first_options`, `forbid_options`) to the new numbering by the
option's text.

Usage: python3 -I tools/snapshot/add_fixture_options.py [--capture | --check]
  --capture: re-capture every case's facts with the isolated test server first (about a minute;
             the play server keeps running, as with `wlb test e2e`).
  --check:   exit 1 if any eval snapshot's options differ from what the module would offer.
"""

import json
import pathlib
import re
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
EVAL = ROOT / "harness" / "testdata" / "eval"
SCREENS = ROOT / "harness" / "testdata" / "screens"
HARNESS = ROOT / "out" / "common" / "harness"
TOOL = ROOT / "out" / "common" / "fixture_options"
QUERIES_DIR = ROOT / "var" / "test-facts"
OPTION_RE = re.compile(r"^\s+(\d+)\. ([^:\n]+)", re.M)
SCENE_DEFAULTS = {"team": "HORDE", "has_food": True, "has_drink": True, "herbalism": 0,
                  "mining": 0}
CAPTURE_TIMEOUT_S = 120


def scene_of(case):
    return {**SCENE_DEFAULTS, **case.get("scene", {})}


def screen_fixture_stems():
    return [p.name.removesuffix(".snapshot.json") for p in sorted(SCREENS.glob("*.snapshot.json"))]


def capture(cases, screens_only=False):
    """Has the module write every case's facts (test/facts_dump.cc)."""
    sys.path.insert(0, str(ROOT / "tools"))
    from wlblib import testing  # noqa: E402 (needs tools/ on the path)

    shutil.rmtree(QUERIES_DIR, ignore_errors=True)
    QUERIES_DIR.mkdir(parents=True)
    targets = []
    if not screens_only:
        for case in cases:
            targets.append((
                EVAL / f"{case['name']}.snapshot.json",
                QUERIES_DIR / f"eval_{case['name']}.facts.json",
                EVAL / f"{case['name']}.facts.json",
                scene_of(case),
            ))
    for stem in screen_fixture_stems():
        targets.append((
            SCREENS / f"{stem}.snapshot.json",
            QUERIES_DIR / f"screen_{stem}.facts.json",
            SCREENS / f"{stem}.facts.json",
            SCENE_DEFAULTS,
        ))
    lines = []
    for snap_p, out_p, _, s in targets:
        lines.append("\t".join([
            str(snap_p), str(out_p),
            s["team"], "1" if s["has_food"] else "0", "1" if s["has_drink"] else "0",
            str(s["herbalism"]), str(s["mining"])]))
    (QUERIES_DIR / "queries.tsv").write_text("\n".join(lines) + "\n")
    want = [out_p for _, out_p, _, _ in targets]
    with testing.Stack("facts", bot="Loada") as stack:
        end = time.time() + CAPTURE_TIMEOUT_S
        while not all(p.exists() for p in want):
            if "facts dump failed" in stack.server_log() or time.time() > end:
                missing = [p.name for p in want if not p.exists()]
                raise SystemExit(f"the module wrote no facts for {', '.join(missing[:5])}...: "
                                 "see var/logs/test/Server.log ('facts dump')")
            time.sleep(0.5)
    for _, src, dst, _ in targets:
        shutil.copyfile(src, dst)
    print(f"captured facts for {len(want)} snapshots")


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
    if "--capture" in sys.argv[1:]:
        capture(cases, screens_only=False)
    elif "--capture-screens" in sys.argv[1:]:
        capture(cases, screens_only=True)
    stale, rejudge = [], []
    for case in cases:
        path = EVAL / f"{case['name']}.snapshot.json"
        if not (EVAL / f"{case['name']}.facts.json").exists():
            raise SystemExit(f"{case['name']}: no facts.json (run with --capture)")
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
    for stem in screen_fixture_stems():
        path = SCREENS / f"{stem}.snapshot.json"
        if not (SCREENS / f"{stem}.facts.json").exists():
            raise SystemExit(f"screens/{stem}: no facts.json (run with --capture-screens)")
        snap = json.loads(path.read_text())
        want = module_options(path)
        if snap.get("options") == want:
            continue
        if check:
            stale.append(f"screens/{stem}")
            continue
        snap["options"] = want
        path.write_text(json.dumps(snap, indent=2, ensure_ascii=False) + "\n")
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
