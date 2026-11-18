"""A bot told to Explore walks to the spot and the module reports the step done.

The stepped mmap walk of docs/plans/options.md: Explore is offered from the module's facts, a
Choose starts the walk, the module steps it ~70 yd at a time and sets `done_command_id` on
arrival. Talk and Rest have no live case yet (docs/TODO.md, "Loose ends").
"""

import math

from wlblib.testing import expect

NAME = "walk"
DESCRIPTION = "Explore: the bot walks to the spot and the step is reported done"
TEST_CONFIG = "choose"  # One level-1 priest in the Valley of Trials, its own grinding off.
BOT = "Loada"
# Spots are 25 yd to a few hundred yd away; a level-1 walks ~7 yd/s.
ARRIVED_WITHIN_S = 90
MOVED_AT_LEAST_YD = 20


def run(stack):
    t = stack.play(["Explore"], seconds=ARRIVED_WITHIN_S + 30, tail_s=ARRIVED_WITHIN_S)
    expect(not t["closed"], "the module closed the connection")
    expect(not t["never_offered"],
           f"Explore never offered (options seen: "
           f"{sorted({o for s in t['snapshots'] for o in s['options']})})")
    (explore,) = t["commands"]
    failed = [e["event"]["error"] for e in t["events"]
              if e["event"].get("error", {}).get("command_id") == explore["command_id"]]
    expect(not failed, f"the walk failed: {failed}")

    before = next(s for s in t["snapshots"] if s["seq"] == explore["seq"])
    done = next((s for s in t["snapshots"]
                 if s["done_command_id"] == explore["command_id"]), None)
    expect(done, f"no done signal within {ARRIVED_WITHIN_S} s of '{explore['text']}'")
    moved = math.dist(before["pos"], done["pos"])
    expect(moved >= MOVED_AT_LEAST_YD, f"done after moving only {moved:.0f} yd")
    return (f"'{explore['text']}': done {done['t'] - explore['t']:.1f} s later, "
            f"{moved:.0f} yd walked")
