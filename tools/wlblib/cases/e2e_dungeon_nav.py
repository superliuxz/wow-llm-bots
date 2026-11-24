"""Live dungeon navigation across stair-heavy and seam-prone Classic/TBC/WotLK entrances.

Verifies that multi-room dungeon clustering (`ClusterDungeonHostiles`) and the
navmesh seam-bridge (`FindDungeonSeamHop` + `TickLongWalk`) expose concrete
`Explore` options at disconnected/steep entrance stairs and actually advance
the bot down the stairs without returning `"stuck"`.
"""

import math
import time

from wlblib import server
from wlblib.common import TEST
from wlblib.testing import expect

NAME = "dungeon_nav"
DESCRIPTION = "Explore advances across stair-heavy Classic/TBC/WotLK dungeon entrances"
TEST_CONFIG = "dungeon_nav"
BOT = "Loada"
MOVED_AT_LEAST_YD = 10.0

# Verified `areatrigger_teleport` entrance coordinates for stair-heavy /
# long-corridor / seam-prone dungeons across Classic, TBC, and WotLK.
DUNGEONS = [
    ("Shadowfang Keep", 33, -229.13, 2109.18, 76.89),
    ("Magisters' Terrace", 585, 7.09, -0.45, -2.80),
    ("Utgarde Keep", 574, 153.79, -86.55, 12.55),
    ("The Nexus", 576, 145.87, -10.55, -16.64),
    ("Azjol-Nerub", 601, 413.31, 795.97, 831.35),
    ("Gundrak", 604, 1891.84, 832.17, 176.67),
    ("Ahn'kahet", 619, 333.35, -1109.94, 69.77),
]


def run(stack):
    server.console(f"llmtest gear {BOT} 80", inst=TEST)
    time.sleep(1.5)

    summaries = []
    for name, map_id, x, y, z in DUNGEONS:
        server.console(f"llmtest tele {BOT} {map_id} {x} {y} {z} 0", inst=TEST)
        time.sleep(2.0)

        t = stack.play(["Explore"], seconds=25, tail_s=10)
        expect(not t["closed"], f"{name}: the module closed the connection")
        expect(
            not t["never_offered"],
            f"{name}: Explore never offered (options seen: "
            f"{sorted({o for s in t['snapshots'] for o in s['options']})})",
        )
        (explore,) = t["commands"]
        # Reaching a hostile pack naturally interrupts a long walk with
        # "walk interrupted: you are in combat"; any other error (e.g. "stuck")
        # is a navigation failure.
        failed = [
            e["event"]["error"]
            for e in t["events"]
            if e["event"].get("error", {}).get("command_id") == explore["command_id"]
            and e["event"].get("error", {}).get("text", "")
            != "walk interrupted: you are in combat"
        ]
        expect(not failed, f"{name}: Explore '{explore['text']}' failed: {failed}")

        before = next(s for s in t["snapshots"] if s["seq"] == explore["seq"])
        after = [s for s in t["snapshots"] if s["seq"] >= explore["seq"]]
        max_moved = max(
            (math.dist(before["pos"], s["pos"]) for s in after),
            default=0.0,
        )
        expect(
            max_moved >= MOVED_AT_LEAST_YD,
            f"{name}: moved only {max_moved:.1f} yd after '{explore['text']}' "
            f"(start={before['pos']}, end={after[-1]['pos']})",
        )
        summaries.append(f"{name}={max_moved:.0f}yd")

    return ", ".join(summaries)
