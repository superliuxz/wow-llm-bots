"""Keep while walking: the walk carries on and arrives (review of f40b0b6, finding 1).

A Choose of "Keep doing what I'm doing" must leave the body's current step alone. It used to
reset the walk: on map 1 the walk then failed ("you changed map"), on map 0 the bot walked to
(0, 0, 0).
"""

import math

from wlblib.testing import expect

NAME = "keep"
DESCRIPTION = "Explore, then Keep: the walk carries on and is reported done on arrival"
TEST_CONFIG = "choose"  # One level-1 priest in the Valley of Trials (map 1), grinding off.
BOT = "Loada"
ARRIVED_WITHIN_S = 90
MOVED_AT_LEAST_YD = 20


def run(stack):
    t = stack.play(["Explore", "Keep"], seconds=ARRIVED_WITHIN_S + 30,
                   tail_s=ARRIVED_WITHIN_S)
    expect(not t["closed"], "the module closed the connection")
    expect(not t["never_offered"], f"never offered: {t['never_offered']}")
    explore, keep = t["commands"]
    failed = [e["event"]["error"] for e in t["events"]
              if e["event"].get("error", {}).get("command_id") in (explore["command_id"],
                                                                     keep["command_id"])]
    expect(not failed, f"the walk failed after Keep: {failed}")
    before = next(s for s in t["snapshots"] if s["seq"] == explore["seq"])
    done = next((s for s in t["snapshots"] if s["t"] > keep["t"]
                 and s["done_command_id"] == explore["command_id"]), None)
    expect(done, f"the walk wasn't reported done within {ARRIVED_WITHIN_S} s after Keep")
    moved = math.dist(before["pos"], done["pos"])
    expect(moved >= MOVED_AT_LEAST_YD, f"done after moving only {moved:.0f} yd")
    return (f"Keep {keep['t'] - explore['t']:.1f} s into the walk; arrived "
            f"{done['t'] - explore['t']:.1f} s after Explore, {moved:.0f} yd walked")
